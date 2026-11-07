#define DUCKDB_EXTENSION_MAIN

#include "cronjob_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "ccronexpr.h"
#include "query_farm_telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#ifndef CRONJOB_VERSION
#define CRONJOB_VERSION "0.0.1"
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Cron expressions
//===--------------------------------------------------------------------===//

// ccronexpr evaluates every expression in UTC. On MinGW it falls back to the
// non-reentrant gmtime(), so all calls into the library are serialized; they
// are cheap and infrequent (once per job run and once per cron() call).
static std::mutex &CronLibraryMutex() {
	static std::mutex mutex;
	return mutex;
}

// The longest expression ccronexpr accepts.
static constexpr idx_t MAX_SCHEDULE_LENGTH = 255;

static cron_expr ParseSchedule(const string &schedule) {
	if (schedule.size() > MAX_SCHEDULE_LENGTH) {
		throw InvalidInputException("Invalid cron schedule: expressions are limited to %llu characters",
		                            MAX_SCHEDULE_LENGTH);
	}
	if (schedule.find('\0') != string::npos) {
		throw InvalidInputException("Invalid cron schedule: the expression contains a NUL character");
	}
	cron_expr expr;
	memset(&expr, 0, sizeof(expr));
	const char *err = nullptr;
	{
		std::lock_guard<std::mutex> guard(CronLibraryMutex());
		cron_parse_expr(schedule.c_str(), &expr, &err);
	}
	if (err) {
		throw InvalidInputException("Invalid cron schedule '%s': %s (expected six fields: second minute hour "
		                            "day-of-month month day-of-week)",
		                            schedule, err);
	}
	return expr;
}

// The first fire time strictly after `after` (seconds since the epoch, UTC), or
// -1 if the expression can never fire (e.g. February 30th).
static time_t NextFireTime(cron_expr &expr, time_t after) {
	std::lock_guard<std::mutex> guard(CronLibraryMutex());
	return cron_next(&expr, after);
}

static constexpr time_t NO_NEXT_RUN = static_cast<time_t>(-1);

static int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(
	           std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

static time_t NowSeconds() {
	return static_cast<time_t>(NowMicros() / Interval::MICROS_PER_SEC);
}

static Value MicrosToTimestampTZ(int64_t micros) {
	return Value::TIMESTAMPTZ(timestamp_tz_t(Timestamp::FromEpochMicroSeconds(micros)));
}

//===--------------------------------------------------------------------===//
// Scheduler state
//===--------------------------------------------------------------------===//

struct CronRun {
	int64_t started_us;
	int64_t finished_us;
	bool success;
	string message;
};

struct CronTask {
	idx_t seq = 0;
	string id;
	string query;
	string schedule;
	cron_expr expr;
	bool active = true;
	//! Seconds since the epoch (UTC), NO_NEXT_RUN when the job is inactive.
	time_t next_run = NO_NEXT_RUN;
	int64_t created_us = 0;
	idx_t run_count = 0;
	idx_t error_count = 0;
	std::deque<CronRun> history;
};

//! A copy of one job, taken under the lock, for cron_jobs().
struct CronTaskSnapshot {
	string id;
	string query;
	string schedule;
	bool active;
	time_t next_run;
	int64_t created_us;
	idx_t run_count;
	idx_t error_count;
	bool has_last_run;
	CronRun last_run;
};

//! A copy of one history entry, taken under the lock, for cron_job_history().
struct CronHistorySnapshot {
	string id;
	CronRun run;
};

// State shared between a CronScheduler and its worker thread. The worker holds
// its own reference, so the state stays valid even in the rare case where the
// worker itself ends up destroying the database (and with it the CronScheduler)
// by releasing the last reference to it after running a job.
struct CronSchedulerState {
	std::mutex mutex;
	std::condition_variable condition;
	bool should_stop = false;
	idx_t task_counter = 0;
	//! Keyed by sequence number, so listings come out in creation order.
	std::map<idx_t, unique_ptr<CronTask>> tasks;
	// Never a strong reference: the scheduler must not keep the database alive.
	weak_ptr<DatabaseInstance> db;
};

// One scheduler per database, owned by the database's ObjectCache.
//
// ~DatabaseInstance destroys the ObjectCache while every thread is still alive,
// and the destructor below stops and joins the worker there, so jobs never
// outlive their database and loading the extension into a second database
// never touches the first one's jobs.
//
// Locking: the state mutex only guards the job table. Jobs run with the mutex
// released, so a slow job never blocks cron()/cron_jobs()/cron_delete() or
// shutdown, and a job may itself call those functions.
class CronScheduler : public ObjectCacheEntry {
public:
	static constexpr const char *CACHE_KEY = "cronjob_scheduler";
	static constexpr idx_t MAX_EXECUTION_HISTORY = 100;

	explicit CronScheduler(DatabaseInstance &db) : state(make_shared_ptr<CronSchedulerState>()) {
		state->db = db.shared_from_this();
		auto worker_state = state;
		scheduler_thread = std::thread([worker_state]() { RunScheduler(worker_state); });
	}

	~CronScheduler() override {
		Stop();
	}

	static string ObjectType() {
		return "cronjob_scheduler";
	}

	string GetObjectType() override {
		return ObjectType();
	}

	// Not evictable: the scheduler must live exactly as long as the database.
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

	string AddTask(const string &query, const string &schedule) {
		auto task = make_uniq<CronTask>();
		task->query = query;
		task->schedule = schedule;
		task->expr = ParseSchedule(schedule);
		task->created_us = NowMicros();
		task->next_run = NextFireTime(task->expr, NowSeconds());
		if (task->next_run == NO_NEXT_RUN) {
			throw InvalidInputException("Invalid cron schedule '%s': the expression never matches a real date",
			                            schedule);
		}

		string task_id;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			task->seq = state->task_counter++;
			task_id = "task_" + std::to_string(task->seq);
			task->id = task_id;
			state->tasks[task->seq] = std::move(task);
		}
		state->condition.notify_one();
		return task_id;
	}

	bool RemoveTask(const string &id) {
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			auto it = FindTask(*state, id);
			if (it == state->tasks.end()) {
				throw InvalidInputException("Cron job '%s' does not exist", id);
			}
			state->tasks.erase(it);
		}
		state->condition.notify_one();
		return true;
	}

	vector<CronTaskSnapshot> SnapshotTasks() {
		vector<CronTaskSnapshot> result;
		std::lock_guard<std::mutex> lock(state->mutex);
		result.reserve(state->tasks.size());
		for (auto &entry : state->tasks) {
			auto &task = *entry.second;
			CronTaskSnapshot snap;
			snap.id = task.id;
			snap.query = task.query;
			snap.schedule = task.schedule;
			snap.active = task.active;
			snap.next_run = task.next_run;
			snap.created_us = task.created_us;
			snap.run_count = task.run_count;
			snap.error_count = task.error_count;
			snap.has_last_run = !task.history.empty();
			if (snap.has_last_run) {
				snap.last_run = task.history.back();
			}
			result.push_back(std::move(snap));
		}
		return result;
	}

	vector<CronHistorySnapshot> SnapshotHistory() {
		vector<CronHistorySnapshot> result;
		std::lock_guard<std::mutex> lock(state->mutex);
		for (auto &entry : state->tasks) {
			auto &task = *entry.second;
			for (auto &run : task.history) {
				result.push_back(CronHistorySnapshot {task.id, run});
			}
		}
		return result;
	}

private:
	shared_ptr<CronSchedulerState> state;
	std::thread scheduler_thread;

	static std::map<idx_t, unique_ptr<CronTask>>::iterator FindTask(CronSchedulerState &s, const string &id) {
		for (auto it = s.tasks.begin(); it != s.tasks.end(); ++it) {
			if (it->second->id == id) {
				return it;
			}
		}
		return s.tasks.end();
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->should_stop = true;
		}
		state->condition.notify_all();
		if (!scheduler_thread.joinable()) {
			return;
		}
		if (scheduler_thread.get_id() == std::this_thread::get_id()) {
			// The worker released the last reference to the database and is
			// running its destructor. It cannot join itself; it only touches the
			// shared state from here on and exits as soon as it sees should_stop.
			scheduler_thread.detach();
		} else {
			scheduler_thread.join();
		}
	}

	static void RunScheduler(shared_ptr<CronSchedulerState> s) {
		std::unique_lock<std::mutex> lock(s->mutex);
		while (!s->should_stop) {
			auto now = NowSeconds();

			// Collect the due jobs; they run with the lock released.
			vector<std::pair<idx_t, string>> due;
			for (auto &entry : s->tasks) {
				auto &task = *entry.second;
				if (task.active && task.next_run != NO_NEXT_RUN && task.next_run <= now) {
					due.emplace_back(task.seq, task.query);
				}
			}

			if (!due.empty()) {
				for (auto &job : due) {
					if (s->should_stop) {
						break;
					}
					lock.unlock();
					CronRun run;
					run.started_us = NowMicros();
					run.success = ExecuteQuery(s, job.second, run.message);
					run.finished_us = NowMicros();
					lock.lock();
					// The job may have been deleted while it was running.
					auto it = s->tasks.find(job.first);
					if (it != s->tasks.end()) {
						RecordRun(*it->second, std::move(run));
					}
				}
				continue;
			}

			auto next_wake = std::chrono::system_clock::now() + std::chrono::hours(24);
			for (auto &entry : s->tasks) {
				auto &task = *entry.second;
				if (task.active && task.next_run != NO_NEXT_RUN) {
					next_wake = std::min(next_wake, std::chrono::system_clock::from_time_t(task.next_run));
				}
			}
			s->condition.wait_until(lock, next_wake);
		}
	}

	static bool ExecuteQuery(const shared_ptr<CronSchedulerState> &s, const string &query, string &message) {
		try {
			auto db = s->db.lock();
			if (!db) {
				message = "Error: database is closed";
				return false;
			}
			bool success;
			{
				Connection conn(*db);
				auto query_result = conn.Query(query);
				if (query_result->HasError()) {
					message = "Error: " + query_result->GetError();
					success = false;
				} else {
					message = "Success";
					success = true;
				}
			}
			// If this was the last reference, the database (and the CronScheduler
			// that owns this thread) is destroyed right here; see Stop().
			db.reset();
			return success;
		} catch (const std::exception &e) {
			message = "Error: " + string(e.what());
			return false;
		} catch (...) {
			message = "Error: unknown exception";
			return false;
		}
	}

	static void RecordRun(CronTask &task, CronRun run) {
		task.run_count++;
		if (!run.success) {
			task.error_count++;
		}
		// Schedule from the end of the run: fire times missed while the job was
		// running are skipped rather than replayed back to back.
		auto after = std::max<time_t>(NowSeconds(), static_cast<time_t>(run.finished_us / Interval::MICROS_PER_SEC));
		task.next_run = NextFireTime(task.expr, after);
		if (task.next_run == NO_NEXT_RUN) {
			// Defensive: AddTask rejects expressions that never fire, but if the
			// library ever runs out of future dates, stop instead of spinning.
			task.active = false;
		}
		task.history.push_back(std::move(run));
		while (task.history.size() > MAX_EXECUTION_HISTORY) {
			task.history.pop_front();
		}
	}
};

static shared_ptr<CronScheduler> GetScheduler(ClientContext &context) {
	auto scheduler = ObjectCache::GetObjectCache(context).Get<CronScheduler>(CronScheduler::CACHE_KEY);
	if (!scheduler) {
		throw InternalException("Cron scheduler not initialized");
	}
	return scheduler;
}

//===--------------------------------------------------------------------===//
// cron(query, schedule) -> job id
//===--------------------------------------------------------------------===//

static void ValidateQuery(ClientContext &context, const string &query) {
	Parser parser(context);
	try {
		parser.ParseQuery(query);
	} catch (const std::exception &e) {
		ErrorData error(e);
		throw InvalidInputException("Invalid cron query: %s", error.RawMessage());
	}
	if (parser.statements.empty()) {
		throw InvalidInputException("Invalid cron query: the query contains no SQL statement");
	}
}

static void CronScalarFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto scheduler = GetScheduler(context);
	// Every row schedules its own job, even when the arguments are constant
	// (the executor would otherwise evaluate a constant input only once).
	args.Flatten();
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, args.size(), [&](string_t query_p, string_t schedule_p) {
		    auto query = query_p.GetString();
		    ValidateQuery(context, query);
		    auto task_id = scheduler->AddTask(query, schedule_p.GetString());
		    return StringVector::AddString(result, task_id);
	    });
}

//===--------------------------------------------------------------------===//
// cron_delete(job_id) -> true
//===--------------------------------------------------------------------===//

static void CronDeleteFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto scheduler = GetScheduler(state.GetContext());
	args.Flatten();
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(),
	                                       [&](string_t task_id) { return scheduler->RemoveTask(task_id.GetString()); });
}

//===--------------------------------------------------------------------===//
// cron_jobs() / cron_job_history()
//===--------------------------------------------------------------------===//

struct CronJobsState : public GlobalTableFunctionState {
	vector<CronTaskSnapshot> rows;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> CronJobsBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"job_id",   "query",       "schedule",  "next_run",    "status",
	         "last_run", "last_result", "run_count", "error_count", "created_at"};
	return_types = {LogicalType::VARCHAR,      LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ,
	                LogicalType::VARCHAR,      LogicalType::TIMESTAMP_TZ, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::BIGINT,      LogicalType::TIMESTAMP_TZ};
	return make_uniq<TableFunctionData>();
}

static unique_ptr<GlobalTableFunctionState> CronJobsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<CronJobsState>();
	result->rows = GetScheduler(context)->SnapshotTasks();
	return std::move(result);
}

static void CronJobsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<CronJobsState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		output.data[0].SetValue(count, Value(row.id));
		output.data[1].SetValue(count, Value(row.query));
		output.data[2].SetValue(count, Value(row.schedule));
		output.data[3].SetValue(count, row.active && row.next_run != NO_NEXT_RUN
		                                   ? MicrosToTimestampTZ(int64_t(row.next_run) * Interval::MICROS_PER_SEC)
		                                   : Value(LogicalType::TIMESTAMP_TZ));
		output.data[4].SetValue(count, Value(row.active ? "Active" : "Inactive"));
		if (row.has_last_run) {
			output.data[5].SetValue(count, MicrosToTimestampTZ(row.last_run.started_us));
			output.data[6].SetValue(count, Value(row.last_run.message));
		} else {
			output.data[5].SetValue(count, Value(LogicalType::TIMESTAMP_TZ));
			output.data[6].SetValue(count, Value(LogicalType::VARCHAR));
		}
		output.data[7].SetValue(count, Value::BIGINT(int64_t(row.run_count)));
		output.data[8].SetValue(count, Value::BIGINT(int64_t(row.error_count)));
		output.data[9].SetValue(count, MicrosToTimestampTZ(row.created_us));
		count++;
	}
	output.SetChildCardinality(count);
}

struct CronHistoryState : public GlobalTableFunctionState {
	vector<CronHistorySnapshot> rows;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> CronHistoryBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {"job_id", "started_at", "finished_at", "success", "result"};
	return_types = {LogicalType::VARCHAR, LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ, LogicalType::BOOLEAN,
	                LogicalType::VARCHAR};
	return make_uniq<TableFunctionData>();
}

static unique_ptr<GlobalTableFunctionState> CronHistoryInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<CronHistoryState>();
	result->rows = GetScheduler(context)->SnapshotHistory();
	return std::move(result);
}

static void CronHistoryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<CronHistoryState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		output.data[0].SetValue(count, Value(row.id));
		output.data[1].SetValue(count, MicrosToTimestampTZ(row.run.started_us));
		output.data[2].SetValue(count, MicrosToTimestampTZ(row.run.finished_us));
		output.data[3].SetValue(count, Value::BOOLEAN(row.run.success));
		output.data[4].SetValue(count, Value(row.run.message));
		count++;
	}
	output.SetChildCardinality(count);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

// Declares a positional-only parameter with a real name, so duckdb_functions() reports it instead of col<N>.
static FunctionParameter Positional(const char *name, LogicalType type) {
	return FunctionParameter(name, std::move(type), {}, FunctionParameterKind::POSITIONAL_ONLY);
}

static FunctionDescription Describe(string description, vector<string> examples) {
	FunctionDescription desc;
	desc.description = std::move(description);
	desc.examples = std::move(examples);
	desc.categories = {"scheduling"};
	return desc;
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db_instance = loader.GetDatabaseInstance();
	// One scheduler per database; loading the extension again reuses it.
	auto scheduler = db_instance.GetObjectCache().GetOrCreate<CronScheduler>(CronScheduler::CACHE_KEY, db_instance);
	if (!scheduler) {
		throw InternalException("Failed to initialize cron scheduler");
	}

	// cron() and cron_delete() change scheduler state, so they are VOLATILE: the
	// optimizer must never constant-fold them (which would schedule a job while
	// merely planning, e.g. under EXPLAIN).
	{
		ScalarFunction cron_func("cron",
		                         {Positional("query", LogicalType::VARCHAR), Positional("schedule", LogicalType::VARCHAR)},
		                         LogicalType::VARCHAR, CronScalarFunction);
		cron_func.SetFallible();
		cron_func.SetVolatile();
		CreateScalarFunctionInfo info(std::move(cron_func));
		info.descriptions.push_back(Describe(
		    "Schedules a SQL query to run in the background on a six-field cron schedule "
		    "(second minute hour day-of-month month day-of-week, evaluated in UTC) and returns the new job's id. "
		    "The query is checked for syntax errors and the schedule must match at least one real date. "
		    "Returns NULL (and schedules nothing) when either argument is NULL.",
		    {"cron('INSERT INTO heartbeat VALUES (now())', '*/10 * * * * *')",
		     "cron('CHECKPOINT', '0 0 3 * * *')"}));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	}

	{
		ScalarFunction cron_delete_func("cron_delete", {Positional("job_id", LogicalType::VARCHAR)},
		                                LogicalType::BOOLEAN, CronDeleteFunction);
		cron_delete_func.SetFallible();
		cron_delete_func.SetVolatile();
		CreateScalarFunctionInfo info(std::move(cron_delete_func));
		info.descriptions.push_back(Describe("Deletes a scheduled cron job by id and returns true. A run that is "
		                                     "already in progress finishes; no further runs start. Raises an error if "
		                                     "the job does not exist.",
		                                     {"cron_delete('task_0')"}));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	}

	{
		TableFunction cron_jobs_func("cron_jobs", {}, CronJobsFunction, CronJobsBind, CronJobsInit);
		CreateTableFunctionInfo info(std::move(cron_jobs_func));
		info.descriptions.push_back(
		    Describe("Lists the cron jobs scheduled in this database with their query, schedule, next and last run "
		             "time (TIMESTAMPTZ), status, last result, run and error counts and creation time.",
		             {"SELECT * FROM cron_jobs()", "SELECT job_id, next_run, last_result FROM cron_jobs()"}));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	}

	{
		TableFunction cron_history_func("cron_job_history", {}, CronHistoryFunction, CronHistoryBind,
		                                CronHistoryInit);
		CreateTableFunctionInfo info(std::move(cron_history_func));
		info.descriptions.push_back(
		    Describe("Lists the most recent runs (up to 100 per job) of every scheduled cron job, with start and "
		             "finish time, whether the run succeeded and its result or error message.",
		             {"SELECT * FROM cron_job_history()",
		              "SELECT job_id, started_at, result FROM cron_job_history() WHERE NOT success"}));
		info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	}

	QueryFarmSendTelemetry(loader, "cronjob", CRONJOB_VERSION);
}

void CronjobExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string CronjobExtension::Name() {
	return "cronjob";
}

std::string CronjobExtension::Version() const {
	return CRONJOB_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cronjob, loader) {
	duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *cronjob_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}
