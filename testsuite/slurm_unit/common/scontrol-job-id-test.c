/*****************************************************************************\
 * scontrol-job-id-test.c - bounded job-ID iteration across strtok_r variants.
 * This file is part of Slurm. Distributed under GPL version 2 or later.
\*****************************************************************************/

#include "src/scontrol/scontrol.h"

static char *_test_strtok_r(char *str, const char *delim, char **save);
#define strtok_r _test_strtok_r
#include "src/scontrol/update_job.c"
#undef strtok_r

#define CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, \
				__LINE__, #expr); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

int exit_code, quiet_flag;
uint32_t euid = SLURM_AUTH_NOBODY;
static bool null_at_end;
static const char **expected_ids;
static size_t expected_count, rpc_count;

/* Both save-pointer states are permitted after returning the last token. */
static char *_test_strtok_r(char *str, const char *delim, char **save)
{
	char *token = strtok_r(str, delim, save);

	if (token && (!*save || !**save))
		*save = null_at_end ? NULL : token + strlen(token);
	return token;
}

/* Run the real suspend/resume loops without contacting any controller. */
extern int slurm_suspend2(char *job_id, job_array_resp_msg_t **resp)
{
	CHECK(rpc_count <
	      expected_count); /* Bounds a repeated-final-token bug. */
	CHECK(!strcmp(job_id, expected_ids[rpc_count++]));
	*resp = NULL;
	return SLURM_SUCCESS;
}

extern int slurm_resume2(char *job_id, job_array_resp_msg_t **resp)
{
	return slurm_suspend2(job_id, resp);
}

extern int scontrol_load_job(job_info_msg_t **buffer, slurm_step_id_t id)
{
	/* These tests supply IDs and must never resolve names or load a job. */
	CHECK(false);
	return SLURM_ERROR;
}

static void _sequence(const char *input, const char **ids, size_t count)
{
	char *copy = xstrdup(input);

	expected_ids = ids;
	expected_count = count;
	rpc_count = 0;
	scontrol_suspend("suspend", copy);
	CHECK(rpc_count == expected_count);
	CHECK(!exit_code);
	CHECK(!local_job_str);
	rpc_count = 0;
	scontrol_suspend("resume", copy);
	CHECK(rpc_count == expected_count);
	CHECK(!exit_code);
	CHECK(!local_job_str);
	xfree(copy);
}

int main(void)
{
	const char *single[] = { "42" };
	const char *multiple[] = { "11", "12", "13" };
	const char *arrays[] = { "10_1-3", "20_5" };
	const char *ranges[] = { "101_2", "102_2", "103_2", "200", "201" };
	const char *heterogeneous[] = { "42+1", "43+2" };

	alarm(10);
	for (int variant = 0; variant < 2; variant++) {
		null_at_end = variant;
		_sequence("42", single, ARRAY_SIZE(single));
		_sequence("11, 12,,13", multiple, ARRAY_SIZE(multiple));
		_sequence("10_[1-3],20_5", arrays, ARRAY_SIZE(arrays));
		_sequence("[101-103]_2,[200-201]", ranges, ARRAY_SIZE(ranges));
		_sequence("42+1,43+2", heterogeneous,
			  ARRAY_SIZE(heterogeneous));
		_sequence("", NULL, 0);
		_sequence("42", single, ARRAY_SIZE(single));
	}
	puts("Job-ID iteration passed both final-token states, multiple IDs, arrays, ranges, heterogeneous IDs, and repeated commands");
	return EXIT_SUCCESS;
}
