/* contrib/sr_plan/init.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sr_plan" to load this file. \quit

CREATE TABLE sr_plans (
	query_hash	int NOT NULL,
	plan_hash	int NOT NULL,
	query		varchar NOT NULL,
	plan		text NOT NULL,
	enable		boolean NOT NULL,
	valid		boolean NOT NULL,
	reloids		oid[]
);

CREATE INDEX sr_plans_query_hash_idx ON sr_plans (query_hash);
CREATE INDEX sr_plans_query_oids ON sr_plans USING gin(reloids);

CREATE FUNCTION _p(anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

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
			EXECUTE 'DELETE FROM sr_plans WHERE reloids @> ARRAY[$1]'
			USING obj.objid;
		ELSE
			IF obj.object_type = 'index' THEN
				FOR indobj IN SELECT indrelid FROM pg_index
					WHERE indexrelid = obj.objid
				LOOP
					EXECUTE 'DELETE FROM sr_plans WHERE reloids @> ARRAY[$1]'
					USING indobj.indrelid;
				END LOOP;
			END IF;
		END IF;
    END LOOP;
END
$$;

CREATE EVENT TRIGGER sr_plan_invalid_table ON sql_drop
    EXECUTE PROCEDURE sr_plan_invalid_table();
