<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# DuckDB CronJob Extension

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/cronjob.html)
[![v1.5 build](https://github.com/Query-farm/cronjob/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/cronjob/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

The DuckDB Cron extension adds support for scheduled query execution within DuckDB. It allows you to schedule SQL queries to run periodically using standard cron expressions while your DuckDB process is active.

> Experimental: USE AT YOUR OWN RISK!

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/cronjob](https://query.farm/products/extensions/cronjob)**

## Installation

```sql
INSTALL cronjob FROM community;
LOAD cronjob;
```
