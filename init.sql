/* contrib/sr_plan/init.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sr_plan" to load this file. \quit

CREATE TABLE sr_plans (
	query_hash	int NOT NULL,
	query_id	int8 NOT NULL,
	plan_hash	int NOT NULL,
	enable		boolean NOT NULL,
	query		varchar NOT NULL,
	plan		text NOT NULL,

	reloids				oid[],
	index_reloids		oid[]
);

CREATE INDEX sr_plans_query_hash_idx ON sr_plans (query_hash);
CREATE INDEX sr_plans_query_oids ON sr_plans USING gin(reloids);
CREATE INDEX sr_plans_query_index_oids ON sr_plans USING gin(index_reloids);

CREATE FUNCTION _p(anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME', 'do_nothing'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION show_plan(query_hash int4,
							index int4 default null,
							format cstring default null)
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'show_plan'
LANGUAGE C VOLATILE;

CREATE FUNCTION sr_plan_invalid_table() RETURNS event_trigger
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
