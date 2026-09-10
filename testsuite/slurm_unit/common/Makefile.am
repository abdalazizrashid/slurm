AUTOMAKE_OPTIONS = foreign

SUBDIRS = bitstring \
	  hostlist \
	  slurm_protocol_defs \
	  slurm_protocol_pack \
	  slurmdb_defs \
	  slurmdb_pack

AM_CPPFLAGS = -I$(top_srcdir)
LDADD = -ldl -lpthread $(LIB_SLURM)

check_PROGRAMS = \
	$(TESTS)

TESTS = auth-fork-test \
	scontrol-job-id-test \
	fd-test \
	rlimits-test \
	parse-config-read-test \
	stepd-inventory-test \
	fetch_config-test \
	conmgr-delay-test \
	conmgr-quiesce-test \
	srun-pty-accept-test \
	log-test




if DARWIN_BUILD
TESTS += darwin-proc-test \
	darwin-pgid-test \
	topology-load-test \
	pmi-accept-test
# /bin/sh clears DYLD_* variables. Keep the shell/exec regression independent
# of libtool's temporary library search path and an older installed Slurm.
topology_load_test_CPPFLAGS = $(AM_CPPFLAGS) \
	-DTOPOLOGY_BUILD_DIR='"$(abs_top_builddir)/src/plugins/topology"'
darwin_proc_test_SOURCES = darwin-proc-test.c \
	$(top_srcdir)/src/plugins/jobacct_gather/darwin/darwin_proc.c
darwin_proc_test_LDADD =
if WITH_S2N
TESTS += tls-darwin-test
tls_darwin_test_CPPFLAGS = $(AM_CPPFLAGS) $(S2N_CPPFLAGS)
tls_darwin_test_LDADD = $(LDADD) $(S2N_LIBS)
tls_darwin_test_LDFLAGS = $(S2N_LDFLAGS)
endif
endif

if HAVE_CHECK
MYCFLAGS  = @CHECK_CFLAGS@ -Wall
MYCFLAGS += -D_ISO99_SOURCE
TESTS += xhash-test \
	 buf-test \
	 data-test \
	 dns-test \
	 http-test \
	 sluid-test \
	 xbase64-test \
	 xsched-test \
	 run-command-test \
	 jobacct-registration-test \
	 job-mem-limit-test \
	 privileges-test \
	 conmgr-signals-fork-test \
	 xstring-test \
	 parse_time-test \
	 pack-test \
	 reverse_tree-test \
	 xahash-test

xhash_test_CFLAGS = $(MYCFLAGS)
xhash_test_LDADD  = $(LDADD) @CHECK_LIBS@
xahash_test_CFLAGS = $(MYCFLAGS)
xahash_test_LDADD  = $(LDADD) @CHECK_LIBS@
buf_test_CFLAGS   = $(MYCFLAGS)
buf_test_LDADD    = $(LDADD) @CHECK_LIBS@
data_test_CFLAGS  = $(MYCFLAGS)
data_test_LDADD   = $(LDADD) @CHECK_LIBS@
dns_test_CFLAGS  = $(MYCFLAGS)
dns_test_LDADD   = $(LDADD) @CHECK_LIBS@
http_test_CFLAGS  = $(MYCFLAGS)
http_test_LDADD   = $(LDADD) @CHECK_LIBS@
sluid_test_CFLAGS = $(MYCFLAGS)
sluid_test_LDADD = $(LDADD) @CHECK_LIBS@
xbase64_test_CFLAGS = $(MYCFLAGS)
xbase64_test_LDADD = $(LDADD) @CHECK_LIBS@
xsched_test_CFLAGS = $(MYCFLAGS)
xsched_test_LDADD = $(LDADD) @CHECK_LIBS@
run_command_test_CFLAGS = $(MYCFLAGS)
run_command_test_LDADD = $(LDADD) @CHECK_LIBS@
jobacct_registration_test_CFLAGS = $(MYCFLAGS)
jobacct_registration_test_LDADD = $(LDADD) @CHECK_LIBS@
job_mem_limit_test_CFLAGS = $(MYCFLAGS)
job_mem_limit_test_LDADD = $(LDADD) @CHECK_LIBS@
privileges_test_CFLAGS = $(MYCFLAGS)
privileges_test_LDADD = $(LDADD) @CHECK_LIBS@
conmgr_signals_fork_test_CFLAGS = $(MYCFLAGS)
conmgr_signals_fork_test_LDADD = $(LDADD)
xstring_test_CFLAGS   = $(MYCFLAGS)
xstring_test_LDADD    = $(LDADD) @CHECK_LIBS@
parse_time_test_CFLAGS= $(MYCFLAGS)
parse_time_test_LDADD = $(LDADD) @CHECK_LIBS@
pack_test_CFLAGS = $(MYCFLAGS)
pack_test_LDADD = $(LDADD) @CHECK_LIBS@
reverse_tree_test_CFLAGS = $(MYCFLAGS)
reverse_tree_test_LDADD = $(LDADD) @CHECK_LIBS@

if DARWIN_BUILD
TESTS += darwin-accounting-test \
	darwin-proctrack-test
darwin_proctrack_test_CFLAGS = $(MYCFLAGS)
darwin_proctrack_test_LDADD = $(LDADD) @CHECK_LIBS@
darwin_accounting_test_CFLAGS = $(MYCFLAGS)
darwin_accounting_test_LDADD = $(LDADD) @CHECK_LIBS@ \
	$(top_builddir)/src/plugins/jobacct_gather/common/libjobacct_gather_common.la
endif

if HAVE_LUA

TESTS += lua-test

AM_CPPFLAGS += $(lua_CFLAGS)

lua_test_CFLAGS = $(MYCFLAGS) \
	-DLUA_TEST_SCRIPT=\"$(top_srcdir)/testsuite/slurm_unit/common/lua-test.lua\"
lua_test_LDADD = \
	$(LDADD) @CHECK_LIBS@ \
	$(top_builddir)/src/lua/libslurm_lua.la \
	$(lua_LIBS)

endif

endif
