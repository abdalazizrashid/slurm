/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config.h"
#undef NDEBUG
#include <assert.h>

#include "src/interfaces/runtime.c"

int main(void)
{
	plugin_context_t context = { .type = "runtime/none" };

	assert(!runtime_g_is_none());
	g_context = &context;
	assert(!runtime_g_is_none()); /* A context alone is not initialization. */
	plugin_inited = PLUGIN_INITED;
	assert(runtime_g_is_none());
	context.type = "runtime/oci";
	assert(!runtime_g_is_none());
	context.type = "runtime/none-extra";
	assert(!runtime_g_is_none());
	g_context = NULL;
	assert(!runtime_g_is_none());
	plugin_inited = PLUGIN_NOT_INITED;
	return 0;
}
