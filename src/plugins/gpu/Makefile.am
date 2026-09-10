# Makefile for gpu plugins

SUBDIRS = common generic


if LINUX_BUILD
SUBDIRS += nrt nvidia
endif

if BUILD_RSMI
SUBDIRS += rsmi
endif

if BUILD_NVML
SUBDIRS += nvml
endif

if BUILD_ONEAPI
SUBDIRS += oneapi
endif
