AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir)
AM_OBJCFLAGS = $(METAL_OBJCFLAGS)
# Objective-C uses the C compiler/linker settings in Libtool.
AM_LIBTOOLFLAGS = --tag=CC

pkglib_LTLIBRARIES = gpu_metal.la
gpu_metal_la_SOURCES = gpu_metal.m
gpu_metal_la_LDFLAGS = -module -avoid-version --export-dynamic
gpu_metal_la_LIBADD = ../common/libgpu_common.la $(METAL_LIBS)
