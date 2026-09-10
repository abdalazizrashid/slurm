/*****************************************************************************\
 * topology-load-test.c - topology modules must load without controller state.
 * This file is part of Slurm. Distributed under GPL version 2 or later.
\*****************************************************************************/

#include "config.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/common/hostlist.h"
#include "src/common/plugin.h"
#include "src/common/read_config.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/plugins/topology/common/common_topo.h"

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, \
				__LINE__, #expr); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

int main(int argc, char **argv)
{
	const char *types[] = { "flat", "block", "ring", "tree", "torus3d" };

	CHECK(argc <= 2);
	/* The loader must not be rescued by test-only controller definitions. */
	CHECK(!dlsym(RTLD_DEFAULT, "idle_node_bitmap"));
	CHECK(!dlsym(RTLD_DEFAULT, "part_list"));
	slurm_conf.topology_param = xstrdup("RoutePart");

	for (int i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		plugin_handle_t plugin;
		char *path;
		int (*split)(hostlist_t *, hostlist_t ***, int *, uint16_t);
		int (*eval)(topology_eval_t *);
		hostlist_t *hosts = hostlist_create("n[1-6]"), **parts = NULL;
		int count, total = 0;
		job_details_t details = { 0 };
		job_record_t job = { .details = &details };
		topology_eval_t request = {
			.job_ptr = &job,
			.prefer_alloc_nodes = true,
			.node_map = bit_alloc(1),
		};

		if (argc == 2)
			path = xstrdup_printf("%s/topology_%s.so", argv[1],
					      types[i]);
		else
			path = xstrdup_printf(TOPOLOGY_BUILD_DIR
					      "/%s/.libs/topology_%s.so",
					      types[i], types[i]);
		CHECK(plugin_load_from_file(&plugin, path, true) ==
		      SLURM_SUCCESS);
		CHECK((split = dlsym(plugin,
				     "common_topo_split_hostlist_treewidth")));
		CHECK((eval = dlsym(plugin, "eval_nodes")));
		/* RoutePart must fall back to ordinary forwarding on a client. */
		CHECK(split(hosts, &parts, &count, 2) > 0);
		CHECK(count == 2);
		for (int j = 0; j < count; j++) {
			total += hostlist_count(parts[j]);
			hostlist_destroy(parts[j]);
		}
		CHECK(total == 6);
		CHECK(eval(&request) == ESLURM_NOT_SUPPORTED);
		FREE_NULL_BITMAP(request.node_map);
		xfree(parts);
		hostlist_destroy(hosts);
		plugin_unload(plugin);
		printf("topology/%s: load and client forwarding passed\n",
		       types[i]);
		xfree(path);
	}
	xfree(slurm_conf.topology_param);
	return EXIT_SUCCESS;
}
