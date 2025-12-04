# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the DuckDB CronJob Extension - a DuckDB extension that adds support for scheduled query execution using standard cron expressions. The extension runs as a background scheduler thread while the DuckDB process is active.

## Build Commands

```bash
# Build release version
GEN=ninja make release

# Build debug version
GEN=ninja make debug

# Run tests (requires release build)
make test

# Run tests with debug build
make test_debug

# Format code
make format

```

It is best to build with a debug build for testing as it adds more assertions.

All extension functions should be documented inside of DuckDB with CreateScalarFunctionInfo or CreateAggregateFunctionInfo or the appropriate type for the function.  This documentation of the function should include examples, parameter types and parameter names.  The function should be categorized.

When making changes the version should always be updated to the current date plus an ordinal counter in the form of YYYYMMDDCC.



## Testing

Tests use DuckDB's SQLLogicTest format located in `test/sql/`. To run a single test, build the extension and run the test binary directly:

```bash
./build/release/test/unittest "test/sql/cronjob.test"
```

## Architecture

### Extension Structure
- `src/cronjob_extension.cpp` - Main extension implementation containing:
  - `CronTask` - Struct representing a scheduled job with ID, query, schedule, and execution history
  - `CronScheduler` - Singleton scheduler class that manages a background thread for job execution
  - SQL functions: `cron()`, `cron_delete()`, `cron_jobs()` table function

### Dependencies
- `ccronexpr/` - C library for parsing cron expressions (vendored)
- `duckdb/` - DuckDB source as git submodule
- `extension-ci-tools/` - DuckDB extension build tools as git submodule
- OpenSSL - Required for build (linked via CMake)

### Cron Expression Format
The extension uses six-field cron expressions: `second minute hour day-of-month month day-of-week`

## Updating DuckDB Version

See `docs/UPDATING.md` for the full process. Key steps:
1. Update `./duckdb` submodule to latest tagged release
2. Update `./extension-ci-tools` to matching version branch
3. Update `duckdb_version` in `.github/workflows/MainDistributionPipeline.yml`
