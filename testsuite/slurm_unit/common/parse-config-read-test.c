/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

static int reads_before_failure = -1;
static bool stream_unbuffered;
static bool failure_injected;

static char *_test_fgets(char *buf, int size, FILE *file)
{
	if (reads_before_failure >= 0) {
		if (!stream_unbuffered) {
			assert(!setvbuf(file, NULL, _IONBF, 0));
			stream_unbuffered = true;
		}
		if (!reads_before_failure) {
			int fd = open("/dev/null", O_WRONLY);

			assert(fd >= 0);
			assert(dup2(fd, fileno(file)) == fileno(file));
			assert(!close(fd));
			reads_before_failure = -1;
			failure_injected = true;
		} else {
			reads_before_failure--;
		}
	}
	return fgets(buf, size, file);
}

/*
 * Exercise the real parser and stdio error indicator. Only this translation
 * unit substitutes fgets; the injected failure replaces its stream descriptor
 * with a write-only descriptor after the requested number of successful reads.
 * Give the included parser private names and omit its aliases so this test can
 * also link with the monolithic libslurm.o used by builds without shared libs.
 */
#define strong_alias(name, aliasname)
#define conf_includes_list test_conf_includes_list
#define _hashtbl_copy_keys test_hashtbl_copy_keys
#define s_p_hashtbl_create_cnt test_s_p_hashtbl_create_cnt
#define s_p_hashtbl_create test_s_p_hashtbl_create
#define s_p_hashtbl_destroy test_s_p_hashtbl_destroy
#define s_p_parse_line test_s_p_parse_line
#define s_p_parse_file test_s_p_parse_file
#define s_p_parse_buffer test_s_p_parse_buffer
#define s_p_hashtbl_merge test_s_p_hashtbl_merge
#define s_p_hashtbl_merge_override test_s_p_hashtbl_merge_override
#define s_p_hashtbl_merge_keys test_s_p_hashtbl_merge_keys
#define s_p_parse_line_complete test_s_p_parse_line_complete
#define s_p_parse_line_expanded test_s_p_parse_line_expanded
#define s_p_parse_pair_with_op test_s_p_parse_pair_with_op
#define s_p_parse_pair test_s_p_parse_pair
#define s_p_get_string test_s_p_get_string
#define s_p_get_long test_s_p_get_long
#define s_p_get_uint16 test_s_p_get_uint16
#define s_p_get_uint32 test_s_p_get_uint32
#define s_p_get_uint64 test_s_p_get_uint64
#define s_p_get_operator test_s_p_get_operator
#define s_p_get_pointer test_s_p_get_pointer
#define s_p_get_array test_s_p_get_array
#define s_p_get_line test_s_p_get_line
#define s_p_get_expline test_s_p_get_expline
#define s_p_get_boolean test_s_p_get_boolean
#define s_p_get_float test_s_p_get_float
#define s_p_get_double test_s_p_get_double
#define s_p_get_long_double test_s_p_get_long_double
#define s_p_dump_values test_s_p_dump_values
#define s_p_pack_hashtbl test_s_p_pack_hashtbl
#define s_p_unpack_hashtbl_full test_s_p_unpack_hashtbl_full
#define s_p_unpack_hashtbl test_s_p_unpack_hashtbl
#define transfer_s_p_options test_transfer_s_p_options
#define fgets _test_fgets
#include "src/common/parse_config.c"
#undef fgets
#undef strong_alias

static void _write_config(char *path, const char *text)
{
	FILE *file = fopen(path, "w");

	assert(file);
	assert(fputs(text, file) >= 0);
	assert(!fclose(file));
}

static void _parse(const char *name, char *path, int fail_after,
		   int expected_rc, bool first, bool second)
{
	s_p_options_t options[] = { { "First", S_P_UINT32 },
				    { "Second", S_P_UINT32 },
				    { NULL } };
	s_p_hashtbl_t *table = s_p_hashtbl_create(options);
	uint32_t value;
	int rc;

	reads_before_failure = fail_after;
	stream_unbuffered = false;
	failure_injected = false;
	rc = s_p_parse_file(table, NULL, path, 0, NULL);
	assert(rc == expected_rc);
	assert(failure_injected == (fail_after >= 0));
	if (failure_injected)
		assert(errno == EBADF);
	assert(s_p_get_uint32(&value, "First", table) == first);
	if (first)
		assert(value == 60);
	assert(s_p_get_uint32(&value, "Second", table) == second);
	if (second)
		assert(value == 128);
	s_p_hashtbl_destroy(table);
	printf("%s: PASS\n", name);
}

int main(void)
{
	char directory[] = "/tmp/slurm-parse-config-read-XXXXXX";
	char *path, *include;

	alarm(10);
	assert(mkdtemp(directory));
	path = xstrdup_printf("%s/test.conf", directory);
	_write_config(path, "First=60\nSecond=128\n");
	_parse("regular file", path, -1, SLURM_SUCCESS, true, true);
	_parse("initial read error", path, 0, SLURM_ERROR, false, false);
	_parse("read error after complete line", path, 1, SLURM_ERROR, true,
	       false);

	_write_config(path, "First=6\\\n0\nSecond=128\n");
	_parse("regular continuation", path, -1, SLURM_SUCCESS, true, true);
	_parse("read error during continuation", path, 1, SLURM_ERROR, false,
	       false);

	_write_config(path, "First=60");
	_parse("EOF without newline", path, -1, SLURM_SUCCESS, true, false);
	_write_config(path, "First=60 \\\n");
	_parse("EOF after continuation", path, -1, SLURM_SUCCESS, true, false);
	_write_config(path, "");
	_parse("empty regular file", path, -1, SLURM_SUCCESS, false, false);
	if (geteuid()) {
		assert(!chmod(path, 0));
		_parse("unreadable empty regular file", path, -1, SLURM_ERROR,
		       false, false);
		assert(!chmod(path, 0600));
	}
	_parse("directory", directory, -1, SLURM_ERROR, false, false);

	include = xstrdup_printf("Include %s\n", directory);
	_write_config(path, include);
	_parse("included directory", path, -1, SLURM_ERROR, false, false);
	xfree(include);

	assert(!unlink(path));
	assert(!rmdir(directory));
	xfree(path);
	return 0;
}
