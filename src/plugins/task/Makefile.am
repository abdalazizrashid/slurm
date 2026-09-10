# Makefile for task plugins

SUBDIRS =

if DARWIN_BUILD
SUBDIRS += darwin
endif

if !DARWIN_BUILD
SUBDIRS += affinity
endif

if WITH_CGROUP
SUBDIRS += cgroup
endif
