#define DUCKDB_EXTENSION_MAIN

#include "cronjob_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/main/client_config.hpp"
#include "ccronexpr.h"
#include <thread>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace duckdb {

// First declare CronTask
struct CronTask {
    string id;
    string query;
    string schedule;
    bool active = true;
    std::chrono::system_clock::time_point next_run;
    std::vector<std::pair<std::chrono::system_clock::time_point, string>> execution_history;
    std::mutex mutex;
    cron_expr expr;
    std::unordered_map<string, Value> variables;  // Store variables for this task
};

class CronScheduler {
public:
    CronScheduler(DatabaseInstance &db) : db_instance(db), should_stop(false) {
        scheduler_thread = std::thread(&CronScheduler::RunScheduler, this);
    }

    ~CronScheduler() {
        {
            std::lock_guard<std::mutex> lock(tasks_mutex);
            should_stop = true;
        }
        condition.notify_one();
        if (scheduler_thread.joinable()) {
            scheduler_thread.join();
        }
    }

    string AddTask(const string &query, const string &schedule, ClientContext &context) {
        // Parse cron expression
        cron_expr expr;
        const char* err = NULL;
        cron_parse_expr(schedule.c_str(), &expr, &err);
        if (err) {
            throw InvalidInputException("Invalid cron expression: %s", err);
        }

        auto task = make_uniq<CronTask>();
        task->id = "task_" + std::to_string(task_counter++);
        task->query = query;
        task->schedule = schedule;
        task->expr = expr;
        task->next_run = CalculateNextRun(task->expr);

        // Capture current variables from the context
        auto &config = ClientConfig::GetConfig(context);
        for (const auto &var_pair : config.user_variables) {
            task->variables[var_pair.first] = var_pair.second;
        }

        std::lock_guard<std::mutex> lock(tasks_mutex);
        string task_id = task->id;
        tasks[task_id] = std::move(task);
        condition.notify_one();
        return task_id;
    }

    void GetTasksStatus(vector<Value> &job_ids, vector<Value> &queries, vector<Value> &schedules,
                       vector<Value> &next_runs, vector<Value> &statuses, vector<Value> &last_runs,
                       vector<Value> &last_results) {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        for (auto &task_pair : tasks) {
            auto &task = task_pair.second;
            std::lock_guard<std::mutex> task_lock(task->mutex);

            job_ids.push_back(Value(task->id));
            queries.push_back(Value(task->query));
            schedules.push_back(Value(task->schedule));
            next_runs.push_back(Value(TimeToString(task->next_run)));
            statuses.push_back(Value(task->active ? "Active" : "Inactive"));

            if (!task->execution_history.empty()) {
                auto last_execution = task->execution_history.back();
                last_runs.push_back(Value(TimeToString(last_execution.first)));
                last_results.push_back(Value(last_execution.second));
            } else {
                last_runs.push_back(Value());
                last_results.push_back(Value());
            }
        }
    }

    bool RemoveTask(const string &id) {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        if (tasks.find(id) == tasks.end()) {
            throw InvalidInputException("Task with ID '%s' does not exist", id);
        }
        tasks.erase(id);
        return true;
    }


private:
    DatabaseInstance &db_instance;
    std::thread scheduler_thread;
    std::mutex tasks_mutex;
    std::condition_variable condition;
    bool should_stop;
    std::atomic<size_t> task_counter{0};
    std::unordered_map<string, unique_ptr<CronTask>> tasks;

    void RunScheduler() {
        while (true) {
            std::unique_lock<std::mutex> lock(tasks_mutex);
            if (should_stop) break;

            auto now = std::chrono::system_clock::now();
            std::chrono::system_clock::time_point next_wake = now + std::chrono::hours(24);

            for (auto &task_pair : tasks) {
                auto &task = task_pair.second;
                if (task->active && task->next_run <= now) {
                    ExecuteTask(*task);
                    task->next_run = CalculateNextRun(task->expr);
                }
                if (task->active) {
                    next_wake = std::min(next_wake, task->next_run);
                }
            }

            condition.wait_until(lock, next_wake);
        }
    }

    void ExecuteTask(CronTask &task) {
        try {
            DuckDB db(db_instance);
            Connection conn(db);
            
            // Set variables before executing the query
            for (const auto &var_pair : task.variables) {
                string set_var_query = "SET VARIABLE " + var_pair.first + " = " + var_pair.second.ToSQLString();
                auto set_result = conn.Query(set_var_query);
                if (set_result->HasError()) {
                    std::lock_guard<std::mutex> task_lock(task.mutex);
                    task.execution_history.emplace_back(
                        std::chrono::system_clock::now(),
                        "Error setting variable " + var_pair.first + ": " + set_result->GetError()
                    );
                    return;
                }
            }
            
            auto result = conn.Query(task.query);

            std::lock_guard<std::mutex> task_lock(task.mutex);
            if (result->HasError()) {
                task.execution_history.emplace_back(
                    std::chrono::system_clock::now(),
                    "Error: " + result->GetError()
                );
            } else {
                task.execution_history.emplace_back(
                    std::chrono::system_clock::now(),
                    "Success"
                );
            }
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> task_lock(task.mutex);
            task.execution_history.emplace_back(
                std::chrono::system_clock::now(),
                "Exception: " + string(e.what())
            );
        }
    }

    std::chrono::system_clock::time_point CalculateNextRun(cron_expr &expr) {
        time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        time_t next = cron_next(&expr, now);
        return std::chrono::system_clock::from_time_t(next);
    }

    string TimeToString(const std::chrono::system_clock::time_point &time) {
        auto t = std::chrono::system_clock::to_time_t(time);
        std::string ts = std::ctime(&t);
        if (!ts.empty() && ts[ts.length()-1] == '\n') {
            ts.erase(ts.length()-1);
        }
        return ts;
    }
};

// Global scheduler instance
static unique_ptr<CronScheduler> scheduler;

// Then the delete function
static void CronDeleteFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    if (!scheduler) {
        throw InternalException("Cron scheduler not initialized");
    }

    auto &task_id_vector = args.data[0];
    UnaryExecutor::Execute<string_t, bool>(
        task_id_vector, result, args.size(),
        [&](string_t task_id) {
            try {
                return scheduler->RemoveTask(task_id.GetString());
            } catch (const Exception &e) {
                throw InvalidInputException("Error deleting cron task: %s", e.what());
            }
        });
}

// Table function data structure
struct CronJobsData : public GlobalTableFunctionState, public TableFunctionData {
    bool data_returned = false;  // Add this flag
    vector<Value> job_ids;
    vector<Value> queries;
    vector<Value> schedules;
    vector<Value> next_runs;
    vector<Value> statuses;
    vector<Value> last_runs;
    vector<Value> last_results;

    CronJobsData() {}

    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<GlobalTableFunctionState> CronJobsInit(ClientContext &context, TableFunctionInitInput &input) {
    return make_uniq<CronJobsData>();
}

static void CronScalarFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    if (!scheduler) {
        throw InternalException("Cron scheduler not initialized");
    }

    auto &query_vector = args.data[0];
    auto &schedule_vector = args.data[1];
    auto &context = state.GetContext();

    UnaryExecutor::Execute<string_t, string_t>(
        query_vector, result, args.size(),
        [&](string_t query) {
            try {
                auto schedule = schedule_vector.GetValue(0).ToString();
                auto task_id = scheduler->AddTask(query.GetString(), schedule, context);
                return StringVector::AddString(result, task_id);
            } catch (const Exception &e) {
                throw InvalidInputException("Error scheduling cron task: %s", e.what());
            }
        });
}

static unique_ptr<FunctionData> CronJobsBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
    
    names = {"job_id", "query", "schedule", "next_run", "status", "last_run", "last_result"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                   LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                   LogicalType::VARCHAR};

    return make_uniq<CronJobsData>();
}

static void CronJobsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    
    if (!scheduler) {
        throw InternalException("Cron scheduler not initialized");
    }

    auto &data = data_p.global_state->Cast<CronJobsData>();

    // If we've already returned the data, we're done
    if (data.data_returned) {
        output.SetCardinality(0);
        return;
    }

    // Get the data
    scheduler->GetTasksStatus(data.job_ids, data.queries, data.schedules,
                            data.next_runs, data.statuses, data.last_runs,
                            data.last_results);

    if (data.job_ids.empty()) {
        output.SetCardinality(0);
        return;
    }

    idx_t chunk_size = data.job_ids.size();

    output.SetCardinality(chunk_size);

    for (idx_t i = 0; i < chunk_size; i++) {
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

void CronjobExtension::Load(DuckDB &db) {
    scheduler = make_uniq<CronScheduler>(*db.instance);
    if (!scheduler) {
        throw InternalException("Failed to initialize cron scheduler");
    }

    // Register the cron scalar function
    auto cron_func = ScalarFunction("cron", 
                                  {LogicalType::VARCHAR, LogicalType::VARCHAR},
                                  LogicalType::VARCHAR,
                                  CronScalarFunction);
    ExtensionUtil::RegisterFunction(*db.instance, cron_func);

    // Register cron_delete function
    auto cron_delete_func = ScalarFunction("cron_delete",
                                         {LogicalType::VARCHAR},
                                         LogicalType::BOOLEAN,
                                         CronDeleteFunction);
    ExtensionUtil::RegisterFunction(*db.instance, cron_delete_func);



    // Register the cron_jobs table function
    auto cron_jobs_func = TableFunction("cron_jobs", {}, CronJobsFunction, CronJobsBind);
    cron_jobs_func.init_global = CronJobsInit;  // Add this line
    ExtensionUtil::RegisterFunction(*db.instance, cron_jobs_func);

}

std::string CronjobExtension::Name() {
    return "cronjob";
}

std::string CronjobExtension::Version() const {
#ifdef CRONJOB_VERSION
    return CRONJOB_VERSION;
#else
    return "0.0.1";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_EXTENSION_API void cronjob_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::CronjobExtension>();
}

DUCKDB_EXTENSION_API const char *cronjob_version() {
    return duckdb::DuckDB::LibraryVersion();
}
}
