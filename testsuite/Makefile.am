#RUNTESTDEFAULTFLAGS = --srcdir $$srcdir/testsuite

SUBDIRS = expect slurm_unit

EXTRA_DIST = macos/install-smoke.py macos/job-smoke.py macos/build-ci.sh \
	macos/pmi1-smoke.c macos/pmi2-smoke.c macos/pmix-smoke.c macos/privileged-smoke.py \
	macos/job-smoke-cleanup-test.py macos/accounting-smoke.py \
	macos/gpu-loader-probe.m macos/gpu-loader-host.c macos/footprint-workload.c \
	macos/privileged-cleanup-test.py \
	macos/privileged-observation-test.py macos/privileged-canary-test.py

DISTCLEANFILES = \
	slurm.sum slurm.log
