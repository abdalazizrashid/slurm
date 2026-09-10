/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#undef NDEBUG
#include <assert.h>

#define plugin_load_from_file test_plugin_load_from_file
#define plugin_peek test_plugin_peek
#include "src/common/spank.c"
#undef plugin_load_from_file
#undef plugin_peek

static int lookups, loads;

int test_plugin_load_from_file(plugin_handle_t *plugin, const char *path,
			       bool required)
{
	loads++;
	return SLURM_ERROR;
}

int test_plugin_peek(const char *path, char *type, const size_t size)
{
	lookups++;
	return SLURM_ERROR;
}

static int _line(struct spank_stack *stack, const char *text)
{
	char *copy = xstrdup(text);
	int rc = _spank_stack_process_line(stack, "test.conf", 1, copy);
	xfree(copy);
	return rc;
}

int main(void)
{
	struct spank_stack stack = { .type = S_TYPE_REMOTE,
				     .plugin_path = "/nonexistent" };
	char directory[] = "/tmp/slurm-spank-policy-XXXXXX";
	char *path, *include;
	FILE *file;

	spank_require_empty_remote_stack(true);
	assert(!_line(&stack, "  # no plugin"));
	assert(_line(&stack, "required relative.so arg1 arg2") == -1);
	assert(errno == ENOTSUP);
	assert(_line(&stack, "optional /absolute.so") == -1);
	assert(errno == ENOTSUP);
	assert(!loads && !lookups);

	assert(mkdtemp(directory));
	path = xstrdup_printf("%s/child.conf", directory);
	file = fopen(path, "w");
	assert(file);
	assert(fputs("optional relative.so\n", file) >= 0);
	assert(!fclose(file));
	include = xstrdup_printf("include %s/*.conf", directory);
	assert(_line(&stack, include) == -1);
	assert(!loads && !lookups);
	assert(!unlink(path));
	assert(!rmdir(directory));
	xfree(include);
	xfree(path);

	/* The restriction does not change the local client stack. */
	stack.type = S_TYPE_LOCAL;
	assert(!_line(&stack, "optional /absolute.so"));
	assert(loads == 1);
	stack.type = S_TYPE_REMOTE;
	spank_require_empty_remote_stack(false);
	assert(!_line(&stack, "optional /absolute.so"));
	assert(loads == 2);
	puts("SPANK policy rejects remote code before lookup/loading: PASS");
	return 0;
}
