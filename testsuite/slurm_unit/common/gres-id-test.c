/*****************************************************************************\
 * gres-id-test.c - identity-only GPU reconciliation, transport and allocation.
 * This file is part of Slurm. Distributed under GPL version 2 or later.
\*****************************************************************************/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>

/* Alias macros must precede declarations used by the included plugin. */
#include "src/common/slurm_xlator.h"

/* Exercise reconciliation without loading a hardware discovery plugin. */
#include "src/common/node_conf.h"
#include "src/common/xmalloc.h"
#include "src/plugins/gres/gpu/gres_gpu.c"

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, \
				__LINE__, #expr); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

static void _check_device(void *item, const char *identity, unsigned index)
{
	gres_device_t *device = item;
	CHECK(device);
	CHECK(device->index == index);
	CHECK(!device->path);
	CHECK(!xstrcmp(device->unique_id, identity));
	CHECK(device->dev_desc.type == DEV_TYPE_NONE);
	CHECK(device->dev_desc.major == NO_VAL);
	CHECK(device->dev_desc.minor == NO_VAL);
}

static void _test_node_bitmaps(list_t *records)
{
	buf_t *buffer = init_buf(1024);
	list_itr_t *itr = list_iterator_create(records);
	gres_slurmd_conf_t *conf;
	config_record_t config = { .gres = "gpu:2" };
	node_record_t node = { .name = "id-test", .config_ptr = &config };
	char *reason = NULL;

	/* Same node-registration layout as gres_node_config_pack(). */
	pack16(SLURM_PROTOCOL_VERSION, buffer);
	pack16(list_count(records), buffer);
	while ((conf = list_next(itr))) {
		pack32(GRES_MAGIC, buffer);
		pack64(conf->count, buffer);
		pack32(conf->cpu_cnt, buffer);
		/* Avoid loading a dynamic plugin in the generic bitmap test. */
		pack32(conf->config_flags | GRES_CONF_COUNT_ONLY, buffer);
		pack32(conf->plugin_id, buffer);
		packstr(conf->cpus, buffer);
		packstr(conf->links, buffer);
		packstr(conf->name, buffer);
		packstr(conf->type_name, buffer);
		packstr(conf->unique_id, buffer);
	}
	list_iterator_destroy(itr);
	set_buf_offset(buffer, 0);
	slurm_conf.gres_plugins = xstrdup("gpu");
	CHECK(gres_init() == SLURM_SUCCESS);
	gres_init_node_config(config.gres, &node.gres_list);
	CHECK(gres_node_config_unpack(buffer, node.name) == SLURM_SUCCESS);
	CHECK(gres_node_config_validate(&node, 1, 2, 1, false, &reason) ==
	      SLURM_SUCCESS);
	CHECK(!reason);
	gres_state_t *state = list_peek(node.gres_list);
	gres_node_state_t *resources = state->gres_data;
	CHECK(state->config_flags & GRES_CONF_HAS_ID);
	CHECK(!(state->config_flags & GRES_CONF_HAS_FILE));
	CHECK(resources->gres_cnt_avail == 2);
	CHECK(resources->gres_bit_alloc);
	CHECK(bit_size(resources->gres_bit_alloc) == 2);
	CHECK(resources->topo_cnt == 2);
	CHECK(bit_test(resources->topo_gres_bitmap[0], 0));
	CHECK(!bit_test(resources->topo_gres_bitmap[0], 1));
	CHECK(bit_test(resources->topo_gres_bitmap[1], 1));
	CHECK(!bit_test(resources->topo_gres_bitmap[1], 0));
	FREE_NULL_BUFFER(buffer);
	FREE_NULL_LIST(node.gres_list);
	xfree(node.gres);
	gres_fini();
	xfree(slurm_conf.gres_plugins);
}

static void _test_invalid_and_file_devices(void)
{
	list_t *records = list_create(destroy_gres_slurmd_conf);
	list_t *devices = NULL;
	node_config_load_t node = { .cpu_cnt = 2,
				    .in_slurmd = true,
				    .gres_name = "gpu" };
	gres_slurmd_conf_t conf = { .name = "gpu",
				    .count = 2,
				    .config_flags = GRES_CONF_HAS_ID,
				    .unique_id = "one-id" };

	add_gres_to_list(records, &conf);
	CHECK(gres_node_config_load(records, &node, &devices) == SLURM_ERROR);
	CHECK(!devices);
	list_flush(records);
	conf.count = 1;
	conf.unique_id = NULL;
	add_gres_to_list(records, &conf);
	CHECK(gres_node_config_load(records, &node, &devices) == SLURM_ERROR);
	CHECK(!devices);
	list_flush(records);

	/* Exercise the unchanged file-backed descriptor path with a real device. */
	conf.name = "test_device";
	conf.file = "/dev/null";
	conf.config_flags = GRES_CONF_HAS_FILE;
	node.gres_name = conf.name;
	add_gres_to_list(records, &conf);
	CHECK(gres_node_config_load(records, &node, &devices) == SLURM_SUCCESS);
	gres_device_t *device = list_peek(devices);
	CHECK(device && device->dev_desc.type == DEV_TYPE_CHAR);
	CHECK(!xstrcmp(device->path, "/dev/null"));
	CHECK(!device->unique_id);
	FREE_NULL_LIST(devices);
	list_flush(records);

	/* Metal configuration must fail before it can load a GPU backend. */
	conf.name = node.gres_name = "gpu";
	conf.file = NULL;
	conf.config_flags = GRES_CONF_ENV_NVML;
	add_gres_to_list(records, &conf);
	gres_autodetect_flags_set_gpu(GRES_AUTODETECT_GPU_METAL);
	CHECK(gres_p_node_config_load(records, &node) == ESLURM_INVALID_GRES);
	gres_autodetect_flags_set_gpu(GRES_AUTODETECT_UNSET);
	FREE_NULL_LIST(records);
}

int main(void)
{
	list_t *system = list_create(destroy_gres_slurmd_conf);
	list_t *config = list_create(destroy_gres_slurmd_conf);
	list_t *received = NULL;
	gres_slurmd_conf_t found = { .config_flags = GRES_CONF_HAS_ID |
						     GRES_CONF_ENV_METAL |
						     GRES_CONF_AUTODETECT,
				     .count = 1,
				     .cpu_cnt = 2,
				     .name = "gpu",
				     .type_name = "test_metal",
				     .unique_id = "0000000000000011" };
	gres_slurmd_conf_t requested = { .config_flags = GRES_CONF_ENV_DEF,
					 .count = 2,
					 .cpu_cnt = 2,
					 .name = "gpu" };
	node_config_load_t node = { .cpu_cnt = 2,
				    .in_slurmd = true,
				    .gres_name = "gpu" };
	buf_t *buffer = init_buf(1024);
	char **env = NULL;
	bitstr_t *allocated = bit_alloc(2);
	bitstr_t *usable = bit_alloc(2);

	add_gres_to_list(system, &found);
	found.unique_id = "0000000000000022";
	add_gres_to_list(system, &found);
	add_gres_to_list(config, &requested);
	_merge_system_gres_conf(config, system);
	CHECK(list_count(config) == 2);
	CHECK(list_count(system) == 0);
	CHECK(gres_node_config_load(config, &node, &gres_devices) ==
	      SLURM_SUCCESS);
	CHECK(list_count(gres_devices) == 2);
	list_itr_t *itr = list_iterator_create(gres_devices);
	_check_device(list_next(itr), "0000000000000011", 0);
	_check_device(list_next(itr), "0000000000000022", 1);
	list_iterator_destroy(itr);
	gres_send_stepd(buffer, gres_devices);
	set_buf_offset(buffer, 0);
	gres_recv_stepd(buffer, &received);
	CHECK(list_count(received) == 2);
	itr = list_iterator_create(received);
	_check_device(list_next(itr), "0000000000000011", 0);
	_check_device(list_next(itr), "0000000000000022", 1);
	list_iterator_destroy(itr);

	list_for_each(config, gres_common_set_env_types_on_node_flags,
		      &node_flags);
	CHECK(node_flags & GRES_CONF_ENV_METAL);
	bit_set(allocated, 1);
	gres_p_job_set_env(&env, allocated, 1, GRES_INTERNAL_FLAG_NONE);
	CHECK(!xstrcmp(getenvp(env, "SLURM_METAL_DEVICE_IDS"),
		       "0000000000000022"));
	CHECK(!xstrcmp(getenvp(env, "SLURM_JOB_GPUS"), "1"));
	CHECK(!xstrcmp(getenvp(env, "SLURM_GPUS_ON_NODE"), "1"));
	CHECK(!getenvp(env, "CUDA_VISIBLE_DEVICES"));
	bit_set(allocated, 0);
	bit_set(usable, 0);
	gres_p_task_set_env(&env, allocated, 2, usable,
			    GRES_INTERNAL_FLAG_NONE);
	CHECK(!xstrcmp(getenvp(env, "SLURM_METAL_DEVICE_IDS"),
		       "0000000000000011"));
	bit_clear_all(allocated);
	gres_p_step_set_env(&env, allocated, 0, GRES_INTERNAL_FLAG_NONE);
	CHECK(!getenvp(env, "SLURM_METAL_DEVICE_IDS"));
	CHECK(!getenvp(env, "SLURM_STEP_GPUS"));

	_test_node_bitmaps(config);
	/* Duplicate identities must never become separate allocatable devices. */
	found.type_name = NULL;
	add_gres_to_list(config, &found);
	CHECK(gres_node_config_load(config, &node, &received) == SLURM_ERROR);
	FREE_NULL_LIST(system);
	FREE_NULL_LIST(config);
	FREE_NULL_LIST(gres_devices);
	FREE_NULL_LIST(received);
	FREE_NULL_BUFFER(buffer);
	FREE_NULL_BITMAP(allocated);
	FREE_NULL_BITMAP(usable);
	env_array_free(env);
	_test_invalid_and_file_devices();
	puts("GRES identity reconciliation, transport, bitmaps and advisory selection passed");
	return EXIT_SUCCESS;
}
