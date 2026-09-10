AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = task_darwin.la
task_darwin_la_SOURCES = task_darwin.c
task_darwin_la_LDFLAGS = -module -avoid-version --export-dynamic
