# Makefile for jobacct plugins

SUBDIRS = common

if DARWIN_BUILD
SUBDIRS += darwin
endif

if LINUX_BUILD
SUBDIRS += linux
endif

if WITH_CGROUP
SUBDIRS += cgroup
endif
