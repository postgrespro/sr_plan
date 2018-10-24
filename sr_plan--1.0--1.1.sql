DROP TABLE sr_plans CASCADE;
CREATE TABLE sr_plans (
	query_hash	int NOT NULL,
	plan_hash	int NOT NULL,
	query		varchar NOT NULL,
	plan		text NOT NULL,
	enable		boolean NOT NULL,

	reloids				oid[],
	index_reloids		oid[]
);

CREATE INDEX sr_plans_query_hash_idx ON sr_plans (query_hash);
CREATE INDEX sr_plans_query_oids ON sr_plans USING gin(reloids);
CREATE INDEX sr_plans_query_index_oids ON sr_plans USING gin(index_reloids);

DROP FUNCTION explain_jsonb_plan(jsonb) CASCADE;
DROP FUNCTION sr_plan_invalid_table() CASCADE;
