#ifndef MYFS_COMPACT_TEST_H
#define MYFS_COMPACT_TEST_H

#include "myfs.h"

#ifdef MYFS_TEST_FAILPOINTS

enum myfs_generation_gc_test_state
{
    MYFS_GENERATION_GC_TEST_NONE = 0,
    MYFS_GENERATION_GC_TEST_PENDING,
    MYFS_GENERATION_GC_TEST_CLAIMED,
};

enum myfs_generation_gc_test_stage
{
    MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE = 0,
    MYFS_GENERATION_GC_TEST_BEFORE_ALIAS,
    MYFS_GENERATION_GC_TEST_BEFORE_REMOVE,
    MYFS_GENERATION_GC_TEST_BEFORE_FINALIZE,
};

enum myfs_generation_adaptive_test_state
{
    MYFS_GENERATION_ADAPTIVE_TEST_IDLE = 0,
    MYFS_GENERATION_ADAPTIVE_TEST_QUEUED,
    MYFS_GENERATION_ADAPTIVE_TEST_INFLIGHT,
    MYFS_GENERATION_ADAPTIVE_TEST_COOLDOWN_DEFERRED,
};

enum myfs_generation_handoff_test_stage
{
    MYFS_GENERATION_HANDOFF_TEST_BEFORE_PUBLISH = 0,
    MYFS_GENERATION_HANDOFF_TEST_AFTER_VISIBLE_PUBLISH,
    MYFS_GENERATION_HANDOFF_TEST_BEFORE_RECORD_CREATE,
    MYFS_GENERATION_HANDOFF_TEST_AFTER_FIRST_WRITER_RELINK,
    MYFS_GENERATION_HANDOFF_TEST_AFTER_REGISTRY_TRANSACTION,
};

struct myfs_generation_registry_test_snapshot
{
    unsigned open_refs;
    unsigned writer_refs;
    uint64_t metadata_epoch;
    uint64_t gc_claim_id;
    int gc_state;
    bool install_aliases;
    uint64_t adaptive_full_windows;
    uint64_t adaptive_partial_rmw;
    uint64_t classified_since_evaluation;
    uint64_t adaptive_eval_id;
    int adaptive_state;
    bool superseded;
};

typedef int (*myfs_generation_gc_test_hook_fn)(
    enum myfs_generation_gc_test_stage stage,
    const myfs_storage_t *storage, void *context);

typedef int (*myfs_generation_handoff_test_hook_fn)(
    enum myfs_generation_handoff_test_stage stage,
    const myfs_storage_t *old_storage,
    const myfs_storage_t *new_storage, void *context);

typedef void (*myfs_compaction_stop_test_hook_fn)(void *context);

struct myfs_compaction_queue_test_snapshot
{
    size_t queued_requests;
    bool head_is_null;
    bool tail_is_null;
    bool worker_running;
    bool stop_requested;
};

size_t myfs_generation_registry_test_shard_index(const char *path);
size_t myfs_generation_registry_test_bucket_index(const char *path);
size_t myfs_generation_registry_test_record_count(void);
int myfs_generation_registry_test_try_path_shard(const char *path);
int myfs_generation_registry_test_snapshot(
    const myfs_storage_t *storage,
    struct myfs_generation_registry_test_snapshot *snapshot);
void myfs_generation_registry_test_set_gc_hook(
    myfs_generation_gc_test_hook_fn hook, void *context);
void myfs_generation_registry_test_set_handoff_hook(
    myfs_generation_handoff_test_hook_fn hook, void *context);
void myfs_compaction_test_set_stop_post_join_hook(
    myfs_compaction_stop_test_hook_fn hook, void *context);
int myfs_compaction_test_queue_snapshot(
    struct myfs_compaction_queue_test_snapshot *snapshot);

#endif

#endif
