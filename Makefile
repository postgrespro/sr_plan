# contrib/sr_plan/Makefile

MODULE_big = sr_plan
OBJS = sr_plan.o serialize.o deserialize.o $(WIN32RES)

EXTENSION = sr_plan
DATA = sr_plan--1.0.sql sr_plan--unpackaged--1.0.sql
PGFILEDESC = "sr_plan - save and read plan"
EXTRA_CLEAN = serialize.c deserialize.c

REGRESS = sr_plan

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

gen_parser:
	python gen_parser.py nodes.h `$(PG_CONFIG) --includedir-server`
else
subdir = contrib/sr_plan
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk

gen_parser:
	python gen_parser.py nodes.h '$(top_srcdir)/src/include'
endif
