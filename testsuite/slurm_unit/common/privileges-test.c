/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <check.h>
#include <grp.h>
#include <unistd.h>

/* Include system prototypes before substituting syscall names. */
static uid_t mock_getuid(void);
static uid_t mock_geteuid(void);
static gid_t mock_getegid(void);
static int mock_getgroups(int, gid_t *);
static int mock_setgroups(int, const gid_t *);
static int mock_setegid(gid_t);
static int mock_seteuid(uid_t);
static void mock_auth_lock(void);
static void mock_auth_unlock(void);

#define getuid mock_getuid
#define geteuid mock_geteuid
#define getegid mock_getegid
#define getgroups mock_getgroups
#define setgroups mock_setgroups
#define setegid mock_setegid
#define seteuid mock_seteuid
#define auth_setuid_lock mock_auth_lock
#define auth_setuid_unlock mock_auth_unlock
#include "src/slurmd/common/privileges.c"
#undef getuid
#undef geteuid
#undef getegid
#undef getgroups
#undef setgroups
#undef setegid
#undef seteuid
#undef auth_setuid_lock
#undef auth_setuid_unlock

static uid_t real_uid, effective_uid;
static gid_t effective_gid, groups[8];
static int count, locked, fail_setgroups, fail_getgroups, writes;
static bool fail_restore_uid, fail_restore_gid;

static uid_t mock_getuid(void)
{
	return real_uid;
}

static uid_t mock_geteuid(void)
{
	return effective_uid;
}

static gid_t mock_getegid(void)
{
	return effective_gid;
}

static void mock_auth_lock(void)
{
	ck_assert_int_eq(locked++, 0);
}

static void mock_auth_unlock(void)
{
	ck_assert_int_eq(locked--, 1);
}

static int mock_getgroups(int size, gid_t *list)
{
	if (!size)
		return count;
	if (fail_getgroups) {
		errno = EIO;
		return -1;
	}
	ck_assert_int_ge(size, count);
	memcpy(list, groups, count * sizeof(gid_t));
	return count;
}

static int mock_setgroups(int size, const gid_t *list)
{
	writes++;
	ck_assert_uint_eq(effective_uid, 0);
	if (fail_setgroups) {
		fail_setgroups = 0;
		errno = EPERM;
		return -1;
	}
	ck_assert_int_le(size, 8);
	if (size)
		ck_assert_ptr_nonnull(list);
	memcpy(groups, list, size * sizeof(gid_t));
	count = size;
	return 0;
}

static int mock_setegid(gid_t gid)
{
	writes++;
	ck_assert_uint_eq(effective_uid, 0);
	if (fail_restore_gid && gid == 7) {
		errno = EPERM;
		return -1;
	}
	effective_gid = gid;
	return 0;
}

static int mock_seteuid(uid_t uid)
{
	writes++;
	if (fail_restore_uid && !uid) {
		errno = EPERM;
		return -1;
	}
	effective_uid = uid;
	return 0;
}

static void _setup(void)
{
	real_uid = effective_uid = 0;
	effective_gid = 7;
	groups[0] = 10;
	groups[1] = 20;
	count = 2;
	locked = writes = fail_setgroups = fail_getgroups = 0;
	fail_restore_uid = fail_restore_gid = false;
}

static void _restored(void)
{
	ck_assert_uint_eq(effective_uid, 0);
	ck_assert_uint_eq(effective_gid, 7);
	ck_assert_int_eq(count, 2);
	ck_assert_uint_eq(groups[0], 10);
	ck_assert_uint_eq(groups[1], 20);
	ck_assert_int_eq(locked, 0);
}

START_TEST(test_group_only_drop)
{
	gid_t target_groups[] = { 40, 50 };
	stepd_step_rec_t step = {
		.uid = 501, .gid = 40, .ngids = 2, .gids = target_groups
	};
	struct priv_state state;
	ck_assert_int_eq(drop_privileges(&step, false, &state, true), 0);
	ck_assert_uint_eq(effective_uid, 0);
	ck_assert_uint_eq(effective_gid, 40);
	ck_assert_uint_eq(groups[0], 40);
	ck_assert_int_eq(reclaim_privileges(&state), 0);
	_restored();
}

END_TEST

START_TEST(test_restore_without_legacy_snapshot_hint)
{
	gid_t target_groups[] = { 40, 50 };
	stepd_step_rec_t step = {
		.uid = 501, .gid = 40, .ngids = 2, .gids = target_groups
	};
	struct priv_state state = { 0 };
	ck_assert_int_eq(drop_privileges(&step, true, &state, false), 0);
	ck_assert_uint_eq(effective_uid, 501);
	ck_assert_ptr_nonnull(state.gid_list);
	ck_assert_int_eq(reclaim_privileges(&state), 0);
	_restored();
}

END_TEST

START_TEST(test_partial_failure_restores_and_unlocks)
{
	gid_t target_groups[] = { 40, 50 };
	stepd_step_rec_t step = {
		.uid = 501, .gid = 40, .ngids = 2, .gids = target_groups
	};
	struct priv_state state = { 0 };
	int rc, saved_errno;

	fail_setgroups = 1;
	rc = drop_privileges(&step, true, &state, false);
	saved_errno = errno;
	ck_assert_int_eq(rc, -1);
	ck_assert_int_eq(saved_errno, EPERM);
	ck_assert_ptr_null(state.gid_list);
	_restored();
}

END_TEST

START_TEST(test_snapshot_failure_does_not_change_identity)
{
	stepd_step_rec_t step = { 0 };
	struct priv_state state = { 0 };
	int rc, saved_errno;

	fail_getgroups = 1;
	rc = drop_privileges(&step, true, &state, false);
	saved_errno = errno;
	ck_assert_int_eq(rc, -1);
	ck_assert_int_eq(saved_errno, EIO);
	ck_assert_int_eq(writes, 0);
	_restored();
}

END_TEST

START_TEST(test_nonroot_no_privileged_writes)
{
	stepd_step_rec_t step = { 0 };
	struct priv_state state = { 0 };
	real_uid = effective_uid = 501;
	ck_assert_int_eq(drop_privileges(&step, true, &state, false), 0);
	ck_assert_int_eq(reclaim_privileges(&state), 0);
	ck_assert_int_eq(writes, 0);
	ck_assert_uint_eq(effective_uid, 501);
	ck_assert_int_eq(locked, 0);
}

END_TEST

START_TEST(test_uid_restore_failure_unlocks_and_reports_failure)
{
	gid_t target_groups[] = { 40 };
	stepd_step_rec_t step = {
		.uid = 501, .gid = 40, .ngids = 1, .gids = target_groups
	};
	struct priv_state state = { 0 };
	ck_assert_int_eq(drop_privileges(&step, true, &state, false), 0);
	fail_restore_uid = true;
	ck_assert_int_eq(reclaim_privileges(&state), SLURM_ERROR);
	ck_assert_uint_eq(effective_uid, 501);
	ck_assert_uint_eq(effective_gid, 40);
	ck_assert_int_eq(locked, 0);
	ck_assert_ptr_null(state.gid_list);
}

END_TEST

START_TEST(test_gid_restore_failure_still_restores_supplementary_groups)
{
	gid_t target_groups[] = { 40 };
	stepd_step_rec_t step = {
		.uid = 501, .gid = 40, .ngids = 1, .gids = target_groups
	};
	struct priv_state state = { 0 };
	ck_assert_int_eq(drop_privileges(&step, true, &state, false), 0);
	fail_restore_gid = true;
	ck_assert_int_eq(reclaim_privileges(&state), SLURM_ERROR);
	ck_assert_uint_eq(effective_uid, 0);
	ck_assert_uint_eq(effective_gid, 40);
	ck_assert_int_eq(count, 2);
	ck_assert_uint_eq(groups[0], 10);
	ck_assert_uint_eq(groups[1], 20);
	ck_assert_int_eq(locked, 0);
	ck_assert_ptr_null(state.gid_list);
}

END_TEST

int main(void)
{
	Suite *suite = suite_create("Temporary credential changes");
	TCase *test = tcase_create("restoration");
	SRunner *runner;
	int failed;
	tcase_add_checked_fixture(test, _setup, NULL);
	tcase_add_test(test, test_group_only_drop);
	tcase_add_test(test, test_restore_without_legacy_snapshot_hint);
	tcase_add_test(test, test_partial_failure_restores_and_unlocks);
	tcase_add_test(test, test_snapshot_failure_does_not_change_identity);
	tcase_add_test(test, test_nonroot_no_privileged_writes);
	tcase_add_test(test,
		       test_uid_restore_failure_unlocks_and_reports_failure);
	tcase_add_test(
		test,
		test_gid_restore_failure_still_restores_supplementary_groups);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_run_all(runner, CK_ENV);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
