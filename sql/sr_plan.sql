CREATE EXTENSION sr_plan;

SET sr_plan.log_usage = NOTICE;
CREATE TABLE test_table(test_attr1 int, test_attr2 int);
INSERT INTO test_table SELECT i, i + 1 FROM generate_series(1, 20) i;
SET sr_plan.write_mode = true;
SELECT * FROM test_table WHERE test_attr1 = _p(10);
SELECT * FROM test_table WHERE test_attr1 = 10;
SELECT * FROM test_table WHERE test_attr1 = 10;
SET sr_plan.write_mode = false;

SELECT * FROM test_table WHERE test_attr1 = _p(10);
SELECT * FROM test_table WHERE test_attr1 = 10;
SELECT * FROM test_table WHERE test_attr1 = 15;

UPDATE sr_plans SET enable = true;


SELECT * FROM test_table WHERE test_attr1 = _p(10);
SELECT * FROM test_table WHERE test_attr1 = _p(15);
SELECT * FROM test_table WHERE test_attr1 = 10;
SELECT * FROM test_table WHERE test_attr1 = 15;

SELECT enable, query FROM sr_plans ORDER BY length(query);
DROP TABLE test_table;
SELECT enable, query FROM sr_plans ORDER BY length(query);

CREATE TABLE test_table(test_attr1 int, test_attr2 int, test_attr3 int);

SELECT * FROM test_table WHERE test_attr1 = _p(10);
SELECT * FROM test_table WHERE test_attr1 = 10;
SELECT * FROM test_table WHERE test_attr1 = 10;
SELECT * FROM test_table WHERE test_attr1 = 15;

DROP EXTENSION sr_plan CASCADE;
DROP TABLE test_table;
