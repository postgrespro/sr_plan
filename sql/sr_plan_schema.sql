CREATE SCHEMA plan;
CREATE EXTENSION sr_plan SCHEMA plan;
CREATE SCHEMA test;

CREATE TABLE test.test_table(test_attr1 int, test_attr2 int);
INSERT INTO test.test_table SELECT i, i + 1 FROM generate_series(1, 1000) i;
CREATE INDEX i1 ON test.test_table(test_attr1);

SET enable_seqscan=off;
SET sr_plan.write_mode = true;
SELECT * FROM test.test_table WHERE test_attr1 = plan._p(10);
SELECT * FROM test.test_table WHERE test_attr1 = 10;
SET sr_plan.write_mode = false;

UPDATE plan.sr_plans SET enable = TRUE;

SELECT enable, query FROM plan.sr_plans ORDER BY length(query);
EXPLAIN (COSTS OFF) SELECT * FROM test.test_table WHERE test_attr1 = plan._p(10);
EXPLAIN (COSTS OFF) SELECT * FROM test.test_table WHERE test_attr1 = 10;

DROP INDEX test.i1;
SELECT enable, query FROM plan.sr_plans ORDER BY length(query);

SELECT * FROM test.test_table WHERE test_attr1 = plan._p(10);
SELECT * FROM test.test_table WHERE test_attr1 = plan._p(20);
SELECT * FROM test.test_table WHERE test_attr1 = 10;

DROP SCHEMA plan CASCADE;
SELECT * FROM test.test_table WHERE test_attr1 = 10;

DROP SCHEMA test CASCADE;
