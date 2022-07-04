#include "sr_plan.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "catalog/pg_extension.h"
#include "catalog/indexing.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/hash.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "miscadmin.h"

#if PG_VERSION_NUM >= 100000
#include "utils/queryenvironment.h"
#include "catalog/index.h"
#endif

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(_p);

void _PG_init(void);
void _PG_fini(void);

static planner_hook_type srplan_planner_hook_next = NULL;
post_parse_analyze_hook_type srplan_post_parse_analyze_hook_next = NULL;
static bool sr_plan_write_mode = false;
static int sr_plan_log_usage = 0;

static Oid sr_plan_fake_func = 0;

static PlannedStmt *sr_planner(Query *parse, int cursorOptions,
								ParamListInfo boundParams);

static void sr_analyze(ParseState *pstate, Query *query);

static Oid get_sr_plan_schema(void);
static Oid sr_get_relname_oid(Oid schema_oid, const char *relname);
static bool sr_query_walker(Query *node, void *context);
static bool sr_query_expr_walker(Node *node, void *context);
void walker_callback(void *node);

static void plan_tree_visitor(Plan *plan,
				  void (*visitor) (Plan *plan, void *context),
				  void *context);
static void execute_for_plantree(PlannedStmt *planned_stmt,
					 void (*proc) (void *context, Plan *plan),
					 void *context);
static void restore_params(void *context, Plan *plan);
static Datum get_query_hash(Query *node);
static void collect_indexid(void *context, Plan *plan);

struct QueryParam
{
	int location;
	int funccollid;
	void *node;
};

struct QueryParamsContext
{
	bool	collect;
	List   *params;
};

struct IndexIds
{
	List   *ids;
};

List *query_params;
const char *query_text;

static void
sr_analyze(ParseState *pstate, Query *query)
{
	query_text = pstate->p_sourcetext;
	if (srplan_post_parse_analyze_hook_next)
		srplan_post_parse_analyze_hook_next(pstate, query);
}

/*
 * Return sr_plan schema's Oid or InvalidOid if that's not possible.
 */
static Oid
get_sr_plan_schema(void)
{
	Oid				result;
	Relation		rel;
	SysScanDesc		scandesc;
	HeapTuple		tuple;
	ScanKeyData		entry[1];
	Oid				ext_schema;
	LOCKMODE heap_lock =  AccessShareLock;

	/* It's impossible to fetch sr_plan's schema now */
	if (!IsTransactionState())
		return InvalidOid;

	ext_schema = get_extension_oid("sr_plan", true);
	if (ext_schema == InvalidOid)
		return InvalidOid; /* exit if sr_plan does not exist */

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_schema));

	rel = heap_open(ExtensionRelationId, heap_lock);
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	heap_close(rel, heap_lock);

	return result;
}

/*
 * Return Oid of relation in sr_plan extension schema or
 * InvalidOid if that's not possible.
 */

static Oid
sr_get_relname_oid(Oid schema_oid, const char *relname)
{
	if (schema_oid == InvalidOid)
		schema_oid = get_sr_plan_schema();

	if (schema_oid == InvalidOid)
		return InvalidOid;

	return get_relname_relid(relname, schema_oid);
}

static void
params_restore_visitor(Plan *plan, void *context)
{
	expression_tree_walker((Node *) plan->qual, sr_query_expr_walker, context);
	expression_tree_walker((Node *) plan->targetlist, sr_query_expr_walker, context);
}

static void
restore_params(void *context, Plan *plan)
{
	plan_tree_visitor(plan, params_restore_visitor, context);
}

static void
collect_indexid_visitor(Plan *plan, void *context)
{
	struct IndexIds		*index_ids = context;
	if (plan == NULL)
		return;

	if (IsA(plan, IndexScan))
	{
		IndexScan	*scan = (IndexScan *) plan;
		index_ids->ids = lappend_oid(index_ids->ids, scan->indexid);
	}

	if (IsA(plan, IndexOnlyScan))
	{
		IndexOnlyScan	*scan = (IndexOnlyScan *) plan;
		index_ids->ids = lappend_oid(index_ids->ids, scan->indexid);
	}

	if (IsA(plan, BitmapIndexScan))
	{
		BitmapIndexScan	*scan = (BitmapIndexScan *) plan;
		index_ids->ids = lappend_oid(index_ids->ids, scan->indexid);
	}
}

static void
collect_indexid(void *context, Plan *plan)
{
	plan_tree_visitor(plan, collect_indexid_visitor, context);
}

static PlannedStmt *
lookup_plan_by_query_hash(Snapshot snapshot, Relation sr_index_rel,
							Relation sr_plans_heap, ScanKey key, void *context)
{
	PlannedStmt	   *pl_stmt = NULL;
	HeapTuple		htup;
	IndexScanDesc	query_index_scan;

	query_index_scan = index_beginscan(sr_plans_heap, sr_index_rel, snapshot, 1, 0);
	index_rescan(query_index_scan, key, 1, NULL, 0);

	while ((htup = index_getnext(query_index_scan, ForwardScanDirection)) != NULL)
	{
		Datum		search_values[Anum_sr_attcount];
		bool		search_nulls[Anum_sr_attcount];

		heap_deform_tuple(htup, sr_plans_heap->rd_att,
						  search_values, search_nulls);

		/* Check enabled field */
		if (DatumGetBool(search_values[Anum_sr_enable - 1]))
		{
			char *out = TextDatumGetCString(DatumGetTextP((search_values[3])));
			pl_stmt = stringToNode(out);

			execute_for_plantree(pl_stmt, restore_params, context);
			break;
		}
	}

	index_endscan(query_index_scan);
	return pl_stmt;
}

/* planner_hook */
static PlannedStmt *
sr_planner(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	Datum			query_hash;
	Relation		sr_plans_heap,
					sr_index_rel;
	HeapTuple		tuple;
	Oid				sr_plans_oid,
					sr_index_oid,
					schema_oid;
	char		   *schema_name;
	char		   *plan_text;
	List		   *func_name_list;
	Snapshot		snapshot;
	ScanKeyData		key;
	bool			found;
	Datum			plan_hash;
	IndexScanDesc	query_index_scan;

	PlannedStmt	   *pl_stmt = NULL;
	LOCKMODE		heap_lock =  AccessShareLock;
	struct QueryParamsContext qp_context = {true, NULL};

	static int		level = 0;

	level++;

#define call_standard_planner() \
	(srplan_planner_hook_next ? \
		srplan_planner_hook_next(parse, cursorOptions, boundParams) : \
		standard_planner(parse, cursorOptions, boundParams))

	schema_oid = get_sr_plan_schema();
	if (!OidIsValid(schema_oid))
	{
		/* Just call standard_planner() if schema doesn't exist. */
		pl_stmt = call_standard_planner();
		level--;
		return pl_stmt;
	}

	if (sr_plan_fake_func)
	{
		HeapTuple   ftup;
		ftup = SearchSysCache1(PROCOID, ObjectIdGetDatum(sr_plan_fake_func));
		if (!HeapTupleIsValid(ftup))
			sr_plan_fake_func = 0;
		else
			ReleaseSysCache(ftup);
	}
	else
	{
		Oid args[1] = {ANYELEMENTOID};

		/* find oid of _p function */
		schema_name = get_namespace_name(schema_oid);
		func_name_list = list_make2(makeString(schema_name), makeString("_p")); 
		sr_plan_fake_func = LookupFuncName(func_name_list, 1, args, true);
		list_free(func_name_list);
		pfree(schema_name);
	}

	sr_index_oid = sr_get_relname_oid(schema_oid, SR_PLANS_TABLE_QUERY_INDEX_NAME);
	sr_plans_oid = sr_get_relname_oid(schema_oid, SR_PLANS_TABLE_NAME);

	if (sr_plans_oid == InvalidOid || sr_index_oid == InvalidOid)
	{
		/* Just call standard_planner() if we didn't find relations of sr_plan. */
		elog(WARNING, "sr_plan extension installed incorrectly. Do nothing. It's ok in pg_restore.");
		pl_stmt = call_standard_planner();
		level--;
		return pl_stmt;
	}

	/* Make list with all _p functions and his position */
	sr_query_walker((Query *) parse, &qp_context);
	query_hash = get_query_hash(parse);
	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT4EQ, query_hash);

	/* Try to find already planned statement */
	heap_lock = AccessShareLock;
	sr_plans_heap = heap_open(sr_plans_oid, heap_lock);
	sr_index_rel = index_open(sr_index_oid, heap_lock);

	qp_context.collect = false;
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	pl_stmt = lookup_plan_by_query_hash(snapshot, sr_index_rel, sr_plans_heap,
										&key, &qp_context);
	if (pl_stmt != NULL)
	{
		level--;
		if (sr_plan_log_usage > 0)
			elog(sr_plan_log_usage, "sr_plan: cached plan was used for query: %s", query_text);

		goto cleanup;
	}

	if (!sr_plan_write_mode || level > 1)
	{
		/* quick way out if not in write mode */
		pl_stmt = call_standard_planner();
		level--;
		goto cleanup;
	}

	/* close and get AccessExclusiveLock */
	UnregisterSnapshot(snapshot);
	index_close(sr_index_rel, heap_lock);
	heap_close(sr_plans_heap, heap_lock);

	heap_lock = AccessExclusiveLock;
	sr_plans_heap = heap_open(sr_plans_oid, heap_lock);
	sr_index_rel = index_open(sr_index_oid, heap_lock);

	/* recheck plan in index */
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	pl_stmt = lookup_plan_by_query_hash(snapshot, sr_index_rel, sr_plans_heap,
										&key, &qp_context);
	if (pl_stmt != NULL)
	{
		level--;
		goto cleanup;
	}

	/* from now on we use this new plan */
	pl_stmt = call_standard_planner();
	level--;
	plan_text = nodeToString(pl_stmt);
	plan_hash = hash_any((unsigned char *) plan_text, strlen(plan_text));

	/*
	 * Try to find existing plan for this query and skip addding if
	 * it already exists even it is not valid and not enabled.
	 */
	query_index_scan = index_beginscan(sr_plans_heap, sr_index_rel,
									   snapshot, 1, 0);
	index_rescan(query_index_scan, &key, 1, NULL, 0);

	found = false;
	for (;;)
	{
		HeapTuple	htup;
		Datum		search_values[Anum_sr_attcount];
		bool		search_nulls[Anum_sr_attcount];

		ItemPointer tid = index_getnext_tid(query_index_scan, ForwardScanDirection);
		if (tid == NULL)
			break;

		htup = index_fetch_heap(query_index_scan);
		if (htup == NULL)
			break;

		heap_deform_tuple(htup, sr_plans_heap->rd_att,
						  search_values, search_nulls);

		/* Detect full plan duplicate */
		if (DatumGetInt32(search_values[Anum_sr_plan_hash - 1]) == DatumGetInt32(plan_hash))
		{
			found = true;
			break;
		}
	}
	index_endscan(query_index_scan);

	if (!found)
	{
		struct IndexIds	index_ids = {NIL};

		Relation	reloids_index_rel;
		Oid			reloids_index_oid;

		Relation	index_reloids_index_rel;
		Oid			index_reloids_index_oid;

		ArrayType  *reloids = NULL;
		ArrayType  *index_reloids = NULL;
		Datum		values[Anum_sr_attcount];
		bool		nulls[Anum_sr_attcount];

		int			reloids_len = list_length(pl_stmt->relationOids);

		/* prepare relation for reloids index too */
		reloids_index_oid = sr_get_relname_oid(schema_oid, SR_PLANS_RELOIDS_INDEX);
		reloids_index_rel = index_open(reloids_index_oid, heap_lock);

		/* prepare relation for reloids index too */
		index_reloids_index_oid = sr_get_relname_oid(schema_oid, SR_PLANS_INDEX_RELOIDS_INDEX);
		index_reloids_index_rel = index_open(index_reloids_index_oid, heap_lock);

		MemSet(nulls, 0, sizeof(nulls));

		values[Anum_sr_query_hash - 1] = query_hash;
		values[Anum_sr_plan_hash - 1] = plan_hash;
		values[Anum_sr_query - 1] = CStringGetTextDatum(query_text);
		values[Anum_sr_plan - 1] = CStringGetTextDatum(plan_text);
		values[Anum_sr_enable - 1] = BoolGetDatum(false);
		values[Anum_sr_reloids - 1] = (Datum) 0;
		values[Anum_sr_index_reloids - 1] = (Datum) 0;

		/* save related oids */
		if (reloids_len)
		{
			int			pos;
			ListCell   *lc;
			Datum	   *reloids_arr = palloc(sizeof(Datum) * reloids_len);

			pos = 0;
			foreach(lc, pl_stmt->relationOids)
			{
				reloids_arr[pos] = ObjectIdGetDatum(lfirst_oid(lc));
				pos++;
			}
			reloids = construct_array(reloids_arr, reloids_len, OIDOID,
											 sizeof(Oid), true, 'i');
			values[Anum_sr_reloids - 1] = PointerGetDatum(reloids);

			pfree(reloids_arr);
		}
		else nulls[Anum_sr_reloids - 1] = true;

		/* saved related index oids */
		execute_for_plantree(pl_stmt, collect_indexid, (void *) &index_ids);
		if (list_length(index_ids.ids))
		{
			int len = list_length(index_ids.ids);
			int			pos;
			ListCell   *lc;
			Datum	   *ids_arr = palloc(sizeof(Datum) * len);

			pos = 0;
			foreach(lc, index_ids.ids)
			{
				ids_arr[pos] = ObjectIdGetDatum(lfirst_oid(lc));
				pos++;
			}
			index_reloids = construct_array(ids_arr, len, OIDOID,
											 sizeof(Oid), true, 'i');
			values[Anum_sr_index_reloids - 1] = PointerGetDatum(index_reloids);

			pfree(ids_arr);
		}
		else nulls[Anum_sr_index_reloids - 1] = true;

		tuple = heap_form_tuple(sr_plans_heap->rd_att, values, nulls);
		simple_heap_insert(sr_plans_heap, tuple);

		if (sr_plan_log_usage)
			elog(sr_plan_log_usage, "sr_plan: saved plan for %s", query_text);

		index_insert_compat(sr_index_rel,
					 values, nulls,
					 &(tuple->t_self),
					 sr_plans_heap,
					 UNIQUE_CHECK_NO);

		if (reloids)
		{
			index_insert_compat(reloids_index_rel,
						 &values[Anum_sr_reloids - 1],
						 &nulls[Anum_sr_reloids-1],
						 &(tuple->t_self),
						 sr_plans_heap,
						 UNIQUE_CHECK_NO);
		}

		if (index_reloids)
		{
			index_insert_compat(index_reloids_index_rel,
						 &values[Anum_sr_index_reloids - 1],
						 &nulls[Anum_sr_index_reloids-1],
						 &(tuple->t_self),
						 sr_plans_heap,
						 UNIQUE_CHECK_NO);
		}

		index_close(reloids_index_rel, heap_lock);
		index_close(index_reloids_index_rel, heap_lock);

		/* Make changes visible */
		CommandCounterIncrement();
	}

cleanup:
	UnregisterSnapshot(snapshot);

	index_close(sr_index_rel, heap_lock);
	heap_close(sr_plans_heap, heap_lock);

	return pl_stmt;
}

static bool
sr_query_walker(Query *node, void *context)
{
	if (node == NULL)
		return false;

	// check for nodes that special work is required for, eg:
	if (IsA(node, FromExpr))
		return sr_query_expr_walker((Node *)node, context);

	// for any node type not specially processed, do:
	if (IsA(node, Query))
		return query_tree_walker(node, sr_query_walker, context, 0);

	return false;
}

static bool
sr_query_expr_walker(Node *node, void *context)
{
	struct QueryParamsContext *qp_context = context;
	FuncExpr	*fexpr = (FuncExpr *) node;

	if (node == NULL)
		return false;

	if (IsA(node, FuncExpr) && fexpr->funcid == sr_plan_fake_func)
	{
		if (qp_context->collect)
		{
			struct QueryParam *param = (struct QueryParam *) palloc(sizeof(struct QueryParam));
			param->location = fexpr->location;
			param->node = fexpr->args->head->data.ptr_value;
			param->funccollid = fexpr->funccollid;

			/* HACK: location could lost after planning */
			fexpr->funccollid = fexpr->location;

			if (sr_plan_log_usage)
				elog(sr_plan_log_usage, "sr_plan: collected parameter on %d", param->location);

			qp_context->params = lappend(qp_context->params, param);
		}
		else
		{
			ListCell	*lc;

			foreach(lc, qp_context->params)
			{
				struct QueryParam *param = lfirst(lc);

				if (param->location == fexpr->funccollid)
				{
					fexpr->funccollid = param->funccollid;
					fexpr->args->head->data.ptr_value = param->node;
					if (sr_plan_log_usage)
						elog(sr_plan_log_usage, "sr_plan: restored parameter on %d", param->location);

					break;
				}
			}
		}

		return false;
	}

	return expression_tree_walker(node, sr_query_expr_walker, context);
}

static bool
sr_query_fake_const_expr_walker(Node *node, void *context)
{
	FuncExpr	*fexpr = (FuncExpr *) node;

	if (node == NULL)
		return false;

	if (IsA(node, FuncExpr) && fexpr->funcid == sr_plan_fake_func)
	{
		Const		   *fakeconst;

		fakeconst = makeConst(23, -1,  0, 4, (Datum) 0, false, true);
		fexpr->args = list_make1(fakeconst);
	}

	return expression_tree_walker(node, sr_query_fake_const_expr_walker, context);
}

static bool
sr_query_fake_const_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;

	// check for nodes that special work is required for, eg:
	if (IsA(node, FromExpr))
		return sr_query_fake_const_expr_walker(node, context);

	// for any node type not specially processed, do:
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, sr_query_fake_const_walker, context, 0);

	return false;
}

static Datum
get_query_hash(Query *node)
{
	Datum			result;
	Node		   *copy;
	MemoryContext	tmpctx,
					oldctx;
	char		   *temp;

	tmpctx = AllocSetContextCreate(CurrentMemoryContext,
									  "temporary context",
									  ALLOCSET_DEFAULT_SIZES);

	oldctx = MemoryContextSwitchTo(tmpctx);
	copy = copyObject((Node *) node);
	sr_query_fake_const_walker(copy, NULL);
	temp = nodeToString(copy);
	result = hash_any((unsigned char *) temp, strlen(temp));
	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(tmpctx);

	return result;
}

static const struct config_enum_entry log_usage_options[] = {
	{"none", 0, true},
	{"debug", DEBUG2, true},
	{"debug5", DEBUG5, false},
	{"debug4", DEBUG4, false},
	{"debug3", DEBUG3, false},
	{"debug2", DEBUG2, false},
	{"debug1", DEBUG1, false},
	{"log", LOG, false},
	{"info", INFO, true},
	{"notice", NOTICE, false},
	{"warning", WARNING, false},
	{NULL, 0, false}
};

void
_PG_init(void)
{
	DefineCustomBoolVariable("sr_plan.write_mode",
							 "Save all plans for all query.",
							 NULL,
							 &sr_plan_write_mode,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomEnumVariable("sr_plan.log_usage",
							 "Log cached plan usage with specified level",
							 NULL,
							 &sr_plan_log_usage,
							 0,
							 log_usage_options,
							 PGC_USERSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	srplan_planner_hook_next = planner_hook;
	planner_hook = &sr_planner;

	srplan_post_parse_analyze_hook_next	= post_parse_analyze_hook;
	post_parse_analyze_hook	= &sr_analyze;
}

void
_PG_fini(void)
{
	/* nothing to do */
}

Datum
_p(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

/*
 * Basic plan tree walker.
 *
 * 'visitor' is applied right before return.
 */
static void
plan_tree_visitor(Plan *plan,
				  void (*visitor) (Plan *plan, void *context),
				  void *context)
{
	ListCell   *l;

	if (plan == NULL)
		return;

	check_stack_depth();

	/* Plan-type-specific fixes */
	switch (nodeTag(plan))
	{
		case T_SubqueryScan:
			plan_tree_visitor(((SubqueryScan *) plan)->subplan, visitor, context);
			break;

		case T_CustomScan:
			foreach (l, ((CustomScan *) plan)->custom_plans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_ModifyTable:
			foreach (l, ((ModifyTable *) plan)->plans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_Append:
			foreach (l, ((Append *) plan)->appendplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_MergeAppend:
			foreach (l, ((MergeAppend *) plan)->mergeplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_BitmapAnd:
			foreach (l, ((BitmapAnd *) plan)->bitmapplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_BitmapOr:
			foreach (l, ((BitmapOr *) plan)->bitmapplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		default:
			break;
	}

	plan_tree_visitor(plan->lefttree, visitor, context);
	plan_tree_visitor(plan->righttree, visitor, context);

	/* Apply visitor to the current node */
	visitor(plan, context);
}

static void
execute_for_plantree(PlannedStmt *planned_stmt,
					 void (*proc) (void *context, Plan *plan),
					 void *context)
{
	ListCell	*lc;

	proc(context, planned_stmt->planTree);

	foreach (lc, planned_stmt->subplans)
	{
		Plan	*subplan = lfirst(lc);
		proc(context, subplan);
	}
}
