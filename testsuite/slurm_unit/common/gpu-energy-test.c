/*****************************************************************************\
 * gpu-energy-test.c - unavailable GPU sensors must not become energy values.
 * This file is part of Slurm. Distributed under GPL version 2 or later.
\*****************************************************************************/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>

#include "src/common/slurm_xlator.h"

/* Install one provider in the real interface, bypassing plugin loading. */
#include "src/common/assoc_mgr.h"
#include "src/interfaces/acct_gather_energy.c"
#include "src/plugins/acct_gather_energy/gpu/acct_gather_energy_gpu.c"
#include "src/plugins/jobacct_gather/common/common_jag.h"

#ifdef __APPLE__
slurmd_conf_t *conf;
#endif

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, \
				__LINE__, #expr); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

static unsigned sample;
static unsigned provider_samples;
static bool provider_gpu = true;
static bool empty_container;
static acct_gather_energy_t provider_energy;
static void _check_unavailable(acct_gather_energy_t *energy);

int acct_gather_profile_g_get(enum acct_gather_profile_info type, void *data)
{
	*(uint32_t *) data = 0;
	return SLURM_SUCCESS;
}

int acct_gather_filesystem_g_get_data(acct_gather_data_t *data)
{
	return SLURM_SUCCESS;
}

int acct_gather_interconnect_g_get_data(acct_gather_data_t *data)
{
	return SLURM_SUCCESS;
}

int proctrack_g_get_pids(uint64_t cont_id, pid_t **pids, int *count)
{
	CHECK(empty_container);
	*pids = NULL;
	*count = 0;
	return SLURM_SUCCESS;
}

static int _get_provider_energy(enum acct_energy_type type, void *data)
{
	CHECK(type == ENERGY_DATA_NODE_ENERGY_UP);
	provider_samples++;
	if (provider_gpu)
		_get_node_energy(data);
	else
		memcpy(data, &provider_energy, sizeof(provider_energy));
	return SLURM_SUCCESS;
}

/* Keep a process sample available on both the Linux and native paths. */
static list_t *_get_test_precs(list_t *tasks, uint64_t cont_id,
			       jag_callbacks_t *callbacks)
{
	jag_prec_t *prec = list_peek(prec_list);

	if (!prec) {
		prec = xmalloc(sizeof(*prec));
		prec->pid = 123;
		prec->tres_count = g_tres_count;
		prec->tres_data =
			xcalloc(g_tres_count, sizeof(acct_gather_data_t));
		list_append(prec_list, prec);
	}
	for (int i = 0; i < prec->tres_count; i++) {
		prec->tres_data[i].size_read = INFINITE64;
		prec->tres_data[i].size_write = INFINITE64;
	}
	return prec_list;
}

static void _get_test_offspring(list_t *precs, jag_prec_t *ancestor, pid_t pid,
				jag_prec_t *permanent_ancestor)
{
}

static void _check_usage(jobacctinfo_t *record, uint64_t energy, uint64_t power,
			 uint64_t max_energy, uint64_t max_power)
{
	CHECK(record->tres_usage_in_tot[TRES_ARRAY_ENERGY] == energy);
	CHECK(record->tres_usage_out_tot[TRES_ARRAY_ENERGY] == power);
	CHECK(record->tres_usage_in_max[TRES_ARRAY_ENERGY] == max_energy);
	CHECK(record->tres_usage_in_min[TRES_ARRAY_ENERGY] == max_energy);
	CHECK(record->tres_usage_out_max[TRES_ARRAY_ENERGY] == max_power);
	CHECK(record->tres_usage_out_min[TRES_ARRAY_ENERGY] == max_power);
}

static void _check_accounting_consumer(pid_t pid)
{
	slurm_acct_gather_energy_ops_t provider_ops = {
		.get_data = _get_provider_energy,
	};
	jag_callbacks_t callbacks = {
		.get_precs = _get_test_precs,
		.get_offspring_data = _get_test_offspring,
	};
	list_t *records = list_create(jobacctinfo_destroy);
	jobacctinfo_t *record;

	/* Exercise the real single-provider dispatch on every accounting poll. */
	g_context_num = 1;
	ops = &provider_ops;
	provider_gpu = true;
	provider_samples = 0;
	g_tres_count = TRES_ARRAY_TOTAL_CNT;
	jag_common_init(1);
	record = jobacctinfo_create(NULL);
	record->pid = pid;
#ifdef __APPLE__
	record->id.record_id = 0;
#endif
	list_append(records, record);

	_energy_unavailable(&gpus[0].energy);
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, INFINITE64, INFINITE64, INFINITE64, INFINITE64);
	_check_unavailable(&record->energy);

	gpus[0].energy = (acct_gather_energy_t) {
		.consumed_energy = 60,
		.current_watts = 40,
	};
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, 60, 40, 60, 40);

	/* A failed GPU sample must not poison either historical maximum. */
	_energy_unavailable(&gpus[0].energy);
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, INFINITE64, INFINITE64, 60, 40);
	_check_unavailable(&record->energy);

	/* GPU recovery starts a new consumed-energy baseline at zero. */
	gpus[0].energy = (acct_gather_energy_t) { .current_watts = 12 };
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, 0, 12, 60, 40);
	gpus[0].energy.consumed_energy = 100;
	gpus[0].energy.current_watts = 50;
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, 100, 50, 100, 50);

	/* Providers can expose energy and power independently. */
	provider_gpu = false;
	provider_energy = (acct_gather_energy_t) {
		.consumed_energy = 120,
		.current_watts = NO_VAL,
	};
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, 120, INFINITE64, 120, 50);
	CHECK(record->energy.current_watts == NO_VAL);
	provider_energy.consumed_energy = NO_VAL64;
	provider_energy.current_watts = 70;
	jag_common_poll_data(records, 1, &callbacks, false);
	_check_usage(record, INFINITE64, 70, 120, 70);
	CHECK(record->energy.consumed_energy == NO_VAL64);
	CHECK(provider_samples == 7);

	if (pid) {
		/* The default collector updates totals before an empty poll exits. */
		FREE_NULL_LIST(prec_list);
		prec_list = list_create(destroy_jag_prec);
		callbacks = (jag_callbacks_t) { 0 };
		empty_container = true;
		provider_gpu = true;
		_energy_unavailable(&gpus[0].energy);
		jag_common_poll_data(records, 1, &callbacks, false);
		_check_usage(record, INFINITE64, INFINITE64, 120, 70);
		_check_unavailable(&record->energy);
		gpus[0].energy = (acct_gather_energy_t) {
			.consumed_energy = 80,
			.current_watts = 30,
		};
		jag_common_poll_data(records, 1, &callbacks, false);
		_check_usage(record, 80, 30, 120, 70);
		CHECK(provider_samples == 9);
		empty_container = false;
	}

	FREE_NULL_LIST(records);
	jag_common_fini();
	ops = NULL;
	g_context_num = -1;
}

/* Inject sensor data without contacting a daemon. */
int slurm_get_node_energy(char *host, uint16_t id, uint16_t delta,
			  uint16_t *count, acct_gather_energy_t **energy)
{
	*count = 1;
	*energy = xmalloc(sizeof(**energy));
	if (sample == 1)
		_energy_unavailable(*energy);
	else {
		(*energy)->current_watts = 10;
		(*energy)->consumed_energy =
			sample ? 200 + 20 * (sample - 2) : 100;
		(*energy)->poll_time = time(NULL);
	}
	sample++;
	return SLURM_SUCCESS;
}

static void _check_unavailable(acct_gather_energy_t *energy)
{
	CHECK(energy->current_watts == NO_VAL);
	CHECK(energy->ave_watts == NO_VAL);
	CHECK(energy->consumed_energy == NO_VAL64);
	CHECK(energy->base_consumed_energy == NO_VAL64);
	CHECK(energy->previous_consumed_energy == NO_VAL64);
}

int main(void)
{
	gpu_status_t sensors[2] = { 0 };
	acct_gather_energy_t total;
	slurmd_conf_t daemon_conf = { .node_name = "energy-test" };

	gpus = sensors;
	gpus_len = 2;
	_get_node_energy(&total);
	CHECK(total.current_watts == 0 && total.consumed_energy == 0);
	sensors[0].energy.current_watts = 10;
	sensors[0].energy.consumed_energy = 20;
	sensors[1].energy.current_watts = 30;
	sensors[1].energy.consumed_energy = 40;
	_get_node_energy(&total);
	CHECK(total.current_watts == 40 && total.consumed_energy == 60);
	_energy_unavailable(&sensors[0].energy);
	_get_node_energy(&total);
	_check_unavailable(&total);
	_energy_unavailable(&sensors[1].energy);
	_get_node_energy(&total);
	_check_unavailable(&total);
	sensors[0].last_update_watt = 12;
	_update_energy(&sensors[0], 0);
	CHECK(sensors[0].energy.current_watts == 12);
	CHECK(sensors[0].energy.consumed_energy == 0);
	CHECK(sensors[0].energy.base_consumed_energy == 0);
	sensors[0].previous_update_time = 1;
	sensors[0].last_update_time = 3;
	_update_energy(&sensors[0], 1);
	CHECK(sensors[0].energy.consumed_energy == 24);

	gpus_len = 1;
	_check_accounting_consumer(123);
	_check_accounting_consumer(0);

	/* Invalid samples propagate to step accounting without arithmetic. */
	gpus = NULL;
	gpus_len = 0;
	context_id = 0;
	conf = &daemon_conf;
	slurm_conf.gres_plugins = xstrdup("gpu");
	CHECK(gres_init() == SLURM_SUCCESS);
	CHECK(_get_joules_task(0) == SLURM_SUCCESS);
	CHECK(gpus[0].energy.consumed_energy == 0);
	CHECK(_get_joules_task(0) == SLURM_SUCCESS);
	_check_unavailable(&gpus[0].energy);
	CHECK(start_current_energies[0] == NO_VAL64);
	CHECK(_get_joules_task(0) == SLURM_SUCCESS);
	CHECK(gpus[0].energy.consumed_energy == 0);
	CHECK(_get_joules_task(0) == SLURM_SUCCESS);
	CHECK(gpus[0].energy.consumed_energy >= 20);
	CHECK(gpus[0].energy.consumed_energy < 100);
	xfree(gpus);
	xfree(start_current_energies);
	gres_fini();
	xfree(slurm_conf.gres_plugins);
	puts("GPU energy single-provider accounting, unavailable samples and recovery passed");
	return EXIT_SUCCESS;
}
