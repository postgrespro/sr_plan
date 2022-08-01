# contrib/sr_plan/Makefile

MODULE_big = sr_plan
OBJS = sr_plan.o $(WIN32RES)

PGFILEDESC = "sr_plan - save and read plan"

EXTENSION = sr_plan
EXTVERSION = 1.2
DATA_built = sr_plan--$(EXTVERSION).sql
DATA = sr_plan--1.0--1.1.sql sr_plan--1.1--1.2.sql

EXTRA_CLEAN = sr_plan--$(EXTVERSION).sql
#REGRESS = security sr_plan sr_plan_schema joins explain

ifdef USE_PGXS
ifndef PG_CONFIG
PG_CONFIG = pg_config
endif
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/sr_plan
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

$(EXTENSION)--$(EXTVERSION).sql: init.sql
	cat $^ > $@
