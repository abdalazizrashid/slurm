# Makefile for jobacct_gather/darwin plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = jobacct_gather_darwin.la

jobacct_gather_darwin_la_SOURCES = \
	jobacct_gather_darwin.c \
	darwin_proc.c \
	darwin_proc.h

jobacct_gather_darwin_la_LDFLAGS = $(PLUGIN_FLAGS)
jobacct_gather_darwin_la_LIBADD = ../common/libjobacct_gather_common.la

force:
$(jobacct_gather_darwin_la_LIBADD) : force
	@cd `dirname $@` && $(MAKE) `basename $@`
