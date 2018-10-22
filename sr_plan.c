#include "sr_plan.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "catalog/pg_extension.h"
#include "catalog/indexing.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "access/hash.h"
#include "utils/lsyscache.h"
#include "utils/fmgrprotos.h"

#if PG_VERSION_NUM >= 100000
#include "utils/queryenvironment.h"
#include "catalog/index.h"
#endif

PG_MODULE_MAGIC;

void	_PG_init(void);
void	_PG_fini(void);

planner_hook_type				srplan_planner_hook_next			= NULL;
post_parse_analyze_hook_type	srplan_post_parse_analyze_hook_next	= NULL;
static bool sr_plan_write_mode = false;

static Oid sr_plan_fake_func = 0;
static Oid dropped_objects_func = 0;

static PlannedStmt *sr_planner(Query *parse, int cursorOptions,
								ParamListInfo boundParams);

static void sr_analyze(ParseState *pstate, Query *query);

static Oid get_sr_plan_schema(void);
static Oid sr_get_relname_oid(Oid schema_oid, const char *relname);
static bool sr_query_walker(Query *node, void *context);
static bool sr_query_expr_walker(Node *node, void *context);
void walker_callback(void *node);

struct QueryParams
{
	int location;
	void *node;
};

struct QueryParamsContext
{
	bool	collect;
	List   *params;
};

List *query_params;
const char *query_text;

static void
sr_analyze(ParseState *pstate, Query *query)
{
	query_text = pstate->p_sourcetext;
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

static PlannedStmt *
lookup_plan_by_query_hash(Snapshot snapshot, Relation sr_index_rel,
							Relation sr_plans_heap, ScanKey key, List *query_params)
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

		/* Check enabled and valid field */
		if (DatumGetBool(search_values[Anum_sr_enable - 1])
				&& DatumGetBool(search_values[Anum_sr_valid - 1]))
		{
			char *out = TextDatumGetCString(DatumGetTextP((search_values[3])));
			pl_stmt = stringToNode(out);
			/* TODO: replace params back */
			break;
		}
	}

	index_endscan(query_index_scan);
	return pl_stmt;
}

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
	char		   *temp,
				   *plan_text;
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

		schema_name = get_namespace_name(schema_oid);
		func_name_list = list_make2(makeString(schema_name), makeString("_p")); 
		sr_plan_fake_func = LookupFuncName(func_name_list, 1, args, true);
		list_free(func_name_list);
		pfree(schema_name);
	}

	sr_index_oid = sr_get_relname_oid(schema_oid, SR_PLANS_TABLE_QUERY_INDEX_NAME);
	sr_plans_oid = sr_get_relname_oid(schema_oid, SR_PLANS_TABLE_NAME);

	if (sr_plans_oid == InvalidOid || sr_index_oid == InvalidOid)
		elog(ERROR, "sr_plan extension installed incorrectly");

	/* Make list with all _p functions and his position */
	sr_query_walker((Query *) parse, &qp_context);
	temp = nodeToString(parse);
	query_hash = hash_any((unsigned char *) temp, strlen(temp));
	pfree(temp);
	ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT4EQ, query_hash);

	/* Try to find already planned statement */
	heap_lock = AccessShareLock;
	sr_plans_heap = heap_open(sr_plans_oid, heap_lock);
	sr_index_rel = index_open(sr_index_oid, heap_lock);

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	pl_stmt = lookup_plan_by_query_hash(snapshot, sr_index_rel, sr_plans_heap,
										&key, qp_context.params);
	if (pl_stmt != NULL)
		goto cleanup;

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
										&key, qp_context.params);
	if (pl_stmt != NULL)
		goto cleanup;

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
		heap_deform_tuple(htup, sr_plans_heap->rd_att,
						  search_values, search_nulls);

		/* Detect full plan duplicate */
		if (search_values[Anum_sr_plan_hash - 1] == plan_hash)
		{
			found = true;
			break;
		}
	}
	index_endscan(query_index_scan);

	if (!found)
	{
		Datum		values[Anum_sr_attcount];
		bool		nulls[Anum_sr_attcount];

		MemSet(nulls, 0, sizeof(nulls));
		values[Anum_sr_query_hash - 1] = query_hash;
		values[Anum_sr_plan_hash - 1] = plan_hash;
		values[Anum_sr_query - 1] = CStringGetTextDatum(query_text);
		values[Anum_sr_plan - 1] = CStringGetTextDatum(plan_text);
		values[Anum_sr_enable - 1] = BoolGetDatum(false);
		values[Anum_sr_valid - 1] = BoolGetDatum(true);

		tuple = heap_form_tuple(sr_plans_heap->rd_att, values, nulls);
		simple_heap_insert(sr_plans_heap, tuple);
#if PG_VERSION_NUM >= 100000
		index_insert(sr_index_rel,
					 values, nulls,
					 &(tuple->t_self),
					 sr_plans_heap,
					 UNIQUE_CHECK_NO,
					 NULL);
#else
		index_insert(sr_index_rel,
					 values, nulls,
					 &(tuple->t_self),
					 sr_plans_heap,
					 UNIQUE_CHECK_NO);
#endif

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

	if (qp_context->collect)
	{
		if (IsA(node, FuncExpr) && fexpr->funcid == sr_plan_fake_func)
		{
			struct QueryParams *param = (struct QueryParams *) palloc(sizeof(struct QueryParams));
			param->location = fexpr->location;
			param->node = fexpr->args->head->data.ptr_value;

			qp_context->params = lappend(qp_context->params, param);

			return false;
		}
	}
	else
	{
		ListCell *cell_params;

		foreach(cell_params, qp_context->params)
		{
			struct QueryParams *param = lfirst(cell_params);

			if (param->location == fexpr->location)
			{
				fexpr->args->head->data.ptr_value = param->node;
				break;
			}
		}
	}

	return expression_tree_walker(node, sr_query_expr_walker, context);
}

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

PG_FUNCTION_INFO_V1(_p);

Datum
_p(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

PG_FUNCTION_INFO_V1(explain_jsonb_plan);

Datum
explain_jsonb_plan(PG_FUNCTION_ARGS)
{
	Jsonb *jsonb_plan = PG_GETARG_JSONB_P(0);
	Node *plan;

	if (jsonb_plan == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("Not found jsonb arg"));

	plan = jsonb_to_node_tree(jsonb_plan, NULL, NULL);
	if (plan == NULL)
		PG_RETURN_TEXT_P(cstring_to_text("Not found right jsonb plan"));

	if (IsA(plan, PlannedStmt))
	{
		ExplainState *es = NewExplainState();
		es->costs = false;
		ExplainBeginOutput(es);
		PG_TRY();
		{
#if PG_VERSION_NUM >= 100000
			ExplainOnePlan((PlannedStmt *)plan, NULL,
					   es, NULL,
					   NULL, create_queryEnv(), NULL);
#else
			ExplainOnePlan((PlannedStmt *)plan, NULL,
					   es, NULL,
					   NULL, NULL);
#endif
			PG_RETURN_TEXT_P(cstring_to_text(es->str->data));
		}
		PG_CATCH();
		{
			/* Magic hack but work. In ExplainOnePlan we twice touched snapshot before die.*/
			UnregisterSnapshot(GetActiveSnapshot());
			UnregisterSnapshot(GetActiveSnapshot());
			PopActiveSnapshot();
			ExplainEndOutput(es);
			PG_RETURN_TEXT_P(cstring_to_text("Invalid plan"));
		}
		PG_END_TRY();
		ExplainEndOutput(es);
	}
	else
	{
		PG_RETURN_TEXT_P(cstring_to_text("Not found plan"));
	}
}


PG_FUNCTION_INFO_V1(sr_plan_invalid_table);

Datum
sr_plan_invalid_table(PG_FUNCTION_ARGS)
{
	FunctionCallInfoData fcinfo_new;
	ReturnSetInfo rsinfo;
	FmgrInfo	flinfo;
	ExprContext econtext;
	TupleTableSlot *slot = NULL;
	Relation sr_plans_heap;
	Oid sr_plans_oid;
	HeapScanDesc heapScan;
	Jsonb *jsonb;
	JsonbValue relation_key;

	econtext.ecxt_per_query_memory = CurrentMemoryContext;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))  /* internal error */
		elog(ERROR, "not fired by event trigger manager");

	sr_plans_oid = sr_get_relname_oid(InvalidOid, SR_PLANS_TABLE_NAME);
	if(sr_plans_oid == InvalidOid)
	{
		elog(ERROR, "Cannot find %s table", SR_PLANS_TABLE_NAME);
	}
	sr_plans_heap = heap_open(sr_plans_oid, RowExclusiveLock);

	relation_key.type = jbvString;
	relation_key.val.string.len = strlen("relationOids");
	relation_key.val.string.val = "relationOids";

	rsinfo.type = T_ReturnSetInfo;
	rsinfo.econtext = &econtext;
	//rsinfo.expectedDesc = fcache->funcResultDesc;
	rsinfo.allowedModes = (int) (SFRM_ValuePerCall | SFRM_Materialize);
	/* note we do not set SFRM_Materialize_Random or _Preferred */
	rsinfo.returnMode = SFRM_Materialize;
	/* isDone is filled below */
	rsinfo.setResult = NULL;
	rsinfo.setDesc = NULL;

	if (!dropped_objects_func)
	{
		Oid args[1];
		dropped_objects_func = LookupFuncName(list_make1(makeString("pg_event_trigger_dropped_objects")), 0, args, true);
	}

	/* Look up the function */
	fmgr_info(dropped_objects_func, &flinfo);

	InitFunctionCallInfoData(fcinfo_new, &flinfo, 0, InvalidOid, NULL, (fmNodePtr)&rsinfo);
	(*pg_event_trigger_dropped_objects) (&fcinfo_new);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo_new.isnull)
		elog(ERROR, "function %p returned NULL", (void *) pg_event_trigger_dropped_objects);

	slot = MakeTupleTableSlotCompat();
	ExecSetSlotDescriptor(slot, rsinfo.setDesc);

	while(tuplestore_gettupleslot(rsinfo.setResult, true,
						false, slot))
	{
		Datum		search_values[6];
		bool		search_nulls[6];
		bool		search_replaces[6];

		HeapTuple local_tuple;

		bool isnull = false;
		bool find_plan = false;
		int droped_relation_oid = DatumGetInt32(slot_getattr(slot, 2, &isnull));
		char *type_name = TextDatumGetCString(slot_getattr(slot, 7, &isnull));

		heapScan = heap_beginscan(sr_plans_heap, SnapshotSelf, 0, (ScanKey) NULL);

		while ((local_tuple = heap_getnext(heapScan, ForwardScanDirection)) != NULL)
		{
			heap_deform_tuple(local_tuple, sr_plans_heap->rd_att,
							  search_values, search_nulls);

			if (DatumGetBool(search_values[5])) {
				int type;
				JsonbValue v;
				JsonbIterator *it;
				JsonbValue *node_relation;
				HeapTuple newtuple;

				jsonb = (Jsonb *)DatumGetPointer(PG_DETOAST_DATUM(search_values[3]));

				/*TODO: need move to function*/
				if (strcmp(type_name, "table") == 0)
				{
					node_relation = findJsonbValueFromContainer(&jsonb->root,
												JB_FOBJECT,
												&relation_key);
					it = JsonbIteratorInit(node_relation->val.binary.data);
					while ((type = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
					{
						if (type == WJB_ELEM)
						{
							int oid = DatumGetInt32(DirectFunctionCall1(numeric_int4, NumericGetDatum(v.val.numeric)));
							if (oid == droped_relation_oid)
							{
								find_plan = true;
								break;
							}
						}
					}
				}
				else if (strcmp(type_name, "index") == 0)
				{
					it = JsonbIteratorInit(&jsonb->root);
					while ((type = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
					{
						if (type == WJB_KEY &&
							v.type == jbvString &&
							strncmp(v.val.string.val, "indexid", v.val.string.len) == 0)
						{
							type = JsonbIteratorNext(&it, &v, false);
							if (type == WJB_DONE)
								break;
							if (type == WJB_VALUE)
							{
								int oid = DatumGetInt32(DirectFunctionCall1(numeric_int4, NumericGetDatum(v.val.numeric)));
								if (oid == droped_relation_oid)
								{
									find_plan = true;
									break;
								}
							}
						}
					}
				}
				if (find_plan)
				{
					/* update existing entry */
					MemSet(search_replaces, 0, sizeof(search_replaces));

					search_values[Anum_sr_valid - 1] = BoolGetDatum(false);
					search_replaces[Anum_sr_valid - 1] = true;

					newtuple = heap_modify_tuple(local_tuple, RelationGetDescr(sr_plans_heap),
										 search_values, search_nulls, search_replaces);
					simple_heap_update(sr_plans_heap, &newtuple->t_self, newtuple);
				}
			}
		}
		heap_endscan(heapScan);
	}
	heap_close(sr_plans_heap, RowExclusiveLock);

	PG_RETURN_NULL();
}
