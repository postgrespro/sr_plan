CREATE EXTENSION sr_plan;

SET sr_plan.log_usage = NOTICE;
CREATE TABLE explain_test(test_attr1 int, test_attr2 int);
INSERT INTO explain_test SELECT i, i + 1 FROM generate_series(1, 20) i;

SET sr_plan.write_mode = true;
SELECT * FROM explain_test WHERE test_attr1 = 10;

SET sr_plan.write_mode = false;
UPDATE sr_plans SET enable = true;

-- check show_plan
WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash) FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, index := 1) FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, index := 2) FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, format := 'json') FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, format := 'text') FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, format := 'xml') FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, format := 'yaml') FROM vars;

WITH vars AS (SELECT query_hash FROM sr_plans WHERE
	query = 'SELECT * FROM explain_test WHERE test_attr1 = 10;' LIMIT 1)
SELECT show_plan(vars.query_hash, format := 'nonsense') FROM vars;

DROP TABLE explain_test CASCADE;
DROP EXTENSION sr_plan CASCADE;
