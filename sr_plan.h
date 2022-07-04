#ifndef ___SR_PLAN_H__
#define ___SR_PLAN_H__

#include "postgres.h"
#include "fmgr.h"
#include "string.h"
#include "optimizer/planner.h"
#include "nodes/print.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/jsonb.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#if PG_VERSION_NUM < 120000
#include "utils/tqual.h"
#endif
#include "utils/guc.h"
#include "utils/datum.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/fmgroids.h"
#include "portability/instr_time.h"
#include "storage/lock.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "parser/analyze.h"
#include "parser/parse_func.h"
#include "tcop/utility.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "utils/syscache.h"
#include "funcapi.h"

#define SR_PLANS_TABLE_NAME	"sr_plans"
#define SR_PLANS_TABLE_QUERY_INDEX_NAME	"sr_plans_query_hash_idx"
#define SR_PLANS_RELOIDS_INDEX "sr_plans_query_oids"
#define SR_PLANS_INDEX_RELOIDS_INDEX "sr_plans_query_index_oids"

typedef void *(*deserialize_hook_type) (void *, void *);
void *jsonb_to_node_tree(Jsonb *json, deserialize_hook_type hook_ptr, void *context);

Jsonb *node_tree_to_jsonb(const void *obj, Oid fake_func, bool skip_location_from_node);
void common_walker(const void *obj, void (*callback) (void *));

/*
 * MakeTupleTableSlot()
 */
#if PG_VERSION_NUM >= 110000
#define MakeTupleTableSlotCompat() \
	MakeTupleTableSlot(NULL)
#else
#define MakeTupleTableSlotCompat() \
	MakeTupleTableSlot()
#endif

#if PG_VERSION_NUM >= 100000
#define index_insert_compat(rel,v,n,t,h,u) \
	index_insert(rel,v,n,t,h,u, BuildIndexInfo(rel))
#else
#define index_insert_compat(rel,v,n,t,h,u) index_insert(rel,v,n,t,h,u)
#endif

#ifndef PG_GETARG_JSONB_P
#define PG_GETARG_JSONB_P(x)	PG_GETARG_JSONB(x)
#endif

#ifndef PG_RETURN_JSONB_P
#define PG_RETURN_JSONB_P(x)	PG_RETURN_JSONB(x)
#endif

#ifndef DatumGetJsonbP
#define DatumGetJsonbP(d)	DatumGetJsonb(d)
#endif

enum
{
	Anum_sr_query_hash = 1,
	Anum_sr_query_id,
	Anum_sr_plan_hash,
	Anum_sr_enable,
	Anum_sr_query,
	Anum_sr_plan,
	Anum_sr_reloids,
	Anum_sr_index_reloids,
	Anum_sr_attcount
} sr_plans_attributes;

#endif
