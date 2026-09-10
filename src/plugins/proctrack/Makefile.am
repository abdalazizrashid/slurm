# Makefile for proctrack plugins

SUBDIRS = pgid

if DARWIN_BUILD
SUBDIRS += darwin
endif

if LINUX_BUILD
SUBDIRS += linuxproc
endif

if WITH_CGROUP
SUBDIRS += cgroup
endif
