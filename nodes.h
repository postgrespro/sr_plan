#define __attribute__(x)
#include "pg_config.h"

#if PG_VERSION_NUM < 110000
typedef int __int128
#endif

#include "postgres.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
