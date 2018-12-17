DROP FUNCTION sr_plan_invalid_table() CASCADE;
DROP FUNCTION explain_jsonb_plan(jsonb) CASCADE;
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

CREATE OR REPLACE FUNCTION sr_plan_invalid_table() RETURNS event_trigger
LANGUAGE plpgsql AS $$
DECLARE
    obj		 record;
	indobj	 record;
BEGIN
    FOR obj IN SELECT * FROM pg_event_trigger_dropped_objects()
		WHERE object_type = 'table' OR object_type = 'index'
    LOOP
		IF obj.object_type = 'table' THEN
			DELETE FROM @extschema@.sr_plans WHERE reloids @> ARRAY[obj.objid];
		ELSE
			IF obj.object_type = 'index' THEN
				DELETE FROM @extschema@.sr_plans WHERE index_reloids @> ARRAY[obj.objid];
			END IF;
		END IF;
    END LOOP;
END
$$;

CREATE EVENT TRIGGER sr_plan_invalid_table ON sql_drop
    EXECUTE PROCEDURE sr_plan_invalid_table();
