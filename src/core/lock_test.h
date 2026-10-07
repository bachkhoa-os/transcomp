#ifndef MYFS_LOCK_TEST_H
#define MYFS_LOCK_TEST_H

#include <stdbool.h>

#ifdef MYFS_TEST_FAILPOINTS

typedef bool (*myfs_lock_test_acquire_fail_hook_fn)(const char *path,
                                                     void *context);

/* Invoked before any lock-table shard mutex is acquired.  A true result
 * forces NULL for that acquisition and atomically disarms the hook. */
void myfs_lock_test_set_acquire_fail_hook(
    myfs_lock_test_acquire_fail_hook_fn hook, void *context);

#endif

#endif
