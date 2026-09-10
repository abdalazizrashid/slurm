AUTOMAKE_OPTIONS = foreign

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_builddir)
AM_OBJCFLAGS = $(METAL_OBJCFLAGS)
AM_LIBTOOLFLAGS = --tag=CC

if BUILD_METAL
pkglibexec_PROGRAMS = slurm-darwin-gpu-probe slurm-darwin-metal-probe

# The C launcher deliberately has no Objective-C or Metal dependency. It fixes
# the worker path at build time and applies the policy before that image loads.
slurm_darwin_gpu_probe_SOURCES = gpu_probe.c \
	$(top_srcdir)/src/common/darwin_sandbox.c
slurm_darwin_gpu_probe_CPPFLAGS = $(AM_CPPFLAGS) \
	-DDARWIN_METAL_PROBE_PATH=\"$(pkglibexecdir)/slurm-darwin-metal-probe\"
slurm_darwin_gpu_probe_LDADD =

slurm_darwin_metal_probe_SOURCES = metal_probe.m
slurm_darwin_metal_probe_LDADD = $(METAL_LIBS)
endif
