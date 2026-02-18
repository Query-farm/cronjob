<img src="https://github.com/user-attachments/assets/46a5c546-7e9b-42c7-87f4-bc8defe674e0" width=250 />

# DuckDB CronJob Extension
The DuckDB Cron extension adds support for scheduled query execution within DuckDB. It allows you to schedule SQL queries to run periodically using standard cron expressions while your DuckDB process is active.

> Experimental: USE AT YOUR OWN RISK!

### Install & Load
```sql
INSTALL cronjob FROM community;
LOAD cronjob;
```

### Cron Runner
```sql
-- Every 15 seconds during hours 1-4
SELECT cron('SELECT now()', '*/15 * 1-4 * * *');

-- Every 2 hours (at minute 0, second 0) during hours 1-4
SELECT cron('SELECT version()', '0 0 */2 1-4 * *');

-- Every Monday through Friday at 7:00:00 AM
SELECT cron('SELECT cleanup()', '0 0 7 ? * MON-FRI');
```

The function returns a job ID that can be used to manage the scheduled task.

```sql
┌──────────┐
│  job_id  │
│  varchar │
├──────────┤
│ task_0   │
└──────────┘
```

#### Supported Expressions
The extension uses six-field cron expressions:
```
┌───────────── second (0 - 59)
│ ┌───────────── minute (0 - 59)
│ │ ┌───────────── hour (0 - 23)
│ │ │ ┌───────────── day of month (1 - 31)
│ │ │ │ ┌───────────── month (1 - 12)
│ │ │ │ │ ┌───────────── day of week (0 - 6) (Sunday to Saturday)
│ │ │ │ │ │
* * * * * *
```

##### Special characters:

- `*`: any value
- `?`: no specific value (used in day-of-month/day-of-week)
- `,`: value list separator
- `-`: range of values
- `/`: step values
- `MON-SUN`: named weekdays (case sensitive)

##### Common Patterns:
```
0 0 * * * *: Top of every hour
0 */15 * * * *: Every 15 minutes
0 0 0 * * *: Once per day at midnight
0 0 12 ? * MON-FRI: Weekdays at noon
0 0 0 1 * ?: First day of every month
```


### Listing Jobs
Use the cron_jobs() table function to view all scheduled jobs and their status:
```sql
SELECT * FROM cron_jobs();

┌─────────┬──────────────────┬────────────────┬──────────────────────────┬─────────┬──────────────────────────┬─────────────┐
│ job_id  │      query       │    schedule    │         next_run         │ status  │         last_run         │ last_result │
│ varchar │     varchar      │    varchar     │         varchar          │ varchar │         varchar          │   varchar   │
├─────────┼──────────────────┼────────────────┼──────────────────────────┼─────────┼──────────────────────────┼─────────────┤
│ task_0  │ SELECT version() │ */10 * * * * * │ Fri Nov 15 20:44:20 2024 │ Active  │ Fri Nov 15 20:44:10 2024 │ Success     │
└─────────┴──────────────────┴────────────────┴──────────────────────────┴─────────┴──────────────────────────┴─────────────┘
```

Returns:

- `job_id`: Unique identifier for the job
- `query`: The SQL query to execute
- `schedule`: The cron expression
- `next_run`: Next scheduled execution time
- `status`: Current status (Active/Inactive)
- `last_run`: Last execution timestamp
- `last_result`: Result of the last execution

### Deleting Jobs
To delete a scheduled job:
```sql
SELECT cron_delete('task_0');
```
