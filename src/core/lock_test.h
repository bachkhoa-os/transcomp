#ifndef MYFS_LOCK_TEST_H
#define MYFS_LOCK_TEST_H

#include <stdbool.h>

#ifdef MYFS_TEST_FAILPOINTS

typedef bool (*myfs_lock_test_acquire_fail_hook_fn)(const char *path,
                                                     void *context);

/* Invoked before any lock-table shard mutex is acquired.  The callback runs
 * while lock_test_hook_mu is held, so a blocking callback also blocks every
 * other myfs_lock_file() caller in failpoint builds.  A true result forces
 * NULL for that acquisition and atomically disarms the hook. */
void myfs_lock_test_set_acquire_fail_hook(
    myfs_lock_test_acquire_fail_hook_fn hook, void *context);

#endif

#endif
