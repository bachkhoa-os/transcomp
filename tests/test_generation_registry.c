#include "myfs.h"
#include "core/compact_test.h"
#include "core/lock_test.h"

#include <assert.h>
#include <stdatomic.h>

static const char GENERATION_A[] = "11111111111111111111111111111111";
static const char GENERATION_B[] = "22222222222222222222222222222222";
static const char GENERATION_C[] = "33333333333333333333333333333333";

static struct timespec deadline_after_ms(long milliseconds)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static myfs_storage_t fake_storage(const char *path, const char *generation)
{
    myfs_storage_t storage = {0};
    assert(snprintf(storage.logical_path, sizeof(storage.logical_path),
                    "%s", path) < (int)sizeof(storage.logical_path));
    assert(snprintf(storage.generation_id, sizeof(storage.generation_id),
                    "%s", generation) < (int)sizeof(storage.generation_id));
    assert(snprintf(storage.data_path, sizeof(storage.data_path),
                    "/unused%s.%s", path, generation) <
           (int)sizeof(storage.data_path));
    return storage;
}

static void initialize_handle(myfs_file_handle_t *handle,
                              const myfs_storage_t *storage, int flags)
{
    memset(handle, 0, sizeof(*handle));
    handle->data_fd = -1;
    handle->meta_fd = -1;
    handle->flags = flags;
    handle->storage = *storage;
    assert(pthread_rwlock_init(&handle->cache_lock, NULL) == 0);
}

static void register_handle(myfs_file_handle_t *handle)
{
    myfs_file_lock_t *lock = myfs_lock_file(handle->storage.logical_path);
    assert(lock != NULL);
    assert(register_generation_handle_locked(handle) == 0);
    myfs_unlock_file(lock);
}

static void unregister_handle(myfs_file_handle_t *handle)
{
    myfs_file_lock_t *lock = myfs_lock_file(handle->storage.logical_path);
    assert(lock != NULL);
    unregister_generation_handle_locked(handle);
    myfs_unlock_file(lock);
    assert(pthread_rwlock_destroy(&handle->cache_lock) == 0);
}

static void mark_generation(const myfs_storage_t *storage,
                            bool install_aliases)
{
    myfs_file_lock_t *lock = myfs_lock_file(storage->logical_path);
    assert(lock != NULL);
    assert(mark_generation_for_gc_locked(storage, install_aliases) == 0);
    myfs_unlock_file(lock);
}

static struct myfs_generation_registry_test_snapshot snapshot_for(
    const myfs_storage_t *storage)
{
    struct myfs_generation_registry_test_snapshot snapshot;
    assert(myfs_generation_registry_test_snapshot(storage, &snapshot) == 0);
    return snapshot;
}

static void test_logical_path_sharding_preserves_generation_identity(void)
{
    destroy_generation_registry();
    myfs_storage_t first = fake_storage("/identity", GENERATION_A);
    myfs_storage_t first_copy = first;
    myfs_storage_t second = fake_storage("/identity", GENERATION_B);
    assert(myfs_generation_registry_test_shard_index(first.logical_path) ==
           myfs_generation_registry_test_shard_index(second.logical_path));
    assert(myfs_generation_registry_test_bucket_index(first.logical_path) ==
           myfs_generation_registry_test_bucket_index(second.logical_path));

    myfs_file_handle_t first_reader;
    myfs_file_handle_t first_writer;
    myfs_file_handle_t second_writer;
    initialize_handle(&first_reader, &first, O_RDONLY);
    initialize_handle(&first_writer, &first_copy, O_RDWR);
    initialize_handle(&second_writer, &second, O_RDWR);
    register_handle(&first_reader);
    register_handle(&first_writer);
    register_handle(&second_writer);

    struct myfs_generation_registry_test_snapshot first_snapshot =
        snapshot_for(&first);
    struct myfs_generation_registry_test_snapshot second_snapshot =
        snapshot_for(&second);
    assert(first_snapshot.open_refs == 2);
    assert(first_snapshot.writer_refs == 1);
    assert(second_snapshot.open_refs == 1);
    assert(second_snapshot.writer_refs == 1);
    assert(myfs_generation_registry_test_record_count() == 2);

    unregister_handle(&first_writer);
    unregister_handle(&first_reader);
    assert(myfs_generation_registry_test_snapshot(&first, &first_snapshot) ==
           -ENOENT);
    assert(myfs_generation_registry_test_record_count() == 1);
    unregister_handle(&second_writer);
    assert(myfs_generation_registry_test_record_count() == 0);
}

static void test_legacy_identity_rules_are_unchanged(void)
{
    myfs_storage_t known_a = fake_storage("/legacy-known", "legacy");
    known_a.is_legacy = true;
    known_a.data_dev = 17;
    known_a.data_ino = 23;
    myfs_storage_t known_b = known_a;
    strcpy(known_b.data_path, "/a/different/spelling");

    myfs_file_handle_t first;
    myfs_file_handle_t second;
    initialize_handle(&first, &known_a, O_RDONLY);
    initialize_handle(&second, &known_b, O_RDONLY);
    register_handle(&first);
    register_handle(&second);
    assert(snapshot_for(&known_a).open_refs == 2);
    assert(myfs_generation_registry_test_record_count() == 1);
    unregister_handle(&first);
    unregister_handle(&second);

    myfs_storage_t unknown_a = fake_storage("/legacy-unknown", "legacy");
    unknown_a.is_legacy = true;
    myfs_storage_t unknown_b = unknown_a;
    initialize_handle(&first, &unknown_a, O_RDONLY);
    initialize_handle(&second, &unknown_b, O_RDONLY);
    register_handle(&first);
    register_handle(&second);
    assert(snapshot_for(&unknown_a).open_refs == 2);
    assert(myfs_generation_registry_test_record_count() == 1);
    unregister_handle(&first);
    unregister_handle(&second);
}

static void find_same_bucket_paths(char paths[3][64])
{
    char first[64 * 64][64] = {{0}};
    char second[64 * 64][64] = {{0}};
    for (unsigned i = 0; i < 200000; i++)
    {
        char candidate[64];
        assert(snprintf(candidate, sizeof(candidate), "/registry-bucket-%u", i)
               > 0);
        size_t slot =
            myfs_generation_registry_test_shard_index(candidate) * 64 +
            myfs_generation_registry_test_bucket_index(candidate);
        assert(slot < 64 * 64);
        if (second[slot][0] != '\0')
        {
            strcpy(paths[0], first[slot]);
            strcpy(paths[1], second[slot]);
            strcpy(paths[2], candidate);
            return;
        }
        if (first[slot][0] != '\0')
            strcpy(second[slot], candidate);
        else
            strcpy(first[slot], candidate);
    }
    assert(!"failed to find three deterministic bucket collisions");
}

static void test_collision_chain_unlinks_head_middle_and_tail(void)
{
    char paths[3][64];
    find_same_bucket_paths(paths);
    myfs_storage_t storage[3];
    myfs_file_handle_t handles[3];
    for (size_t i = 0; i < 3; i++)
    {
        storage[i] = fake_storage(paths[i], GENERATION_A);
        initialize_handle(&handles[i], &storage[i], O_RDONLY);
        register_handle(&handles[i]);
    }
    assert(myfs_generation_registry_test_record_count() == 3);

    unregister_handle(&handles[1]);
    assert(myfs_generation_registry_test_snapshot(&storage[1], NULL) ==
           -ENOENT);
    assert(snapshot_for(&storage[0]).open_refs == 1);
    assert(snapshot_for(&storage[2]).open_refs == 1);
    unregister_handle(&handles[2]);
    assert(myfs_generation_registry_test_snapshot(&storage[2], NULL) ==
           -ENOENT);
    assert(snapshot_for(&storage[0]).open_refs == 1);
    unregister_handle(&handles[0]);
    assert(myfs_generation_registry_test_record_count() == 0);
}

static void test_sharded_destroy_is_idempotent_and_reusable(void)
{
    myfs_storage_t first = fake_storage("/destroy-one", GENERATION_A);
    myfs_storage_t second = fake_storage("/destroy-two", GENERATION_B);
    mark_generation(&first, false);
    mark_generation(&second, true);
    assert(myfs_generation_registry_test_record_count() == 2);
    destroy_generation_registry();
    destroy_generation_registry();
    assert(myfs_generation_registry_test_record_count() == 0);
    mark_generation(&first, false);
    assert(myfs_generation_registry_test_record_count() == 1);
    destroy_generation_registry();
}

struct gc_fixture
{
    char root[PATH_MAX];
    struct myfs_config config;
    char path[128];
    myfs_storage_t victim;
    myfs_storage_t active;
    bool victim_is_legacy;
};

static void create_empty_file(const char *path)
{
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
}

static void initialize_gc_fixture(struct gc_fixture *fixture,
                                  const char *logical_path,
                                  bool legacy_victim)
{
    memset(fixture, 0, sizeof(*fixture));
    strcpy(fixture->root, "/tmp/myfs-generation-registry-XXXXXX");
    assert(mkdtemp(fixture->root) != NULL);
    assert(snprintf(fixture->config.root, sizeof(fixture->config.root), "%s",
                    fixture->root) < (int)sizeof(fixture->config.root));
    assert(snprintf(fixture->path, sizeof(fixture->path), "%s", logical_path) <
           (int)sizeof(fixture->path));
    myfs_conf = &fixture->config;
    fixture->victim_is_legacy = legacy_victim;

    if (legacy_victim)
    {
        fixture->victim = fake_storage(logical_path, "legacy");
        fixture->victim.is_legacy = true;
        build_data_path(fixture->victim.data_path, logical_path);
        build_meta_path(fixture->victim.meta_path, logical_path);
        create_empty_file(fixture->victim.data_path);
        create_empty_file(fixture->victim.meta_path);
        struct stat st;
        assert(stat(fixture->victim.data_path, &st) == 0);
        fixture->victim.data_dev = st.st_dev;
        fixture->victim.data_ino = st.st_ino;
    }
    else
    {
        assert(create_generation_storage(logical_path, 0600,
                                         &fixture->victim) == 0);
        create_empty_file(fixture->victim.meta_path);
    }

    assert(create_generation_storage(logical_path, 0600,
                                     &fixture->active) == 0);
    create_empty_file(fixture->active.meta_path);
    assert(publish_generation(logical_path, &fixture->active) == 0);
}

static void cleanup_gc_fixture(struct gc_fixture *fixture)
{
    char current_path[PATH_MAX];
    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_current_path(current_path, fixture->path);
    build_data_path(data_alias, fixture->path);
    build_meta_path(meta_alias, fixture->path);
    assert(unlink(current_path) == 0 || errno == ENOENT);
    assert(unlink(data_alias) == 0 || errno == ENOENT);
    assert(unlink(meta_alias) == 0 || errno == ENOENT);
    assert(remove_generation_storage(&fixture->victim) == 0 ||
           fixture->victim_is_legacy);
    assert(remove_generation_storage(&fixture->active) == 0);
    destroy_generation_registry();
    assert(rmdir(fixture->root) == 0);
    myfs_conf = NULL;
}

struct gc_gate
{
    pthread_mutex_t mu;
    pthread_cond_t cv;
    enum myfs_generation_gc_test_stage stage;
    char path[PATH_MAX];
    bool entered;
    bool release;
    int result;
};

static int gc_gate_hook(enum myfs_generation_gc_test_stage stage,
                        const myfs_storage_t *storage, void *argument)
{
    struct gc_gate *gate = argument;
    if (stage != gate->stage ||
        strcmp(storage->logical_path, gate->path) != 0)
        return 0;
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->entered = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    while (!gate->release)
        assert(pthread_cond_wait(&gate->cv, &gate->mu) == 0);
    int result = gate->result;
    assert(pthread_mutex_unlock(&gate->mu) == 0);
    return result;
}

static void initialize_gate(struct gc_gate *gate,
                            enum myfs_generation_gc_test_stage stage,
                            const char *path)
{
    memset(gate, 0, sizeof(*gate));
    gate->stage = stage;
    assert(snprintf(gate->path, sizeof(gate->path), "%s", path) <
           (int)sizeof(gate->path));
    assert(pthread_mutex_init(&gate->mu, NULL) == 0);
    assert(pthread_cond_init(&gate->cv, NULL) == 0);
}

static bool wait_for_gate(struct gc_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    int status = 0;
    while (!gate->entered && status == 0)
        status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
    bool entered = gate->entered;
    assert(pthread_mutex_unlock(&gate->mu) == 0);
    return entered;
}

static void release_gate(struct gc_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->release = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void destroy_gate(struct gc_gate *gate)
{
    assert(pthread_cond_destroy(&gate->cv) == 0);
    assert(pthread_mutex_destroy(&gate->mu) == 0);
}

struct gc_thread_context
{
    const char *path;
    int result;
};

static void *gc_thread_main(void *argument)
{
    struct gc_thread_context *context = argument;
    myfs_file_lock_t *lock = myfs_lock_file(context->path);
    assert(lock != NULL);
    context->result = run_generation_gc_locked(context->path);
    myfs_unlock_file(lock);
    return NULL;
}

static void find_unrelated_path(const char *path, bool same_bucket,
                                char result[128])
{
    size_t wanted_shard = myfs_generation_registry_test_shard_index(path);
    size_t wanted_bucket = myfs_generation_registry_test_bucket_index(path);
    for (unsigned i = 0; i < 200000; i++)
    {
        char candidate[128];
        assert(snprintf(candidate, sizeof(candidate), "/gc-progress-%u", i) > 0);
        size_t shard = myfs_generation_registry_test_shard_index(candidate);
        size_t bucket = myfs_generation_registry_test_bucket_index(candidate);
        bool matches = same_bucket
            ? shard == wanted_shard && bucket == wanted_bucket
            : shard != wanted_shard;
        if (matches && strcmp(candidate, path) != 0)
        {
            strcpy(result, candidate);
            return;
        }
    }
    assert(!"failed to find unrelated registry path");
}

static void find_paths_in_increasing_shard_order(char first[128],
                                                 char second[128])
{
    char paths[64][128] = {{0}};
    for (unsigned i = 0; i < 200000; i++)
    {
        char candidate[128];
        assert(snprintf(candidate, sizeof(candidate), "/gc-sweep-%u", i) > 0);
        size_t shard = myfs_generation_registry_test_shard_index(candidate);
        assert(shard < 64);
        if (paths[shard][0] == '\0')
            strcpy(paths[shard], candidate);
    }
    for (size_t low = 0; low < 64; low++)
    {
        if (paths[low][0] == '\0')
            continue;
        for (size_t high = low + 1; high < 64; high++)
        {
            if (paths[high][0] == '\0')
                continue;
            strcpy(first, paths[low]);
            strcpy(second, paths[high]);
            return;
        }
    }
    assert(!"failed to find paths in increasing shard order");
}

struct progress_context
{
    char path[128];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
};

static void *registry_progress_main(void *argument)
{
    struct progress_context *context = argument;
    myfs_storage_t storage = fake_storage(context->path, GENERATION_C);
    myfs_file_handle_t handle;
    initialize_handle(&handle, &storage, O_RDWR);
    myfs_file_lock_t *lock = myfs_lock_file(context->path);
    assert(lock != NULL);
    assert(register_generation_handle_locked(&handle) == 0);
    assert(snapshot_for(&storage).open_refs == 1);
    assert(generation_bump_metadata_epoch_locked(&handle) == 2);
    unregister_generation_handle_locked(&handle);
    myfs_unlock_file(lock);
    assert(pthread_rwlock_destroy(&handle.cache_lock) == 0);

    assert(pthread_mutex_lock(&context->mu) == 0);
    context->done = true;
    assert(pthread_cond_broadcast(&context->cv) == 0);
    assert(pthread_mutex_unlock(&context->mu) == 0);
    return NULL;
}

static bool wait_for_progress(struct progress_context *context)
{
    assert(pthread_mutex_lock(&context->mu) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    int status = 0;
    while (!context->done && status == 0)
        status = pthread_cond_timedwait(&context->cv, &context->mu, &deadline);
    bool done = context->done;
    assert(pthread_mutex_unlock(&context->mu) == 0);
    return done;
}

static void verify_gc_pause_allows_progress(
    enum myfs_generation_gc_test_stage stage, bool same_bucket)
{
    bool legacy_victim = stage == MYFS_GENERATION_GC_TEST_BEFORE_ALIAS;
    const char *path = same_bucket ? "/gc-paused-same" : "/gc-paused-other";
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, path, legacy_victim);
    mark_generation(&fixture.victim, legacy_victim);

    struct gc_gate gate;
    initialize_gate(&gate, stage, fixture.path);
    myfs_generation_registry_test_set_gc_hook(gc_gate_hook, &gate);
    struct gc_thread_context gc_context = {.path = fixture.path};
    pthread_t gc_thread;
    assert(pthread_create(&gc_thread, NULL, gc_thread_main, &gc_context) == 0);
    assert(wait_for_gate(&gate));

    struct myfs_generation_registry_test_snapshot claimed =
        snapshot_for(&fixture.victim);
    assert(claimed.gc_state == MYFS_GENERATION_GC_TEST_CLAIMED);
    assert(claimed.gc_claim_id != 0);

    struct progress_context progress = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    find_unrelated_path(fixture.path, same_bucket, progress.path);
    pthread_t progress_thread;
    assert(pthread_create(&progress_thread, NULL, registry_progress_main,
                          &progress) == 0);
    bool progressed = wait_for_progress(&progress);
    release_gate(&gate);
    assert(pthread_join(progress_thread, NULL) == 0);
    assert(pthread_join(gc_thread, NULL) == 0);
    myfs_generation_registry_test_set_gc_hook(NULL, NULL);
    assert(progressed);
    assert(gc_context.result == 0);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    assert(pthread_cond_destroy(&progress.cv) == 0);
    assert(pthread_mutex_destroy(&progress.mu) == 0);
    destroy_gate(&gate);
    cleanup_gc_fixture(&fixture);
}

static void test_gc_io_pause_points_release_registry_shards(void)
{
    const enum myfs_generation_gc_test_stage stages[] = {
        MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE,
        MYFS_GENERATION_GC_TEST_BEFORE_ALIAS,
        MYFS_GENERATION_GC_TEST_BEFORE_REMOVE,
        MYFS_GENERATION_GC_TEST_BEFORE_FINALIZE,
    };
    for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
    {
        verify_gc_pause_allows_progress(stages[i], false);
        verify_gc_pause_allows_progress(stages[i], true);
    }
}

static void test_gc_error_restores_pending_and_retry_is_idempotent(
    enum myfs_generation_gc_test_stage stage)
{
    bool legacy_victim = stage == MYFS_GENERATION_GC_TEST_BEFORE_ALIAS;
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-retry", legacy_victim);
    mark_generation(&fixture.victim, legacy_victim);

    struct gc_gate gate;
    initialize_gate(&gate, stage, fixture.path);
    gate.release = true;
    gate.result = -EIO;
    myfs_generation_registry_test_set_gc_hook(gc_gate_hook, &gate);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == -EIO);
    myfs_unlock_file(lock);
    struct myfs_generation_registry_test_snapshot pending =
        snapshot_for(&fixture.victim);
    assert(pending.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(pending.gc_claim_id != 0);

    myfs_generation_registry_test_set_gc_hook(NULL, NULL);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    destroy_gate(&gate);
    cleanup_gc_fixture(&fixture);
}

static void test_gc_failures_restore_pending_for_retry(void)
{
    test_gc_error_restores_pending_and_retry_is_idempotent(
        MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE);
    test_gc_error_restores_pending_and_retry_is_idempotent(
        MYFS_GENERATION_GC_TEST_BEFORE_ALIAS);
    test_gc_error_restores_pending_and_retry_is_idempotent(
        MYFS_GENERATION_GC_TEST_BEFORE_REMOVE);
}

static void test_open_references_defer_retirement_safely(void)
{
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-open-ref", false);
    myfs_file_handle_t reader;
    initialize_handle(&reader, &fixture.victim, O_RDONLY);
    register_handle(&reader);
    mark_generation(&fixture.victim, false);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(snapshot_for(&fixture.victim).gc_state ==
           MYFS_GENERATION_GC_TEST_PENDING);
    assert(access(fixture.victim.generation_dir, F_OK) == 0);
    unregister_handle(&reader);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    cleanup_gc_fixture(&fixture);
}

static void test_legacy_references_defer_alias_replacement(void)
{
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-legacy-ref", true);
    struct stat original;
    assert(stat(fixture.victim.data_path, &original) == 0);
    myfs_file_handle_t reader;
    initialize_handle(&reader, &fixture.victim, O_RDONLY);
    register_handle(&reader);
    mark_generation(&fixture.victim, true);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    struct stat deferred;
    assert(stat(fixture.victim.data_path, &deferred) == 0);
    assert(deferred.st_dev == original.st_dev && deferred.st_ino == original.st_ino);
    assert(snapshot_for(&fixture.victim).gc_state ==
           MYFS_GENERATION_GC_TEST_PENDING);

    unregister_handle(&reader);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    struct stat alias;
    struct stat active;
    assert(stat(fixture.victim.data_path, &alias) == 0);
    assert(stat(fixture.active.data_path, &active) == 0);
    assert(alias.st_dev == active.st_dev && alias.st_ino == active.st_ino);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    cleanup_gc_fixture(&fixture);
}

static void test_alias_install_can_finish_before_old_reader_retires(void)
{
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-alias-only", false);
    myfs_file_handle_t reader;
    initialize_handle(&reader, &fixture.victim, O_RDONLY);
    register_handle(&reader);
    mark_generation(&fixture.victim, true);

    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    struct myfs_generation_registry_test_snapshot pending =
        snapshot_for(&fixture.victim);
    assert(pending.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(pending.install_aliases);
    assert(access(fixture.victim.generation_dir, F_OK) == 0);

    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_data_path(data_alias, fixture.path);
    build_meta_path(meta_alias, fixture.path);
    struct stat alias_data;
    struct stat active_data;
    struct stat alias_meta;
    struct stat active_meta;
    assert(stat(data_alias, &alias_data) == 0);
    assert(stat(fixture.active.data_path, &active_data) == 0);
    assert(stat(meta_alias, &alias_meta) == 0);
    assert(stat(fixture.active.meta_path, &active_meta) == 0);
    assert(alias_data.st_dev == active_data.st_dev &&
           alias_data.st_ino == active_data.st_ino);
    assert(alias_meta.st_dev == active_meta.st_dev &&
           alias_meta.st_ino == active_meta.st_ino);

    unregister_handle(&reader);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    cleanup_gc_fixture(&fixture);
}

static void test_active_generation_is_never_removed(void)
{
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-active", false);
    mark_generation(&fixture.active, false);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == -EBUSY);
    myfs_unlock_file(lock);
    assert(snapshot_for(&fixture.active).gc_state ==
           MYFS_GENERATION_GC_TEST_PENDING);
    assert(access(fixture.active.generation_dir, F_OK) == 0);
    cleanup_gc_fixture(&fixture);
}

static void test_partial_removal_retries_after_files_disappear(void)
{
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-partial-remove", false);
    char blocker[PATH_MAX];
    assert(snprintf(blocker, sizeof(blocker), "%s/blocker",
                    fixture.victim.generation_dir) < (int)sizeof(blocker));
    create_empty_file(blocker);
    mark_generation(&fixture.victim, false);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == -ENOTEMPTY);
    myfs_unlock_file(lock);
    assert(snapshot_for(&fixture.victim).gc_state ==
           MYFS_GENERATION_GC_TEST_PENDING);
    assert(access(fixture.victim.data_path, F_OK) != 0 && errno == ENOENT);
    assert(access(fixture.victim.meta_path, F_OK) != 0 && errno == ENOENT);
    assert(unlink(blocker) == 0);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    cleanup_gc_fixture(&fixture);
}

static void test_quiescent_sweep_continues_after_first_error(void)
{
    char busy_path[128];
    char removable_path[128];
    find_paths_in_increasing_shard_order(busy_path, removable_path);
    assert(myfs_generation_registry_test_shard_index(busy_path) <
           myfs_generation_registry_test_shard_index(removable_path));

    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, busy_path, false);

    myfs_storage_t removable;
    myfs_storage_t second_active;
    assert(create_generation_storage(removable_path, 0600, &removable) == 0);
    create_empty_file(removable.meta_path);
    assert(create_generation_storage(removable_path, 0600,
                                     &second_active) == 0);
    create_empty_file(second_active.meta_path);
    assert(publish_generation(removable_path, &second_active) == 0);

    mark_generation(&fixture.active, false);
    mark_generation(&removable, false);
    assert(run_generation_gc_locked(NULL) == -EBUSY);
    assert(snapshot_for(&fixture.active).gc_state ==
           MYFS_GENERATION_GC_TEST_PENDING);
    assert(myfs_generation_registry_test_snapshot(&removable, NULL) ==
           -ENOENT);
    assert(access(removable.generation_dir, F_OK) != 0 && errno == ENOENT);

    char current_path[PATH_MAX];
    build_current_path(current_path, removable_path);
    assert(unlink(current_path) == 0);
    assert(remove_generation_storage(&removable) == 0);
    assert(remove_generation_storage(&second_active) == 0);
    cleanup_gc_fixture(&fixture);
}

static void test_repeated_marks_merge_alias_requirement(void)
{
    myfs_storage_t storage = fake_storage("/merge-mark", GENERATION_A);
    mark_generation(&storage, false);
    mark_generation(&storage, true);
    struct myfs_generation_registry_test_snapshot pending =
        snapshot_for(&storage);
    assert(pending.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(pending.install_aliases);
    destroy_generation_registry();
}

static myfs_adaptive_ticket_t queue_adaptive_ticket(
    myfs_file_handle_t *writer)
{
    myfs_adaptive_ticket_t ticket = {0};
    myfs_file_lock_t *lock = myfs_lock_file(writer->storage.logical_path);
    assert(lock != NULL);
    assert(generation_observe_write_locked(writer, 0, 128, &ticket));
    myfs_unlock_file(lock);
    assert(ticket.valid);
    return ticket;
}

static void assert_ticket_restored_once(
    const myfs_storage_t *storage, const myfs_adaptive_ticket_t *ticket,
    int expected_gc_state)
{
    struct myfs_generation_registry_test_snapshot restored =
        snapshot_for(storage);
    assert(restored.gc_state == expected_gc_state);
    assert(restored.adaptive_eval_id == ticket->evaluation_id);
    assert(restored.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(restored.adaptive_full_windows ==
           ticket->stats_snapshot.full_windows);
    assert(restored.adaptive_partial_rmw ==
           ticket->stats_snapshot.partial_rmw);
    assert(restored.classified_since_evaluation == 128);

    generation_adaptive_schedule_failed(ticket);
    struct myfs_generation_registry_test_snapshot duplicate =
        snapshot_for(storage);
    assert(duplicate.adaptive_full_windows ==
           restored.adaptive_full_windows);
    assert(duplicate.adaptive_partial_rmw ==
           restored.adaptive_partial_rmw);
    assert(duplicate.classified_since_evaluation ==
           restored.classified_since_evaluation);
    assert(duplicate.adaptive_state == restored.adaptive_state);
}

struct worker_lock_failure_gate
{
    char path[PATH_MAX];
    pthread_t request_thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered;
    bool release;
    unsigned matching_calls;
};

static bool fail_worker_path_lock_once(const char *path, void *argument)
{
    struct worker_lock_failure_gate *gate = argument;
    if (strcmp(path, gate->path) != 0 ||
        pthread_equal(pthread_self(), gate->request_thread))
        return false;

    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->matching_calls++;
    assert(gate->matching_calls == 1);
    gate->entered = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    while (!gate->release)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
    return true;
}

static void initialize_worker_lock_failure_gate(
    struct worker_lock_failure_gate *gate, const char *path)
{
    memset(gate, 0, sizeof(*gate));
    assert(snprintf(gate->path, sizeof(gate->path), "%s", path) <
           (int)sizeof(gate->path));
    gate->request_thread = pthread_self();
    assert(pthread_mutex_init(&gate->mu, NULL) == 0);
    assert(pthread_cond_init(&gate->cv, NULL) == 0);
}

static void wait_for_worker_lock_failure_gate(
    struct worker_lock_failure_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    while (!gate->entered)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void release_worker_lock_failure_gate(
    struct worker_lock_failure_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->release = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void destroy_worker_lock_failure_gate(
    struct worker_lock_failure_gate *gate)
{
    assert(pthread_cond_destroy(&gate->cv) == 0);
    assert(pthread_mutex_destroy(&gate->mu) == 0);
}

static void *stop_compaction_worker_main(void *argument)
{
    (void)argument;
    stop_compaction_worker();
    return NULL;
}

static void stop_compaction_worker_with_timeout(void)
{
    pthread_t stop_thread;
    assert(pthread_create(&stop_thread, NULL, stop_compaction_worker_main,
                          NULL) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    assert(pthread_timedjoin_np(stop_thread, NULL, &deadline) == 0);
}

static void make_storage_compaction_eligible(const myfs_storage_t *storage)
{
    const unsigned char payload = 'W';
    int fd = open(storage->data_path,
                  O_WRONLY | O_TRUNC | O_CLOEXEC);
    assert(fd >= 0);
    assert(write(fd, &payload, sizeof(payload)) == (ssize_t)sizeof(payload));
    assert(ftruncate(fd, 2 * (off_t)MYFS_DEFAULT_WINDOW_SIZE) == 0);
    assert(close(fd) == 0);

    myfs_chunk_t chunk = {
        .logical_offset = 0,
        .raw_size = sizeof(payload),
        .stored_size = sizeof(payload),
        .codec_type = 0,
        .checksum = chunk_crc32(&payload, sizeof(payload)),
        .physical_offset = 0,
    };
    myfs_inode_t inode = {
        .chunk_map = {
            .num_chunks = 1,
            .logical_size = sizeof(payload),
            .chunks = &chunk,
            .fully_packed = true,
        },
        .window_size = MYFS_DEFAULT_WINDOW_SIZE,
    };
    assert(save_chunk_map_to_path(storage->meta_path, &inode) == 0);
}

struct stop_post_join_gate
{
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered;
    bool release;
    unsigned calls;
};

static void pause_stop_after_join(void *argument)
{
    struct stop_post_join_gate *gate = argument;
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->calls++;
    assert(gate->calls == 1);
    gate->entered = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    while (!gate->release)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void wait_for_stop_post_join_gate(struct stop_post_join_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    while (!gate->entered)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void release_stop_post_join_gate(struct stop_post_join_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->release = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

struct submitter_lock_failure
{
    char path[PATH_MAX];
    pthread_t submitter;
    unsigned matching_calls;
};

static bool fail_submitter_path_lock_once(const char *path, void *argument)
{
    struct submitter_lock_failure *failure = argument;
    if (strcmp(path, failure->path) != 0 ||
        !pthread_equal(pthread_self(), failure->submitter))
        return false;
    failure->matching_calls++;
    assert(failure->matching_calls == 1);
    return true;
}

static void assert_registry_snapshots_equal(
    const struct myfs_generation_registry_test_snapshot *actual,
    const struct myfs_generation_registry_test_snapshot *expected)
{
    assert(actual->open_refs == expected->open_refs);
    assert(actual->writer_refs == expected->writer_refs);
    assert(actual->metadata_epoch == expected->metadata_epoch);
    assert(actual->gc_claim_id == expected->gc_claim_id);
    assert(actual->gc_state == expected->gc_state);
    assert(actual->install_aliases == expected->install_aliases);
    assert(actual->adaptive_full_windows == expected->adaptive_full_windows);
    assert(actual->adaptive_partial_rmw == expected->adaptive_partial_rmw);
    assert(actual->classified_since_evaluation ==
           expected->classified_since_evaluation);
    assert(actual->adaptive_eval_id == expected->adaptive_eval_id);
    assert(actual->adaptive_state == expected->adaptive_state);
    assert(actual->superseded == expected->superseded);
}

static void test_stopping_worker_uses_synchronous_fallback(void)
{
    stop_compaction_worker_with_timeout();
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/worker-stop-fallback", false);
    make_storage_compaction_eligible(&fixture.active);

    myfs_file_handle_t writer;
    initialize_handle(&writer, &fixture.active, O_RDWR);
    register_handle(&writer);

    myfs_adaptive_ticket_t seed = queue_adaptive_ticket(&writer);
    myfs_adaptive_ticket_t unexpected = {0};
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(!generation_observe_write_locked(&writer, 0, 17, &unexpected));
    myfs_unlock_file(lock);
    assert(!unexpected.valid);
    generation_adaptive_schedule_failed(&seed);
    struct myfs_generation_registry_test_snapshot before_ticket =
        snapshot_for(&fixture.active);
    assert(before_ticket.adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(before_ticket.adaptive_full_windows == 0);
    assert(before_ticket.adaptive_partial_rmw == 145);
    assert(before_ticket.classified_since_evaluation == 145);

    struct stop_post_join_gate gate = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    assert(start_compaction_worker() == 0);
    myfs_compaction_test_set_stop_post_join_hook(pause_stop_after_join, &gate);
    pthread_t stop_thread;
    assert(pthread_create(&stop_thread, NULL, stop_compaction_worker_main,
                          NULL) == 0);
    wait_for_stop_post_join_gate(&gate);

    struct myfs_compaction_queue_test_snapshot stopping;
    assert(myfs_compaction_test_queue_snapshot(&stopping) == 0);
    assert(stopping.worker_running);
    assert(stopping.stop_requested);
    assert(stopping.queued_requests == 0);
    assert(stopping.head_is_null && stopping.tail_is_null);

    myfs_adaptive_ticket_t ticket = {0};
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(generation_observe_write_locked(&writer, 0, 0, &ticket));
    myfs_unlock_file(lock);
    assert(ticket.valid);
    assert(ticket.stats_snapshot.full_windows ==
           before_ticket.adaptive_full_windows);
    assert(ticket.stats_snapshot.partial_rmw ==
           before_ticket.adaptive_partial_rmw);

    struct submitter_lock_failure failure = {
        .submitter = pthread_self(),
    };
    assert(snprintf(failure.path, sizeof(failure.path), "%s", fixture.path) <
           (int)sizeof(failure.path));
    myfs_lock_test_set_acquire_fail_hook(fail_submitter_path_lock_once,
                                         &failure);
    assert(schedule_adaptive_compaction(fixture.path, &ticket) == -ENOMEM);
    assert(failure.matching_calls == 1);
    myfs_lock_test_set_acquire_fail_hook(NULL, NULL);

    generation_adaptive_schedule_failed(&ticket);
    struct myfs_generation_registry_test_snapshot restored =
        snapshot_for(&fixture.active);
    assert(restored.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(restored.adaptive_eval_id == ticket.evaluation_id);
    assert(restored.adaptive_eval_id == before_ticket.adaptive_eval_id + 1);
    assert(restored.open_refs == before_ticket.open_refs);
    assert(restored.writer_refs == before_ticket.writer_refs);
    assert(restored.metadata_epoch == before_ticket.metadata_epoch);
    assert(restored.gc_claim_id == before_ticket.gc_claim_id);
    assert(restored.gc_state == before_ticket.gc_state);
    assert(restored.install_aliases == before_ticket.install_aliases);
    assert(restored.adaptive_full_windows ==
           before_ticket.adaptive_full_windows);
    assert(restored.adaptive_partial_rmw ==
           before_ticket.adaptive_partial_rmw);
    assert(restored.classified_since_evaluation ==
           before_ticket.classified_since_evaluation);
    assert(restored.superseded == before_ticket.superseded);
    generation_adaptive_schedule_failed(&ticket);
    struct myfs_generation_registry_test_snapshot duplicate =
        snapshot_for(&fixture.active);
    assert_registry_snapshots_equal(&duplicate, &restored);

    release_stop_post_join_gate(&gate);
    struct timespec deadline = deadline_after_ms(5000);
    assert(pthread_timedjoin_np(stop_thread, NULL, &deadline) == 0);
    myfs_compaction_test_set_stop_post_join_hook(NULL, NULL);
    assert(pthread_mutex_lock(&gate.mu) == 0);
    assert(gate.calls == 1);
    assert(pthread_mutex_unlock(&gate.mu) == 0);
    assert(pthread_cond_destroy(&gate.cv) == 0);
    assert(pthread_mutex_destroy(&gate.mu) == 0);

    unregister_handle(&writer);
    cleanup_gc_fixture(&fixture);
}

struct adaptive_drain_gate
{
    char path[PATH_MAX];
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered;
    bool release;
    unsigned calls;
};

static int pause_adaptive_handoff_for_drain(
    enum myfs_generation_handoff_test_stage stage,
    const myfs_storage_t *old_storage,
    const myfs_storage_t *new_storage, void *argument)
{
    (void)new_storage;
    struct adaptive_drain_gate *gate = argument;
    if (stage != MYFS_GENERATION_HANDOFF_TEST_BEFORE_PUBLISH ||
        strcmp(old_storage->logical_path, gate->path) != 0)
        return 0;
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->calls++;
    assert(gate->calls == 1);
    gate->entered = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    struct timespec deadline = deadline_after_ms(10000);
    while (!gate->release)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
    return 0;
}

static void wait_for_adaptive_drain_gate(struct adaptive_drain_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    while (!gate->entered)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void release_adaptive_drain_gate(struct adaptive_drain_gate *gate)
{
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->release = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static struct myfs_compaction_queue_test_snapshot
wait_for_compaction_stop_requested(void)
{
    struct timespec deadline = deadline_after_ms(5000);
    for (;;)
    {
        struct myfs_compaction_queue_test_snapshot snapshot;
        assert(myfs_compaction_test_queue_snapshot(&snapshot) == 0);
        if (snapshot.stop_requested)
            return snapshot;
        struct timespec now;
        assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
        assert(now.tv_sec < deadline.tv_sec ||
               (now.tv_sec == deadline.tv_sec &&
                now.tv_nsec < deadline.tv_nsec));
        struct timespec delay = {.tv_nsec = 1000 * 1000};
        nanosleep(&delay, NULL);
    }
}

static void unlink_path_links(const char *path)
{
    char current_path[PATH_MAX];
    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_current_path(current_path, path);
    build_data_path(data_alias, path);
    build_meta_path(meta_alias, path);
    assert(unlink(current_path) == 0 || errno == ENOENT);
    assert(unlink(data_alias) == 0 || errno == ENOENT);
    assert(unlink(meta_alias) == 0 || errno == ENOENT);
}

static void test_worker_shutdown_drains_inflight_and_queued_adaptive_work(void)
{
    stop_compaction_worker_with_timeout();
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/worker-drain-a", false);
    make_storage_compaction_eligible(&fixture.active);

    const char *second_path = "/worker-drain-b";
    myfs_storage_t second_source;
    assert(create_generation_storage(second_path, 0600, &second_source) == 0);
    create_empty_file(second_source.meta_path);
    assert(publish_generation(second_path, &second_source) == 0);
    make_storage_compaction_eligible(&second_source);

    myfs_file_handle_t first_reader;
    myfs_file_handle_t first_writer;
    myfs_file_handle_t second_reader;
    myfs_file_handle_t second_writer;
    initialize_handle(&first_reader, &fixture.active, O_RDONLY);
    initialize_handle(&first_writer, &fixture.active, O_RDWR);
    initialize_handle(&second_reader, &second_source, O_RDONLY);
    initialize_handle(&second_writer, &second_source, O_RDWR);
    register_handle(&first_reader);
    register_handle(&first_writer);
    register_handle(&second_reader);
    register_handle(&second_writer);
    myfs_adaptive_ticket_t first_ticket = queue_adaptive_ticket(&first_writer);
    myfs_adaptive_ticket_t second_ticket = queue_adaptive_ticket(&second_writer);
    unregister_handle(&first_writer);
    unregister_handle(&second_writer);

    struct adaptive_drain_gate gate = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    assert(snprintf(gate.path, sizeof(gate.path), "%s", fixture.path) <
           (int)sizeof(gate.path));
    myfs_generation_registry_test_set_handoff_hook(
        pause_adaptive_handoff_for_drain, &gate);
    assert(start_compaction_worker() == 0);
    assert(schedule_adaptive_compaction(fixture.path, &first_ticket) == 0);
    wait_for_adaptive_drain_gate(&gate);
    assert(snapshot_for(&fixture.active).adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_INFLIGHT);

    assert(schedule_adaptive_compaction(second_path, &second_ticket) == 0);
    assert(snapshot_for(&second_source).adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);
    struct myfs_compaction_queue_test_snapshot queued;
    assert(myfs_compaction_test_queue_snapshot(&queued) == 0);
    assert(queued.queued_requests == 1);
    assert(!queued.head_is_null && !queued.tail_is_null);

    pthread_t stop_thread;
    assert(pthread_create(&stop_thread, NULL, stop_compaction_worker_main,
                          NULL) == 0);
    struct myfs_compaction_queue_test_snapshot stopping =
        wait_for_compaction_stop_requested();
    assert(stopping.worker_running);
    assert(stopping.stop_requested);
    assert(stopping.queued_requests == 1);
    assert(snapshot_for(&fixture.active).adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_INFLIGHT);
    assert(snapshot_for(&second_source).adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);

    release_adaptive_drain_gate(&gate);
    struct timespec deadline = deadline_after_ms(5000);
    assert(pthread_timedjoin_np(stop_thread, NULL, &deadline) == 0);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);

    struct myfs_compaction_queue_test_snapshot drained;
    assert(myfs_compaction_test_queue_snapshot(&drained) == 0);
    assert(drained.queued_requests == 0);
    assert(drained.head_is_null && drained.tail_is_null);
    assert(!drained.worker_running);
    assert(!drained.stop_requested);
    assert(snapshot_for(&fixture.active).adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(snapshot_for(&second_source).adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_IDLE);

    myfs_storage_t first_active;
    myfs_storage_t second_active;
    assert(resolve_storage(fixture.path, &first_active) == 0);
    assert(resolve_storage(second_path, &second_active) == 0);
    assert(!storage_generation_equal(&first_active, &fixture.active));
    assert(!storage_generation_equal(&second_active, &second_source));
    int first_active_state = snapshot_for(&first_active).adaptive_state;
    int second_active_state = snapshot_for(&second_active).adaptive_state;
    assert(first_active_state != MYFS_GENERATION_ADAPTIVE_TEST_QUEUED &&
           first_active_state != MYFS_GENERATION_ADAPTIVE_TEST_INFLIGHT);
    assert(second_active_state != MYFS_GENERATION_ADAPTIVE_TEST_QUEUED &&
           second_active_state != MYFS_GENERATION_ADAPTIVE_TEST_INFLIGHT);
    assert(pthread_mutex_lock(&gate.mu) == 0);
    assert(gate.calls == 1);
    assert(pthread_mutex_unlock(&gate.mu) == 0);

    unregister_handle(&first_reader);
    unregister_handle(&second_reader);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    lock = myfs_lock_file(second_path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(second_path) == 0);
    myfs_unlock_file(lock);

    assert(pthread_cond_destroy(&gate.cv) == 0);
    assert(pthread_mutex_destroy(&gate.mu) == 0);
    unlink_path_links(fixture.path);
    unlink_path_links(second_path);
    assert(remove_generation_storage(&fixture.victim) == 0);
    assert(remove_generation_storage(&fixture.active) == 0);
    assert(remove_generation_storage(&first_active) == 0);
    assert(remove_generation_storage(&second_source) == 0);
    assert(remove_generation_storage(&second_active) == 0);
    destroy_generation_registry();
    assert(rmdir(fixture.root) == 0);
    myfs_conf = NULL;
}

static void test_worker_path_lock_failure_restores_adaptive_ticket(void)
{
    stop_compaction_worker_with_timeout();
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/worker-lock-failure", false);
    make_storage_compaction_eligible(&fixture.active);

    myfs_file_handle_t writer;
    initialize_handle(&writer, &fixture.active, O_RDWR);
    register_handle(&writer);

    /* Prime claimable evidence while a seed ticket keeps the record QUEUED,
     * then restore the seed.  A zero-sample observation can now create the
     * ticket under test without changing the pre-ticket statistics. */
    myfs_adaptive_ticket_t seed = queue_adaptive_ticket(&writer);
    myfs_adaptive_ticket_t unexpected = {0};
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(!generation_observe_write_locked(&writer, 0, 17, &unexpected));
    myfs_unlock_file(lock);
    assert(!unexpected.valid);
    generation_adaptive_schedule_failed(&seed);
    struct myfs_generation_registry_test_snapshot before_ticket =
        snapshot_for(&fixture.active);
    assert(before_ticket.adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(before_ticket.adaptive_full_windows == 0);
    assert(before_ticket.adaptive_partial_rmw == 145);
    assert(before_ticket.classified_since_evaluation == 145);

    myfs_adaptive_ticket_t ticket = {0};
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(generation_observe_write_locked(&writer, 0, 0, &ticket));
    myfs_unlock_file(lock);
    assert(ticket.valid);
    assert(ticket.stats_snapshot.full_windows ==
           before_ticket.adaptive_full_windows);
    assert(ticket.stats_snapshot.partial_rmw ==
           before_ticket.adaptive_partial_rmw);
    struct myfs_generation_registry_test_snapshot queued =
        snapshot_for(&fixture.active);
    assert(queued.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);
    assert(queued.adaptive_eval_id == ticket.evaluation_id);
    assert(queued.adaptive_full_windows == 0);
    assert(queued.adaptive_partial_rmw == 0);
    assert(queued.classified_since_evaluation == 0);

    struct worker_lock_failure_gate gate;
    initialize_worker_lock_failure_gate(&gate, fixture.path);
    assert(start_compaction_worker() == 0);
    myfs_lock_test_set_acquire_fail_hook(fail_worker_path_lock_once, &gate);
    assert(schedule_adaptive_compaction(fixture.path, &ticket) == 0);
    wait_for_worker_lock_failure_gate(&gate);

    /* The hook fails before any lock-table shard mutex is acquired.  This
     * exercises the worker's handling of a NULL myfs_lock_file() result, not
     * lock.c's own allocation-failure cleanup. */
    queued = snapshot_for(&fixture.active);
    assert(queued.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);
    assert(queued.adaptive_eval_id == ticket.evaluation_id);
    assert(queued.adaptive_full_windows == 0);
    assert(queued.adaptive_partial_rmw == 0);
    assert(queued.classified_since_evaluation == 0);

    release_worker_lock_failure_gate(&gate);
    stop_compaction_worker_with_timeout();
    myfs_lock_test_set_acquire_fail_hook(NULL, NULL);

    assert(pthread_mutex_lock(&gate.mu) == 0);
    assert(gate.matching_calls == 1);
    assert(pthread_mutex_unlock(&gate.mu) == 0);
    struct myfs_generation_registry_test_snapshot restored =
        snapshot_for(&fixture.active);
    assert(restored.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(restored.adaptive_eval_id == ticket.evaluation_id);
    assert(restored.adaptive_eval_id == before_ticket.adaptive_eval_id + 1);
    assert(restored.open_refs == before_ticket.open_refs);
    assert(restored.writer_refs == before_ticket.writer_refs);
    assert(restored.metadata_epoch == before_ticket.metadata_epoch);
    assert(restored.gc_claim_id == before_ticket.gc_claim_id);
    assert(restored.gc_state == before_ticket.gc_state);
    assert(restored.install_aliases == before_ticket.install_aliases);
    assert(restored.adaptive_full_windows ==
           before_ticket.adaptive_full_windows);
    assert(restored.adaptive_partial_rmw ==
           before_ticket.adaptive_partial_rmw);
    assert(restored.classified_since_evaluation ==
           before_ticket.classified_since_evaluation);
    assert(restored.superseded == before_ticket.superseded);

    myfs_adaptive_ticket_t retry = {0};
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(generation_observe_write_locked(&writer, 0, 0, &retry));
    myfs_unlock_file(lock);
    assert(retry.valid);
    assert(retry.evaluation_id == ticket.evaluation_id + 1);
    assert(retry.stats_snapshot.full_windows ==
           before_ticket.adaptive_full_windows);
    assert(retry.stats_snapshot.partial_rmw ==
           before_ticket.adaptive_partial_rmw);
    generation_adaptive_schedule_failed(&retry);
    struct myfs_generation_registry_test_snapshot retry_restored =
        snapshot_for(&fixture.active);
    assert(retry_restored.adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(retry_restored.adaptive_full_windows ==
           restored.adaptive_full_windows);
    assert(retry_restored.adaptive_partial_rmw ==
           restored.adaptive_partial_rmw);
    assert(retry_restored.classified_since_evaluation ==
           restored.classified_since_evaluation);

    /* Keep one reader on the source generation but remove its final writer so
     * ordinary compaction must get past both the queued-adaptive and writer
     * deferral guards.  Publication of a different active generation is the
     * observable proof that ordinary compaction was actually claimed. */
    myfs_file_handle_t reader;
    initialize_handle(&reader, &fixture.active, O_RDONLY);
    register_handle(&reader);
    unregister_handle(&writer);
    struct myfs_generation_registry_test_snapshot before_ordinary =
        snapshot_for(&fixture.active);
    assert(before_ordinary.open_refs == 1);
    assert(before_ordinary.writer_refs == 0);
    assert(before_ordinary.adaptive_state ==
           MYFS_GENERATION_ADAPTIVE_TEST_IDLE);

    myfs_storage_t old_active = fixture.active;
    assert(schedule_compaction(fixture.path) == 0);
    myfs_storage_t active;
    assert(resolve_storage(fixture.path, &active) == 0);
    assert(!storage_generation_equal(&active, &old_active));
    struct myfs_generation_registry_test_snapshot retired =
        snapshot_for(&old_active);
    assert(retired.open_refs == 1);
    assert(retired.writer_refs == 0);
    assert(retired.superseded);
    assert(retired.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    struct myfs_generation_registry_test_snapshot published =
        snapshot_for(&active);
    assert(published.open_refs == 0);
    assert(published.writer_refs == 0);

    unregister_handle(&reader);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&old_active, NULL) ==
           -ENOENT);
    fixture.active = active;
    destroy_worker_lock_failure_gate(&gate);
    cleanup_gc_fixture(&fixture);
}

static void test_schedule_failure_restoration_races_claimed_gc(void)
{
    struct gc_fixture fixture;
    initialize_gc_fixture(&fixture, "/gc-schedule-restore-before-finalize",
                          false);
    myfs_file_handle_t writer;
    initialize_handle(&writer, &fixture.victim, O_RDWR);
    register_handle(&writer);
    myfs_adaptive_ticket_t ticket = queue_adaptive_ticket(&writer);
    unregister_handle(&writer);
    mark_generation(&fixture.victim, false);

    struct gc_gate gate;
    initialize_gate(&gate, MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE,
                    fixture.path);
    gate.result = -EIO;
    myfs_generation_registry_test_set_gc_hook(gc_gate_hook, &gate);
    struct gc_thread_context gc_context = {.path = fixture.path};
    pthread_t gc_thread;
    assert(pthread_create(&gc_thread, NULL, gc_thread_main, &gc_context) == 0);
    assert(wait_for_gate(&gate));
    struct myfs_generation_registry_test_snapshot claimed =
        snapshot_for(&fixture.victim);
    assert(claimed.gc_state == MYFS_GENERATION_GC_TEST_CLAIMED);
    assert(claimed.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);

    generation_adaptive_schedule_failed(&ticket);
    assert_ticket_restored_once(&fixture.victim, &ticket,
                                MYFS_GENERATION_GC_TEST_CLAIMED);
    release_gate(&gate);
    assert(pthread_join(gc_thread, NULL) == 0);
    assert(gc_context.result == -EIO);
    assert_ticket_restored_once(&fixture.victim, &ticket,
                                MYFS_GENERATION_GC_TEST_PENDING);

    myfs_generation_registry_test_set_gc_hook(NULL, NULL);
    myfs_file_lock_t *lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    destroy_gate(&gate);
    cleanup_gc_fixture(&fixture);

    initialize_gc_fixture(&fixture, "/gc-restore-then-successful-finalize",
                          false);
    initialize_handle(&writer, &fixture.victim, O_RDWR);
    register_handle(&writer);
    ticket = queue_adaptive_ticket(&writer);
    unregister_handle(&writer);
    mark_generation(&fixture.victim, false);

    initialize_gate(&gate, MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE,
                    fixture.path);
    myfs_generation_registry_test_set_gc_hook(gc_gate_hook, &gate);
    gc_context = (struct gc_thread_context){.path = fixture.path};
    assert(pthread_create(&gc_thread, NULL, gc_thread_main, &gc_context) == 0);
    assert(wait_for_gate(&gate));
    claimed = snapshot_for(&fixture.victim);
    assert(claimed.gc_state == MYFS_GENERATION_GC_TEST_CLAIMED);
    uint64_t successful_claim_id = claimed.gc_claim_id;
    generation_adaptive_schedule_failed(&ticket);
    assert_ticket_restored_once(&fixture.victim, &ticket,
                                MYFS_GENERATION_GC_TEST_CLAIMED);
    assert(snapshot_for(&fixture.victim).gc_claim_id == successful_claim_id);
    release_gate(&gate);
    assert(pthread_join(gc_thread, NULL) == 0);
    assert(gc_context.result == 0);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    generation_adaptive_schedule_failed(&ticket);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    myfs_generation_registry_test_set_gc_hook(NULL, NULL);
    destroy_gate(&gate);
    cleanup_gc_fixture(&fixture);

    initialize_gc_fixture(&fixture, "/gc-schedule-restore-after-finalize",
                          false);
    initialize_handle(&writer, &fixture.victim, O_RDWR);
    register_handle(&writer);
    ticket = queue_adaptive_ticket(&writer);
    unregister_handle(&writer);
    mark_generation(&fixture.victim, false);

    initialize_gate(&gate, MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE,
                    fixture.path);
    gate.release = true;
    gate.result = -EIO;
    myfs_generation_registry_test_set_gc_hook(gc_gate_hook, &gate);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == -EIO);
    myfs_unlock_file(lock);
    struct myfs_generation_registry_test_snapshot pending =
        snapshot_for(&fixture.victim);
    assert(pending.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(pending.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);

    myfs_adaptive_ticket_t wrong_ticket = ticket;
    wrong_ticket.evaluation_id++;
    generation_adaptive_schedule_failed(&wrong_ticket);
    pending = snapshot_for(&fixture.victim);
    assert(pending.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_QUEUED);
    assert(pending.adaptive_partial_rmw == 0);
    generation_adaptive_schedule_failed(&ticket);
    assert_ticket_restored_once(&fixture.victim, &ticket,
                                MYFS_GENERATION_GC_TEST_PENDING);

    myfs_generation_registry_test_set_gc_hook(NULL, NULL);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    destroy_gate(&gate);
    cleanup_gc_fixture(&fixture);

    initialize_gc_fixture(&fixture, "/gc-schedule-restore-after-retire",
                          false);
    initialize_handle(&writer, &fixture.victim, O_RDWR);
    register_handle(&writer);
    ticket = queue_adaptive_ticket(&writer);
    unregister_handle(&writer);
    mark_generation(&fixture.victim, false);
    lock = myfs_lock_file(fixture.path);
    assert(lock != NULL);
    assert(run_generation_gc_locked(fixture.path) == 0);
    myfs_unlock_file(lock);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    generation_adaptive_schedule_failed(&ticket);
    assert(myfs_generation_registry_test_snapshot(&fixture.victim, NULL) ==
           -ENOENT);
    cleanup_gc_fixture(&fixture);
}

struct churn_context
{
    unsigned worker;
    atomic_int *failed;
};

static void *registry_churn_main(void *argument)
{
    struct churn_context *context = argument;
    char path[64];
    assert(snprintf(path, sizeof(path), "/registry-churn-%u",
                    context->worker) > 0);
    myfs_storage_t storage = fake_storage(path, GENERATION_A);
    for (unsigned i = 0; i < 1000; i++)
    {
        myfs_file_handle_t handle;
        initialize_handle(&handle, &storage, (i & 1) ? O_RDONLY : O_RDWR);
        myfs_file_lock_t *lock = myfs_lock_file(path);
        assert(lock != NULL);
        assert(register_generation_handle_locked(&handle) == 0);
        struct myfs_generation_registry_test_snapshot snapshot =
            snapshot_for(&storage);
        if (snapshot.open_refs != 1)
            atomic_store(context->failed, 1);
        generation_bump_metadata_epoch_locked(&handle);
        unregister_generation_handle_locked(&handle);
        myfs_unlock_file(lock);
        assert(pthread_rwlock_destroy(&handle.cache_lock) == 0);
    }
    return NULL;
}

static void test_concurrent_shard_churn_is_race_free(void)
{
    enum { THREADS = 8 };
    atomic_int failed;
    atomic_init(&failed, 0);
    pthread_t threads[THREADS];
    struct churn_context contexts[THREADS];
    for (unsigned i = 0; i < THREADS; i++)
    {
        contexts[i] = (struct churn_context){.worker = i, .failed = &failed};
        assert(pthread_create(&threads[i], NULL, registry_churn_main,
                              &contexts[i]) == 0);
    }
    for (unsigned i = 0; i < THREADS; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&failed) == 0);
    assert(myfs_generation_registry_test_record_count() == 0);
}

int main(void)
{
    test_logical_path_sharding_preserves_generation_identity();
    test_legacy_identity_rules_are_unchanged();
    test_collision_chain_unlinks_head_middle_and_tail();
    test_sharded_destroy_is_idempotent_and_reusable();
    test_gc_io_pause_points_release_registry_shards();
    test_gc_failures_restore_pending_for_retry();
    test_open_references_defer_retirement_safely();
    test_legacy_references_defer_alias_replacement();
    test_alias_install_can_finish_before_old_reader_retires();
    test_active_generation_is_never_removed();
    test_partial_removal_retries_after_files_disappear();
    test_quiescent_sweep_continues_after_first_error();
    test_repeated_marks_merge_alias_requirement();
    test_schedule_failure_restoration_races_claimed_gc();
    test_stopping_worker_uses_synchronous_fallback();
    test_worker_shutdown_drains_inflight_and_queued_adaptive_work();
    test_worker_path_lock_failure_restores_adaptive_ticket();
    test_concurrent_shard_churn_is_race_free();
    destroy_generation_registry();
    destroy_lock_table();
    return 0;
}
