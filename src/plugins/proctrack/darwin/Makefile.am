AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)

pkglib_LTLIBRARIES = proctrack_darwin.la
proctrack_darwin_la_SOURCES = proctrack_darwin.c
proctrack_darwin_la_LDFLAGS = -module -avoid-version --export-dynamic
