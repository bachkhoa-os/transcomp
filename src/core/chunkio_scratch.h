#ifndef MYFS_CHUNKIO_SCRATCH_H
#define MYFS_CHUNKIO_SCRATCH_H

#include <stdbool.h>
#include <stddef.h>

/* Roles stay separate because chunk-I/O paths can hold several of them at
 * once.  A same-role nested acquisition is still safe: it gets a temporary
 * allocation instead of aliasing the retained buffer. */
typedef enum
{
    MYFS_CHUNKIO_SCRATCH_RAW = 0,
    MYFS_CHUNKIO_SCRATCH_COMP,
    MYFS_CHUNKIO_SCRATCH_WINDOW,
    MYFS_CHUNKIO_SCRATCH_CACHED,
    MYFS_CHUNKIO_SCRATCH_ROLE_COUNT,
} myfs_chunkio_scratch_role_t;

typedef struct
{
    char *data;
    size_t capacity;
    void *retained_slot;
} myfs_chunkio_scratch_lease_t;

int myfs_chunkio_scratch_acquire(myfs_chunkio_scratch_role_t role,
                                 size_t required_size,
                                 myfs_chunkio_scratch_lease_t *lease);
void myfs_chunkio_scratch_release(myfs_chunkio_scratch_lease_t *lease);
size_t myfs_chunkio_scratch_retained_limit(
    myfs_chunkio_scratch_role_t role);

#ifdef MYFS_TEST_FAILPOINTS
typedef struct
{
    const void *retained_address;
    size_t retained_capacity;
    bool in_use;
    size_t growth_count;
    size_t temporary_acquisition_count;
} myfs_chunkio_scratch_snapshot_t;

int myfs_chunkio_scratch_test_snapshot(
    myfs_chunkio_scratch_role_t role,
    myfs_chunkio_scratch_snapshot_t *snapshot);
size_t myfs_chunkio_scratch_test_destroy_count(void);
void myfs_chunkio_scratch_test_force_tls_unavailable(bool unavailable);
void myfs_chunkio_scratch_test_fail_next_growth(void);
#endif

#endif
