#!/bin/sh
# Build/install and run the portable component gate. GPL version 2 or later.
# Dependencies must already be installed. No daemon is started by this script.
set -eu

if test "$#" -ne 3; then
	echo "Usage: $0 BUILD_DIR INSTALL_PREFIX MUNGE_PREFIX" >&2
	exit 2
fi
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$1" "$2"
build_dir=$(CDPATH='' cd -- "$1" && pwd)
install_prefix=$(CDPATH='' cd -- "$2" && pwd)
munge_prefix=$(CDPATH='' cd -- "$3" && pwd)
if test "$source_dir" = "$build_dir"; then
	echo "Use a separate build directory" >&2
	exit 2
fi
mkdir -p "$build_dir/logs"
run_logged() {
	log_file=$1
	shift
	if "$@" >"$build_dir/logs/$log_file" 2>&1; then
		return 0
	fi
	tail -n 100 "$build_dir/logs/$log_file" >&2
	return 1
}

set -- "--prefix=$install_prefix" "--with-munge=$munge_prefix"
case $(uname -s) in
Darwin)
	export CC=${CC:-clang} CXX=${CXX:-clang++}
	brew_prefix=$(brew --prefix)
	ACLOCAL_PATH="$(brew --prefix glib)/share/aclocal:$brew_prefix/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
	PKG_CONFIG_PATH="$(brew --prefix check)/lib/pkgconfig:$(brew --prefix lua)/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	export ACLOCAL_PATH PKG_CONFIG_PATH
	set -- "$@" "--with-hwloc=$(brew --prefix hwloc)" \
		"--with-json=$(brew --prefix json-c)" \
		"--with-yaml=$(brew --prefix libyaml)" \
		"--with-lz4=$(brew --prefix lz4)" \
		"--with-libhttp-parser=$(brew --prefix http-parser)" \
		--enable-metal=yes --disable-x11
	jobs=$(sysctl -n hw.ncpu)
	;;
Linux)
	jobs=$(getconf _NPROCESSORS_ONLN)
	;;
*)
	echo "This gate covers Darwin and Linux" >&2
	exit 2
	;;
esac
export CFLAGS=${CFLAGS:--O0 -g}
export LDFLAGS="-L$munge_prefix/lib${LDFLAGS:+ $LDFLAGS}"
cd "$source_dir"
run_logged autoreconf.log autoreconf -fi
cd "$build_dir"
run_logged configure.log "$source_dir/configure" "$@"
run_logged build.log make -j "$jobs"
run_logged install.log make install
run_logged contrib-build.log make -j "$jobs" contrib
# PAM modules can have a system install directory independent of --prefix.
# Exercise every contrib installer inside a staging tree without activating it.
run_logged contrib-install.log make install-contrib \
	"DESTDIR=$build_dir/contrib-stage"
run_logged pmi.log make -C contribs/pmi all install
run_logged pmi2.log make -C contribs/pmi2 all install
if test "$(uname -s)" = Darwin; then
	run_logged macos-examples.log make -C contribs/macos install
fi
# Use the Makefile's configured tests and recurse into the packing suites.
# Automake retains hardware-dependent SKIPs in each test-suite.log and .trs.
run_logged unit.log make -C testsuite/slurm_unit/common check
run_logged smoke-cleanup.log python3 "$source_dir/testsuite/macos/job-smoke-cleanup-test.py"
run_logged smoke-python-syntax.log python3 -c \
	'import pathlib,sys; [compile(p.read_bytes(), str(p), "exec") for p in pathlib.Path(sys.argv[1]).glob("*.py")]' \
	"$source_dir/testsuite/macos"
python3 "$source_dir/testsuite/macos/install-smoke.py" \
	--prefix "$install_prefix" --report "$build_dir/logs/install-smoke.json"
