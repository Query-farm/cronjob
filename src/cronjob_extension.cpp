#define DUCKDB_EXTENSION_MAIN

#include "cronjob_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "ccronexpr.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include "query_farm_telemetry.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/storage/object_cache.hpp"
#include <condition_variable>
#include <unordered_map>

namespace duckdb
{

    struct CronTask
    {
        string id;
        string query;
        string schedule;
        bool active = true;
        std::chrono::system_clock::time_point next_run;
        std::vector<std::pair<std::chrono::system_clock::time_point, string>> execution_history;
        cron_expr expr;
    };

    // State shared between a CronScheduler and its worker thread. The worker
    // holds its own reference, so the state stays valid even in the rare case
    // where the worker itself ends up destroying the database (and with it the
    // CronScheduler) by releasing the last reference to it after running a task.
    struct CronSchedulerState
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool should_stop = false;
        size_t task_counter = 0;
        std::unordered_map<string, unique_ptr<CronTask>> tasks;
        // Never a strong reference: the scheduler must not keep the database alive.
        weak_ptr<DatabaseInstance> db;
    };

    // One scheduler per database, owned by the database's ObjectCache.
    //
    // The scheduler used to be a process-wide static whose worker thread was only
    // stopped by the static destructor at process exit. That is too late: on
    // Windows/MinGW the other threads have already been terminated by then, and
    // tearing down the condition variable the dead worker was blocked on hangs
    // forever (the unittest process never exits). It also left the worker holding
    // a dangling DatabaseInstance reference once the database was closed.
    //
    // Living in the ObjectCache ties the worker to the database instead:
    // ~DatabaseInstance destroys the cache while every thread is still alive, and
    // the destructor below stops and joins the worker there.
    class CronScheduler : public ObjectCacheEntry
    {
    public:
        static constexpr const char *CACHE_KEY = "cronjob_scheduler";

        explicit CronScheduler(DatabaseInstance &db) : state(make_shared_ptr<CronSchedulerState>())
        {
            state->db = db.shared_from_this();
            auto worker_state = state;
            scheduler_thread = std::thread([worker_state]()
                                           { RunScheduler(worker_state); });
        }

        ~CronScheduler() override
        {
            Stop();
        }

        static string ObjectType()
        {
            return "cronjob_scheduler";
        }

        string GetObjectType() override
        {
            return ObjectType();
        }

        // Not evictable: the scheduler must live exactly as long as the database.
        optional_idx GetEstimatedCacheMemory() const override
        {
            return optional_idx();
        }

        string AddTask(const string &query, const string &schedule)
        {
            // Parse cron expression
            cron_expr expr;
            const char *err = NULL;
            cron_parse_expr(schedule.c_str(), &expr, &err);
            if (err)
            {
                throw InvalidInputException("Invalid cron expression: %s", err);
            }

            auto task = make_uniq<CronTask>();
            task->query = query;
            task->schedule = schedule;
            task->expr = expr;
            task->next_run = CalculateNextRun(task->expr);

            string task_id;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                task_id = "task_" + std::to_string(state->task_counter++);
                task->id = task_id;
                state->tasks[task_id] = std::move(task);
            }
            state->condition.notify_one();
            return task_id;
        }

        void GetTasksStatus(vector<Value> &job_ids, vector<Value> &queries, vector<Value> &schedules,
                            vector<Value> &next_runs, vector<Value> &statuses, vector<Value> &last_runs,
                            vector<Value> &last_results)
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            for (auto &task_pair : state->tasks)
            {
                auto &task = task_pair.second;

                job_ids.push_back(Value(task->id));
                queries.push_back(Value(task->query));
                schedules.push_back(Value(task->schedule));
                next_runs.push_back(Value(TimeToString(task->next_run)));
                statuses.push_back(Value(task->active ? "Active" : "Inactive"));

                if (!task->execution_history.empty())
                {
                    auto &last_execution = task->execution_history.back();
                    last_runs.push_back(Value(TimeToString(last_execution.first)));
                    last_results.push_back(Value(last_execution.second));
                }
                else
                {
                    last_runs.push_back(Value());
                    last_results.push_back(Value());
                }
            }
        }

        bool RemoveTask(const string &id)
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->tasks.find(id) == state->tasks.end())
            {
                throw InvalidInputException("Task with ID '%s' does not exist", id);
            }
            state->tasks.erase(id);
            return true;
        }

    private:
        static constexpr size_t MAX_EXECUTION_HISTORY = 100;

        shared_ptr<CronSchedulerState> state;
        std::thread scheduler_thread;

        void Stop()
        {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->should_stop = true;
            }
            state->condition.notify_all();
            if (!scheduler_thread.joinable())
            {
                return;
            }
            if (scheduler_thread.get_id() == std::this_thread::get_id())
            {
                // The worker released the last reference to the database and is
                // running its destructor. It cannot join itself; it only touches
                // the shared state from here on and exits as soon as it sees
                // should_stop.
                scheduler_thread.detach();
            }
            else
            {
                scheduler_thread.join();
            }
        }

        static void RunScheduler(shared_ptr<CronSchedulerState> s)
        {
            std::unique_lock<std::mutex> lock(s->mutex);
            while (!s->should_stop)
            {
                auto now = std::chrono::system_clock::now();

                // Collect the due tasks; they run without the lock held, so a
                // slow query does not block cron()/cron_jobs()/cron_delete() and
                // shutdown can always acquire the lock.
                vector<std::pair<string, string>> due;
                for (auto &task_pair : s->tasks)
                {
                    auto &task = task_pair.second;
                    if (task->active && task->next_run <= now)
                    {
                        due.emplace_back(task->id, task->query);
                    }
                }

                if (!due.empty())
                {
                    for (auto &entry : due)
                    {
                        if (s->should_stop)
                        {
                            break;
                        }
                        lock.unlock();
                        auto result = ExecuteQuery(s, entry.second);
                        lock.lock();
                        // The task may have been removed while it was running.
                        auto it = s->tasks.find(entry.first);
                        if (it != s->tasks.end())
                        {
                            AddExecutionResult(*it->second, result);
                            it->second->next_run = CalculateNextRun(it->second->expr);
                        }
                    }
                    continue;
                }

                std::chrono::system_clock::time_point next_wake = now + std::chrono::hours(24);
                for (auto &task_pair : s->tasks)
                {
                    auto &task = task_pair.second;
                    if (task->active)
                    {
                        next_wake = std::min(next_wake, task->next_run);
                    }
                }

                s->condition.wait_until(lock, next_wake);
            }
        }

        static string ExecuteQuery(const shared_ptr<CronSchedulerState> &s, const string &query)
        {
            try
            {
                auto db = s->db.lock();
                if (!db)
                {
                    return "Error: database is closed";
                }
                string result;
                {
                    Connection conn(*db);
                    auto query_result = conn.Query(query);
                    if (query_result->HasError())
                    {
                        result = "Error: " + query_result->GetError();
                    }
                    else
                    {
                        result = "Success";
                    }
                }
                // If this was the last reference, the database (and the
                // CronScheduler that owns this thread) is destroyed right here;
                // see Stop().
                db.reset();
                return result;
            }
            catch (const std::exception &e)
            {
                return "Exception: " + string(e.what());
            }
        }

        static std::chrono::system_clock::time_point CalculateNextRun(cron_expr &expr)
        {
            time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            time_t next = cron_next(&expr, now);
            return std::chrono::system_clock::from_time_t(next);
        }

        static string TimeToString(const std::chrono::system_clock::time_point &time)
        {
            auto t = std::chrono::system_clock::to_time_t(time);
            std::string ts = std::ctime(&t);
            if (!ts.empty() && ts[ts.length() - 1] == '\n')
            {
                ts.erase(ts.length() - 1);
            }
            return ts;
        }

        // Add execution result to history and trim to MAX_EXECUTION_HISTORY entries
        static void AddExecutionResult(CronTask &task, const string &result)
        {
            task.execution_history.emplace_back(std::chrono::system_clock::now(), result);
            while (task.execution_history.size() > MAX_EXECUTION_HISTORY)
            {
                task.execution_history.erase(task.execution_history.begin());
            }
        }
    };

    static shared_ptr<CronScheduler> GetScheduler(ClientContext &context)
    {
        auto scheduler = ObjectCache::GetObjectCache(context).Get<CronScheduler>(CronScheduler::CACHE_KEY);
        if (!scheduler)
        {
            throw InternalException("Cron scheduler not initialized");
        }
        return scheduler;
    }

    // Then the delete function
    static void CronDeleteFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        auto scheduler = GetScheduler(state.GetContext());

        auto &task_id_vector = args.data[0];
        UnaryExecutor::Execute<string_t, bool>(
            task_id_vector, result,
            [&](string_t task_id)
            {
                try
                {
                    return scheduler->RemoveTask(task_id.GetString());
                }
                catch (const Exception &e)
                {
                    throw InvalidInputException("Error deleting cron task: %s", e.what());
                }
            });
    }

    // Table function data structure
    struct CronJobsData : public GlobalTableFunctionState, public TableFunctionData
    {
        bool data_returned = false; // Add this flag
        vector<Value> job_ids;
        vector<Value> queries;
        vector<Value> schedules;
        vector<Value> next_runs;
        vector<Value> statuses;
        vector<Value> last_runs;
        vector<Value> last_results;

        CronJobsData() {}

        idx_t MaxThreads() const override
        {
            return 1;
        }
    };

    static unique_ptr<GlobalTableFunctionState> CronJobsInit(ClientContext &context, TableFunctionInitInput &input)
    {
        return make_uniq<CronJobsData>();
    }

    static void CronScalarFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        auto scheduler = GetScheduler(state.GetContext());

        auto &query_vector = args.data[0];
        auto &schedule_vector = args.data[1];

        UnaryExecutor::Execute<string_t, string_t>(
            query_vector, result,
            [&](string_t query)
            {
                try
                {
                    auto schedule = schedule_vector.GetValue(0).ToString();
                    auto task_id = scheduler->AddTask(query.GetString(), schedule);
                    return StringVector::AddString(result, task_id);
                }
                catch (const Exception &e)
                {
                    throw InvalidInputException("Error scheduling cron task: %s", e.what());
                }
            });
    }

    static unique_ptr<FunctionData> CronJobsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names)
    {

        names = {"job_id", "query", "schedule", "next_run", "status", "last_run", "last_result"};
        return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                        LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                        LogicalType::VARCHAR};

        return make_uniq<CronJobsData>();
    }

    static void CronJobsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output)
    {
        auto scheduler = GetScheduler(context);

        auto &data = data_p.global_state->Cast<CronJobsData>();

        // If we've already returned the data, we're done
        if (data.data_returned)
        {
            output.SetChildCardinality(0);
            return;
        }

        // Get the data
        scheduler->GetTasksStatus(data.job_ids, data.queries, data.schedules,
                                  data.next_runs, data.statuses, data.last_runs,
                                  data.last_results);

        if (data.job_ids.empty())
        {
            output.SetChildCardinality(0);
            return;
        }

        idx_t chunk_size = data.job_ids.size();

        output.SetChildCardinality(chunk_size);

        for (idx_t i = 0; i < chunk_size; i++)
        {
            output.data[0].SetValue(i, data.job_ids[i]);
            output.data[1].SetValue(i, data.queries[i]);
            output.data[2].SetValue(i, data.schedules[i]);
            output.data[3].SetValue(i, data.next_runs[i]);
            output.data[4].SetValue(i, data.statuses[i]);
            output.data[5].SetValue(i, data.last_runs[i]);
            output.data[6].SetValue(i, data.last_results[i]);
        }

        // Mark that we've returned the data
        data.data_returned = true;
    }

    // Declares a positional-only parameter with a real name, so duckdb_functions() reports it instead of col<N>.
    static FunctionParameter Positional(const char *name, LogicalType type) {
        return FunctionParameter(name, std::move(type), {}, FunctionParameterKind::POSITIONAL_ONLY);
    }

    static void LoadInternal(ExtensionLoader &loader);

    void CronjobExtension::Load(ExtensionLoader &loader)
    {
        LoadInternal(loader);
    }

    std::string CronjobExtension::Name()
    {
        return "cronjob";
    }

    std::string CronjobExtension::Version() const
    {
#ifdef CRONJOB_VERSION
        return CRONJOB_VERSION;
#else
        return "0.0.1";
#endif
    }

    static void LoadInternal(ExtensionLoader &loader)
    {
        auto &db_instance = loader.GetDatabaseInstance();
        // One scheduler per database; loading the extension again reuses it.
        auto scheduler = db_instance.GetObjectCache().GetOrCreate<CronScheduler>(CronScheduler::CACHE_KEY, db_instance);
        if (!scheduler)
        {
            throw InternalException("Failed to initialize cron scheduler");
        }

        // Register the cron scalar function
        auto cron_func = ScalarFunction("cron",
                                        {Positional("query", LogicalType::VARCHAR), Positional("schedule", LogicalType::VARCHAR)},
                                        LogicalType::VARCHAR,
                                        CronScalarFunction);
        cron_func.SetFallible();
        loader.RegisterFunction(cron_func);

        // Register cron_delete function
        auto cron_delete_func = ScalarFunction("cron_delete",
                                               {Positional("job_id", LogicalType::VARCHAR)},
                                               LogicalType::BOOLEAN,
                                               CronDeleteFunction);
        cron_delete_func.SetFallible();
        loader.RegisterFunction(cron_delete_func);

        // Register the cron_jobs table function
        auto cron_jobs_func = TableFunction("cron_jobs", {}, CronJobsFunction, CronJobsBind);
        cron_jobs_func.init_global = CronJobsInit;
        loader.RegisterFunction(cron_jobs_func);

        QueryFarmSendTelemetry(loader, "cronjob", "2026102101");
    }
} // namespace duckdb

extern "C"
{

    DUCKDB_CPP_EXTENSION_ENTRY(cronjob, loader)
    {
        duckdb::LoadInternal(loader);
    }

    DUCKDB_EXTENSION_API const char *cronjob_version()
    {
        return duckdb::DuckDB::LibraryVersion();
    }
}
