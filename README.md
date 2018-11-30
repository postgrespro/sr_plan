[![Build Status](https://travis-ci.org/postgrespro/sr_plan.svg?branch=master)](https://travis-ci.org/postgrespro/sr_plan)
[![GitHub license](https://img.shields.io/badge/license-PostgreSQL-blue.svg)](https://raw.githubusercontent.com/postgrespro/sr_plan/master/LICENSE)


# Save and restore query plans in PostgreSQL

## Rationale

sr_plan looks like Oracle Outline system. It can be used to lock the execution plan. It is necessary if you do not trust the planner or able to form a better plan.

## Build and install

```bash
make USE_PGXS=1
make USE_PGXS=1 install
```

and modify your postgres config:
```
shared_preload_libraries = 'sr_plan'
```

## Usage

Install the extension in your database:

```SQL
CREATE EXTENSION sr_plan;
```
If you want to save the query plan is necessary to set the variable:

```SQL
set sr_plan.write_mode = true;
```

Now plans for all subsequent queries will be stored in the table sr_plans.
Don't forget that all queries will be stored including duplicates.

Make an example query:
```SQL
select query_hash from sr_plans where query_hash=10;
```

Disable saving the plan for the query:
```SQL
set sr_plan.write_mode = false;
```

Enable it:

```SQL
update sr_plans set enable=true;
```

After that, the plan for the query will be taken from the sr_plans.

In addition sr plan allows you to save a parameterized query plan.
In this case, we have some constants in the query are not essential.
For the parameters we use a special function _p (anyelement) example:

```SQL
select query_hash from sr_plans where query_hash=1000+_p(10);
```

If we keep the plan for the query and enable it to be used also for the following queries:

```SQL
select query_hash from sr_plans where query_hash=1000+_p(11);
select query_hash from sr_plans where query_hash=1000+_p(-5);
```
