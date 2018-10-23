/* contrib/sr_plan/init.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION sr_plan" to load this file. \quit

CREATE TABLE sr_plans (
	query_hash	int NOT NULL,
	plan_hash	int NOT NULL,
	query		varchar NOT NULL,
	plan		text NOT NULL,
	enable		boolean NOT NULL,
	valid		boolean NOT NULL
);

CREATE INDEX sr_plans_query_hash_idx ON sr_plans (query_hash);

CREATE FUNCTION _p(anyelement)
RETURNS anyelement
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE FUNCTION sr_plan_invalid_table() RETURNS event_trigger
    AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE EVENT TRIGGER sr_plan_invalid_table ON sql_drop
    EXECUTE PROCEDURE sr_plan_invalid_table();
