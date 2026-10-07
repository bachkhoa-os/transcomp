#include "myfs.h"
#include "core/compact_test.h"

#include <assert.h>
#include <stdatomic.h>

static void test_open_handle_reads_from_its_cache(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;

    struct fuse_file_info fi = {0};
    fi.flags = O_RDWR;
    assert(myfs_create("/cached", 0600, &fi) == 0);
    assert(myfs_write("/cached", "cached-data", 11, 0, &fi) == 11);

    myfs_file_handle_t *handle = (myfs_file_handle_t *)(uintptr_t)fi.fh;
    assert(handle != NULL);
    assert(handle->cache_valid);
    assert(handle->cached_inode.chunk_map.num_chunks == 1);
    assert(myfs_truncate("/cached", 6, &fi) == 0);
    assert(INODE_LSIZE(handle->cached_inode) == 6);

    /* A normal handle read must not touch backing metadata again.  Corrupting
     * the pathname after the successful commit makes any reload observable. */
    int corrupt_fd = open(handle->storage.meta_path, O_WRONLY | O_CLOEXEC);
    assert(corrupt_fd >= 0);
    unsigned char bad = 'X';
    assert(pwrite(corrupt_fd, &bad, 1, 0) == 1);
    assert(close(corrupt_fd) == 0);

    for (int i = 0; i < 100; i++)
    {
        char out[12] = {0};
        assert(myfs_read("/cached", out, 11, 0, &fi) == 6);
        assert(memcmp(out, "cached", 6) == 0);
    }

    assert(myfs_release("/cached", &fi) == 0);
    myfs_conf = NULL;
}

static void test_resize_policy_is_ratio_cost_and_cooldown_gated(void)
{
    uint32_t target = 0;
    myfs_window_stats_t partial = {.partial_rmw = 128};
    assert(myfs_choose_resize_target(64 * 1024, 4 * 1024 * 1024,
                                     partial, false, &target) == 1);
    assert(target == 32 * 1024);
    assert(myfs_choose_resize_target(64 * 1024, 8 * 1024 * 1024,
                                     partial, false, &target) == 0);
    assert(myfs_choose_resize_target(64 * 1024, 1,
                                     partial, true, &target) == 0);

    myfs_window_stats_t full = {.full_windows = 116, .partial_rmw = 12};
    assert(myfs_choose_resize_target(64 * 1024, 4 * 1024 * 1024,
                                     full, false, &target) == 1);
    assert(target == 128 * 1024);

    myfs_window_stats_t weak = {.full_windows = 115, .partial_rmw = 13};
    assert(myfs_choose_resize_target(64 * 1024, 1,
                                     weak, false, &target) == 0);

    assert(myfs_choose_resize_target(MYFS_MIN_WINDOW_SIZE, 1,
                                     partial, false, &target) == 0);
}

static void test_metadata_journal_is_periodically_checkpointed(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    struct fuse_file_info fi = {.flags = O_RDWR};
    assert(myfs_create("/checkpoint", 0600, &fi) == 0);
    assert(myfs_write("/checkpoint", "abcdefgh", 8, 0, &fi) == 8);
    for (int i = 0; i < 7; i++)
        assert(myfs_write("/checkpoint", "Z", 1, i, &fi) == 1);
    myfs_file_handle_t *handle = (myfs_file_handle_t *)(uintptr_t)fi.fh;
    assert(handle->cached_inode.metadata_delta_count == 0);
    assert(handle->cached_inode.metadata_journal_bytes == 0);
    assert(myfs_release("/checkpoint", &fi) == 0);

    struct fuse_file_info truncate_fi = {.flags = O_RDWR};
    assert(myfs_create("/truncate-checkpoint", 0600, &truncate_fi) == 0);
    assert(myfs_write("/truncate-checkpoint", "abcdefgh", 8, 0,
                      &truncate_fi) == 8);
    for (int i = 0; i < 7; i++)
        assert(myfs_truncate("/truncate-checkpoint", (i & 1) ? 8 : 7,
                             &truncate_fi) == 0);
    myfs_file_handle_t *truncate_handle =
        (myfs_file_handle_t *)(uintptr_t)truncate_fi.fh;
    assert(truncate_handle->cached_inode.metadata_delta_count == 0);
    assert(truncate_handle->cached_inode.metadata_journal_bytes == 0);
    assert(myfs_release("/truncate-checkpoint", &truncate_fi) == 0);
    myfs_conf = NULL;
}

static void test_file_uses_its_persisted_window_size(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    char data_path[PATH_MAX];
    char meta_path[PATH_MAX];
    build_data_path(data_path, "/small-window");
    build_meta_path(meta_path, "/small-window");
    int data_fd = open(data_path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    assert(data_fd >= 0);
    assert(close(data_fd) == 0);
    myfs_inode_t inode = {.window_size = 16 * 1024};
    inode.chunk_map.fully_packed = true;
    assert(save_chunk_map_to_path(meta_path, &inode) == 0);

    struct fuse_file_info fi = {.flags = O_RDWR};
    assert(myfs_open("/small-window", &fi) == 0);
    char *payload = malloc(32 * 1024);
    assert(payload != NULL);
    memset(payload, 'W', 32 * 1024);
    assert(myfs_write("/small-window", payload, 32 * 1024, 0, &fi)
           == 32 * 1024);
    myfs_file_handle_t *handle = (myfs_file_handle_t *)(uintptr_t)fi.fh;
    assert(handle->cached_inode.window_size == 16 * 1024);
    assert(handle->cached_inode.chunk_map.num_chunks == 2);
    assert(handle->cached_inode.chunk_map.chunks[1].logical_offset == 16 * 1024);
    char *readback = malloc(32 * 1024);
    assert(readback != NULL);
    assert(myfs_read("/small-window", readback, 32 * 1024, 0, &fi)
           == 32 * 1024);
    assert(memcmp(payload, readback, 32 * 1024) == 0);
    free(readback);
    free(payload);
    assert(myfs_release("/small-window", &fi) == 0);
    myfs_conf = NULL;
}

static void test_partial_legacy_repack_keeps_tolerant_reads(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    char data_path[PATH_MAX];
    char meta_path[PATH_MAX];
    build_data_path(data_path, "/legacy-partial");
    build_meta_path(meta_path, "/legacy-partial");
    int fd = open(data_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, "AAAABBBB", 8) == 8);
    assert(close(fd) == 0);

    myfs_chunk_t chunks[2] = {
        {.logical_offset = 100, .physical_offset = 0,
         .raw_size = 4, .stored_size = 4},
        {.logical_offset = 70000, .physical_offset = 4,
         .raw_size = 4, .stored_size = 4},
    };
    uint32_t count = 2;
    uint64_t logical_size = 70004;
    fd = open(meta_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, &count, sizeof(count)) == (ssize_t)sizeof(count));
    assert(write(fd, &logical_size, sizeof(logical_size)) ==
           (ssize_t)sizeof(logical_size));
    assert(write(fd, chunks, sizeof(chunks)) == (ssize_t)sizeof(chunks));
    assert(close(fd) == 0);

    struct fuse_file_info fi = {.flags = O_RDWR};
    assert(myfs_open("/legacy-partial", &fi) == 0);
    assert(myfs_write("/legacy-partial", "Z", 1, 101, &fi) == 1);
    myfs_file_handle_t *handle = (myfs_file_handle_t *)(uintptr_t)fi.fh;
    assert(!handle->cached_inode.chunk_map.fully_packed);
    char out[4] = {0};
    assert(myfs_read("/legacy-partial", out, sizeof(out), 70000, &fi) == 4);
    assert(memcmp(out, "BBBB", 4) == 0);
    assert(myfs_release("/legacy-partial", &fi) == 0);
    myfs_conf = NULL;
}

static void test_readonly_release_does_not_claim_writer_sample(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;

    struct fuse_file_info writer = {.flags = O_RDWR};
    assert(myfs_create("/readonly-release", 0600, &writer) == 0);
    assert(myfs_write("/readonly-release", "x", 1, 0, &writer) == 1);
    struct fuse_file_info reader = {.flags = O_RDONLY};
    assert(myfs_open("/readonly-release", &reader) == 0);

    myfs_file_handle_t *writer_handle =
        (myfs_file_handle_t *)(uintptr_t)writer.fh;
    myfs_file_lock_t *lk = myfs_lock_file("/readonly-release");
    assert(lk != NULL);
    assert(!generation_observe_write_locked(writer_handle, 0, 128, NULL));
    myfs_unlock_file(lk);

    /* Releasing a reader while exactly one writer remains must not consume
     * that writer's sample or trigger a live resize. */
    assert(myfs_release("/readonly-release", &reader) == 0);
    assert(pthread_rwlock_rdlock(&writer_handle->cache_lock) == 0);
    assert(writer_handle->cached_inode.window_size ==
           MYFS_DEFAULT_WINDOW_SIZE);
    pthread_rwlock_unlock(&writer_handle->cache_lock);

    assert(myfs_release("/readonly-release", &writer) == 0);
    myfs_conf = NULL;
}

struct one_read_context
{
    const char *path;
    struct fuse_file_info *fi;
    char data[16];
    int result;
};

static void *one_reader(void *arg)
{
    struct one_read_context *ctx = arg;
    ctx->result = myfs_read(ctx->path, ctx->data, sizeof(ctx->data), 0,
                            ctx->fi);
    return NULL;
}

static void test_getattr_epoch_and_truncate_preserve_reader_bundle(
    const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;

    struct fuse_file_info writer = {.flags = O_RDWR};
    assert(myfs_create("/coherent", 0600, &writer) == 0);
    assert(myfs_write("/coherent", "abc", 3, 0, &writer) == 3);
    struct fuse_file_info observer = {.flags = O_RDONLY};
    assert(myfs_open("/coherent", &observer) == 0);
    assert(myfs_write("/coherent", "def", 3, 3, &writer) == 3);

    struct stat logical_st;
    assert(myfs_getattr("/coherent", &logical_st, &observer) == 0);
    assert(logical_st.st_size == 6);

    assert(setenv("MYFS_TEST_READ_HOLD_MS", "100", 1) == 0);
    struct one_read_context read_ctx = {
        .path = "/coherent",
        .fi = &observer,
        .result = -1,
    };
    pthread_t reader;
    assert(pthread_create(&reader, NULL, one_reader, &read_ctx) == 0);
    struct timespec overlap = {.tv_nsec = 20 * 1000 * 1000};
    nanosleep(&overlap, NULL);

    struct fuse_file_info truncator = {.flags = O_RDWR | O_TRUNC};
    assert(myfs_open("/coherent", &truncator) == 0);
    assert(pthread_join(reader, NULL) == 0);
    assert(read_ctx.result == 6);
    assert(memcmp(read_ctx.data, "abcdef", 6) == 0);
    unsetenv("MYFS_TEST_READ_HOLD_MS");

    myfs_file_handle_t *trunc_handle =
        (myfs_file_handle_t *)(uintptr_t)truncator.fh;
    struct stat backing_st;
    assert(fstat(trunc_handle->data_fd, &backing_st) == 0);
    assert(backing_st.st_size >= 6);

    char after[8] = {0};
    assert(myfs_read("/coherent", after, sizeof(after), 0, &observer) == 0);
    assert(myfs_release("/coherent", &truncator) == 0);
    assert(myfs_release("/coherent", &observer) == 0);
    assert(myfs_release("/coherent", &writer) == 0);
    char data_path[PATH_MAX];
    build_data_path(data_path, "/coherent");
    assert(stat(data_path, &backing_st) == 0);
    assert(backing_st.st_size == 0);
    myfs_conf = NULL;
}

struct reader_context
{
    const char *path;
    struct fuse_file_info *fi;
    const unsigned char *expected;
    size_t size;
    atomic_bool stop;
    atomic_int failures;
};

static void *continuous_reader(void *arg)
{
    struct reader_context *ctx = arg;
    unsigned char *out = malloc(ctx->size);
    assert(out != NULL);
    while (!atomic_load(&ctx->stop))
    {
        int ret = myfs_read(ctx->path, (char *)out, ctx->size, 0, ctx->fi);
        if (ret != (int)ctx->size ||
            memcmp(out, ctx->expected, ctx->size) != 0)
            atomic_fetch_add(&ctx->failures, 1);
    }
    free(out);
    return NULL;
}

struct blocked_read_context
{
    const char *path;
    struct fuse_file_info *fi;
    const unsigned char *expected;
    size_t size;
    unsigned char *out;
    atomic_bool started;
    int result;
};

static void *blocked_reader(void *arg)
{
    struct blocked_read_context *ctx = arg;
    atomic_store(&ctx->started, true);
    ctx->result = myfs_read(ctx->path, (char *)ctx->out, ctx->size, 0,
                            ctx->fi);
    return NULL;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

struct handoff_test_hook
{
    enum myfs_generation_handoff_test_stage stage;
    int result;
    bool publish_alternate;
    bool make_pointer_unresolvable;
    bool verify_first_relink_shard;
    myfs_storage_t alternate;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool pause;
    bool entered;
    bool release;
    unsigned calls;
    pthread_t executing_thread;
    bool executing_thread_valid;
    unsigned first_relink_checks;
};

static int handoff_test_hook_main(
    enum myfs_generation_handoff_test_stage stage,
    const myfs_storage_t *old_storage,
    const myfs_storage_t *new_storage, void *argument)
{
    (void)new_storage;
    struct handoff_test_hook *hook = argument;
    if (stage == MYFS_GENERATION_HANDOFF_TEST_AFTER_FIRST_WRITER_RELINK &&
        hook->verify_first_relink_shard)
    {
        assert(myfs_generation_registry_test_try_path_shard(
                   old_storage->logical_path) == -EBUSY);
        assert(pthread_mutex_lock(&hook->mu) == 0);
        hook->first_relink_checks++;
        assert(pthread_mutex_unlock(&hook->mu) == 0);
        return 0;
    }
    if (stage != hook->stage)
        return 0;
    if (hook->publish_alternate)
    {
        assert(create_generation_storage(old_storage->logical_path, 0600,
                                         &hook->alternate) == 0);
        myfs_inode_t empty = {.window_size = MYFS_DEFAULT_WINDOW_SIZE};
        empty.chunk_map.fully_packed = true;
        assert(save_chunk_map_to_path(hook->alternate.meta_path, &empty) == 0);
        assert(publish_generation(hook->alternate.logical_path,
                                  &hook->alternate) == 0);
    }
    if (hook->make_pointer_unresolvable)
    {
        char current_path[PATH_MAX];
        build_current_path(current_path, old_storage->logical_path);
        int fd = open(current_path,
                      O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
        assert(fd >= 0);
        assert(write(fd, "invalid", 7) == 7);
        assert(close(fd) == 0);
    }
    assert(pthread_mutex_lock(&hook->mu) == 0);
    hook->calls++;
    hook->executing_thread = pthread_self();
    hook->executing_thread_valid = true;
    hook->entered = true;
    assert(pthread_cond_broadcast(&hook->cv) == 0);
    while (hook->pause && !hook->release)
        assert(pthread_cond_wait(&hook->cv, &hook->mu) == 0);
    int result = hook->result;
    assert(pthread_mutex_unlock(&hook->mu) == 0);
    return result;
}

static void initialize_handoff_hook(
    struct handoff_test_hook *hook,
    enum myfs_generation_handoff_test_stage stage, int result)
{
    memset(hook, 0, sizeof(*hook));
    hook->stage = stage;
    hook->result = result;
    assert(pthread_mutex_init(&hook->mu, NULL) == 0);
    assert(pthread_cond_init(&hook->cv, NULL) == 0);
}

static void destroy_handoff_hook(struct handoff_test_hook *hook)
{
    assert(pthread_cond_destroy(&hook->cv) == 0);
    assert(pthread_mutex_destroy(&hook->mu) == 0);
}

static void wait_for_handoff_hook(struct handoff_test_hook *hook)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    assert(pthread_mutex_lock(&hook->mu) == 0);
    while (!hook->entered)
    {
        int status = pthread_cond_timedwait(&hook->cv, &hook->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&hook->mu) == 0);
}

static void release_handoff_hook(struct handoff_test_hook *hook)
{
    assert(pthread_mutex_lock(&hook->mu) == 0);
    hook->release = true;
    assert(pthread_cond_broadcast(&hook->cv) == 0);
    assert(pthread_mutex_unlock(&hook->mu) == 0);
}

static struct myfs_generation_registry_test_snapshot registry_snapshot(
    const myfs_storage_t *storage)
{
    struct myfs_generation_registry_test_snapshot snapshot;
    assert(myfs_generation_registry_test_snapshot(storage, &snapshot) == 0);
    return snapshot;
}

static void write_partial_samples(const char *path,
                                  struct fuse_file_info *writer)
{
    for (unsigned i = 0; i < 128; i++)
        assert(myfs_write(path, "X", 1, 0, writer) == 1);
}

static void assert_handle_bundle(myfs_file_handle_t *handle,
                                 const myfs_storage_t *storage,
                                 uint32_t window_size)
{
    assert(pthread_rwlock_rdlock(&handle->cache_lock) == 0);
    assert(storage_generation_equal(&handle->storage, storage));
    assert(handle->cached_inode.window_size == window_size);
    struct stat handle_data;
    struct stat path_data;
    struct stat handle_meta;
    struct stat path_meta;
    assert(fstat(handle->data_fd, &handle_data) == 0);
    assert(stat(storage->data_path, &path_data) == 0);
    assert(fstat(handle->meta_fd, &handle_meta) == 0);
    assert(stat(storage->meta_path, &path_meta) == 0);
    assert(handle_data.st_dev == path_data.st_dev &&
           handle_data.st_ino == path_data.st_ino);
    assert(handle_meta.st_dev == path_meta.st_dev &&
           handle_meta.st_ino == path_meta.st_ino);
    pthread_rwlock_unlock(&handle->cache_lock);
}

static uint32_t wait_for_handle_window(myfs_file_handle_t *handle,
                                       uint32_t expected)
{
    uint32_t window_size = 0;
    for (int attempt = 0; attempt < 500; attempt++)
    {
        assert(pthread_rwlock_rdlock(&handle->cache_lock) == 0);
        window_size = handle->cached_inode.window_size;
        pthread_rwlock_unlock(&handle->cache_lock);
        if (window_size == expected)
            break;
        struct timespec delay = {.tv_nsec = 10 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }
    return window_size;
}

static void wait_for_path_quiescence(const char *path)
{
    myfs_file_lock_t *lock = myfs_lock_file(path);
    assert(lock != NULL);
    myfs_unlock_file(lock);
}

static void test_handoff_publication_failure_outcomes(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    stop_compaction_worker();

    struct fuse_file_info writer = {.flags = O_RDWR};
    assert(myfs_create("/pre-publish-failure", 0600, &writer) == 0);
    assert(myfs_write("/pre-publish-failure", "initial-data", 12, 0,
                      &writer) == 12);
    myfs_file_handle_t *writer_handle =
        (myfs_file_handle_t *)(uintptr_t)writer.fh;
    myfs_storage_t old_storage = writer_handle->storage;
    struct handoff_test_hook hook;
    initialize_handoff_hook(&hook,
                            MYFS_GENERATION_HANDOFF_TEST_BEFORE_PUBLISH,
                            -EIO);
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    write_partial_samples("/pre-publish-failure", &writer);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(hook.calls == 1);
    myfs_storage_t active;
    assert(resolve_storage("/pre-publish-failure", &active) == 0);
    assert(storage_generation_equal(&active, &old_storage));
    assert_handle_bundle(writer_handle, &old_storage,
                         MYFS_DEFAULT_WINDOW_SIZE);
    struct myfs_generation_registry_test_snapshot restored =
        registry_snapshot(&old_storage);
    assert(restored.adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(restored.adaptive_partial_rmw == 128);
    assert(restored.classified_since_evaluation == 0);
    assert(!restored.superseded);
    destroy_handoff_hook(&hook);
    assert(myfs_release("/pre-publish-failure", &writer) == 0);

    writer = (struct fuse_file_info){.flags = O_RDWR};
    assert(myfs_create("/resolve-failure", 0600, &writer) == 0);
    assert(myfs_write("/resolve-failure", "initial-data", 12, 0,
                      &writer) == 12);
    writer_handle = (myfs_file_handle_t *)(uintptr_t)writer.fh;
    old_storage = writer_handle->storage;
    initialize_handoff_hook(&hook,
                            MYFS_GENERATION_HANDOFF_TEST_BEFORE_PUBLISH,
                            -EIO);
    hook.make_pointer_unresolvable = true;
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    write_partial_samples("/resolve-failure", &writer);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(hook.calls == 1);
    assert(resolve_storage("/resolve-failure", &active) == -EIO);
    assert(storage_generation_equal(&writer_handle->storage, &old_storage));
    struct myfs_generation_registry_test_snapshot unresolved_snapshot =
        registry_snapshot(&old_storage);
    assert(unresolved_snapshot.superseded);
    assert(myfs_write("/resolve-failure", "Y", 1, 1, &writer) == -ESTALE);
    destroy_handoff_hook(&hook);
    assert(myfs_release("/resolve-failure", &writer) == 0);

    writer = (struct fuse_file_info){.flags = O_RDWR};
    assert(myfs_create("/visible-sync-failure", 0600, &writer) == 0);
    assert(myfs_write("/visible-sync-failure", "initial-data", 12, 0,
                      &writer) == 12);
    struct fuse_file_info second_writer = {.flags = O_RDWR};
    assert(myfs_open("/visible-sync-failure", &second_writer) == 0);
    struct fuse_file_info old_reader = {.flags = O_RDONLY};
    assert(myfs_open("/visible-sync-failure", &old_reader) == 0);
    writer_handle = (myfs_file_handle_t *)(uintptr_t)writer.fh;
    myfs_file_handle_t *second_handle =
        (myfs_file_handle_t *)(uintptr_t)second_writer.fh;
    myfs_file_handle_t *reader_handle =
        (myfs_file_handle_t *)(uintptr_t)old_reader.fh;
    struct stat reader_data_before;
    struct stat reader_meta_before;
    assert(fstat(reader_handle->data_fd, &reader_data_before) == 0);
    assert(fstat(reader_handle->meta_fd, &reader_meta_before) == 0);
    old_storage = writer_handle->storage;
    initialize_handoff_hook(
        &hook, MYFS_GENERATION_HANDOFF_TEST_AFTER_VISIBLE_PUBLISH, -EIO);
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    write_partial_samples("/visible-sync-failure", &writer);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(hook.calls == 1);
    assert(resolve_storage("/visible-sync-failure", &active) == 0);
    assert(!storage_generation_equal(&active, &old_storage));
    assert_handle_bundle(writer_handle, &active, 32 * 1024);
    assert_handle_bundle(second_handle, &active, 32 * 1024);
    assert(pthread_rwlock_rdlock(&reader_handle->cache_lock) == 0);
    assert(storage_generation_equal(&reader_handle->storage, &old_storage));
    assert(reader_handle->cached_inode.window_size ==
           MYFS_DEFAULT_WINDOW_SIZE);
    struct stat reader_data_after;
    struct stat reader_meta_after;
    assert(fstat(reader_handle->data_fd, &reader_data_after) == 0);
    assert(fstat(reader_handle->meta_fd, &reader_meta_after) == 0);
    assert(reader_data_before.st_dev == reader_data_after.st_dev &&
           reader_data_before.st_ino == reader_data_after.st_ino);
    assert(reader_meta_before.st_dev == reader_meta_after.st_dev &&
           reader_meta_before.st_ino == reader_meta_after.st_ino);
    pthread_rwlock_unlock(&reader_handle->cache_lock);
    struct myfs_generation_registry_test_snapshot old_snapshot =
        registry_snapshot(&old_storage);
    struct myfs_generation_registry_test_snapshot new_snapshot =
        registry_snapshot(&active);
    assert(old_snapshot.open_refs == 1 && old_snapshot.writer_refs == 0);
    assert(old_snapshot.gc_state == MYFS_GENERATION_GC_TEST_NONE);
    assert(new_snapshot.open_refs == 2 && new_snapshot.writer_refs == 2);
    destroy_handoff_hook(&hook);
    assert(myfs_release("/visible-sync-failure", &writer) == 0);
    assert(myfs_release("/visible-sync-failure", &second_writer) == 0);
    assert(myfs_release("/visible-sync-failure", &old_reader) == 0);

    writer = (struct fuse_file_info){.flags = O_RDWR};
    assert(myfs_create("/record-allocation-failure", 0600, &writer) == 0);
    assert(myfs_write("/record-allocation-failure", "initial-data", 12, 0,
                      &writer) == 12);
    writer_handle = (myfs_file_handle_t *)(uintptr_t)writer.fh;
    old_storage = writer_handle->storage;
    initialize_handoff_hook(
        &hook, MYFS_GENERATION_HANDOFF_TEST_BEFORE_RECORD_CREATE, -ENOMEM);
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    write_partial_samples("/record-allocation-failure", &writer);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(hook.calls == 1);
    assert(resolve_storage("/record-allocation-failure", &active) == 0);
    assert(!storage_generation_equal(&active, &old_storage));
    assert(storage_generation_equal(&writer_handle->storage, &old_storage));
    old_snapshot = registry_snapshot(&old_storage);
    assert(old_snapshot.superseded);
    assert(old_snapshot.adaptive_partial_rmw == 128);
    assert(old_snapshot.classified_since_evaluation == 0);
    assert(myfs_generation_registry_test_snapshot(&active, NULL) == -ENOENT);
    assert(myfs_write("/record-allocation-failure", "Y", 1, 1, &writer) ==
           -ESTALE);
    destroy_handoff_hook(&hook);
    assert(myfs_release("/record-allocation-failure", &writer) == 0);

    writer = (struct fuse_file_info){.flags = O_RDWR};
    assert(myfs_create("/ambiguous-publication", 0600, &writer) == 0);
    assert(myfs_write("/ambiguous-publication", "initial-data", 12, 0,
                      &writer) == 12);
    writer_handle = (myfs_file_handle_t *)(uintptr_t)writer.fh;
    old_storage = writer_handle->storage;
    initialize_handoff_hook(&hook,
                            MYFS_GENERATION_HANDOFF_TEST_BEFORE_PUBLISH,
                            -EIO);
    hook.publish_alternate = true;
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    write_partial_samples("/ambiguous-publication", &writer);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(resolve_storage("/ambiguous-publication", &active) == 0);
    assert(storage_generation_equal(&active, &hook.alternate));
    old_snapshot = registry_snapshot(&old_storage);
    assert(old_snapshot.superseded);
    assert(myfs_write("/ambiguous-publication", "Y", 1, 1, &writer) ==
           -ESTALE);
    destroy_handoff_hook(&hook);
    assert(myfs_release("/ambiguous-publication", &writer) == 0);
    myfs_conf = NULL;
}

struct execution_context_outcome
{
    uint32_t writer_window;
    uint32_t reader_window;
    unsigned old_open_refs;
    unsigned old_writer_refs;
    unsigned new_open_refs;
    unsigned new_writer_refs;
    int old_gc_state;
    int old_adaptive_state;
    int new_adaptive_state;
    uint64_t old_full_windows;
    uint64_t old_partial_rmw;
    uint64_t new_full_windows;
    uint64_t new_partial_rmw;
    bool old_superseded;
    bool old_retired_on_reader_release;
};

static struct execution_context_outcome run_execution_context_case(
    const char *dir, const char *path, bool worker_running)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    stop_compaction_worker();
    if (worker_running)
        assert(start_compaction_worker() == 0);

    struct fuse_file_info writer = {.flags = O_RDWR};
    assert(myfs_create(path, 0600, &writer) == 0);
    assert(myfs_write(path, "initial-data", 12, 0, &writer) == 12);
    struct fuse_file_info reader = {.flags = O_RDONLY};
    assert(myfs_open(path, &reader) == 0);
    myfs_file_handle_t *writer_handle =
        (myfs_file_handle_t *)(uintptr_t)writer.fh;
    myfs_file_handle_t *reader_handle =
        (myfs_file_handle_t *)(uintptr_t)reader.fh;
    myfs_storage_t old_storage = writer_handle->storage;

    struct handoff_test_hook hook;
    initialize_handoff_hook(
        &hook, MYFS_GENERATION_HANDOFF_TEST_AFTER_REGISTRY_TRANSACTION, 0);
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    pthread_t caller = pthread_self();
    write_partial_samples(path, &writer);
    assert(wait_for_handle_window(writer_handle, 32 * 1024) == 32 * 1024);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(hook.calls == 1 && hook.executing_thread_valid);
    if (worker_running)
        assert(!pthread_equal(hook.executing_thread, caller));
    else
        assert(pthread_equal(hook.executing_thread, caller));

    myfs_storage_t active;
    assert(resolve_storage(path, &active) == 0);
    assert(!storage_generation_equal(&active, &old_storage));
    assert_handle_bundle(writer_handle, &active, 32 * 1024);
    assert(storage_generation_equal(&reader_handle->storage, &old_storage));
    assert(reader_handle->cached_inode.window_size ==
           MYFS_DEFAULT_WINDOW_SIZE);
    char contents[13] = {0};
    assert(myfs_read(path, contents, 12, 0, &writer) == 12);
    assert(memcmp(contents, "Xnitial-data", 12) == 0);
    memset(contents, 0, sizeof(contents));
    assert(myfs_read(path, contents, 12, 0, &reader) == 12);
    assert(memcmp(contents, "Xnitial-data", 12) == 0);

    wait_for_path_quiescence(path);
    struct myfs_generation_registry_test_snapshot old_snapshot =
        registry_snapshot(&old_storage);
    struct myfs_generation_registry_test_snapshot new_snapshot =
        registry_snapshot(&active);
    struct execution_context_outcome outcome = {
        .writer_window = writer_handle->cached_inode.window_size,
        .reader_window = reader_handle->cached_inode.window_size,
        .old_open_refs = old_snapshot.open_refs,
        .old_writer_refs = old_snapshot.writer_refs,
        .new_open_refs = new_snapshot.open_refs,
        .new_writer_refs = new_snapshot.writer_refs,
        .old_gc_state = old_snapshot.gc_state,
        .old_adaptive_state = old_snapshot.adaptive_state,
        .new_adaptive_state = new_snapshot.adaptive_state,
        .old_full_windows = old_snapshot.adaptive_full_windows,
        .old_partial_rmw = old_snapshot.adaptive_partial_rmw,
        .new_full_windows = new_snapshot.adaptive_full_windows,
        .new_partial_rmw = new_snapshot.adaptive_partial_rmw,
        .old_superseded = old_snapshot.superseded,
    };

    assert(myfs_release(path, &reader) == 0);
    outcome.old_retired_on_reader_release =
        myfs_generation_registry_test_snapshot(&old_storage, NULL) == -ENOENT;
    assert(outcome.old_retired_on_reader_release);
    assert(myfs_release(path, &writer) == 0);
    stop_compaction_worker();
    destroy_handoff_hook(&hook);
    myfs_conf = NULL;
    return outcome;
}

static void test_worker_and_fallback_have_identical_outcomes(
    const char *worker_dir, const char *fallback_dir)
{
    struct execution_context_outcome worker = run_execution_context_case(
        worker_dir, "/worker-context", true);
    struct execution_context_outcome fallback = run_execution_context_case(
        fallback_dir, "/fallback-context", false);

    assert(worker.writer_window == fallback.writer_window);
    assert(worker.reader_window == fallback.reader_window);
    assert(worker.old_open_refs == fallback.old_open_refs);
    assert(worker.old_writer_refs == fallback.old_writer_refs);
    assert(worker.new_open_refs == fallback.new_open_refs);
    assert(worker.new_writer_refs == fallback.new_writer_refs);
    assert(worker.old_gc_state == fallback.old_gc_state);
    assert(worker.old_adaptive_state == fallback.old_adaptive_state);
    assert(worker.new_adaptive_state == fallback.new_adaptive_state);
    assert(worker.old_full_windows == fallback.old_full_windows);
    assert(worker.old_partial_rmw == fallback.old_partial_rmw);
    assert(worker.new_full_windows == fallback.new_full_windows);
    assert(worker.new_partial_rmw == fallback.new_partial_rmw);
    assert(worker.old_superseded == fallback.old_superseded);
    assert(worker.old_retired_on_reader_release ==
           fallback.old_retired_on_reader_release);
    assert(worker.writer_window == 32 * 1024);
    assert(worker.reader_window == MYFS_DEFAULT_WINDOW_SIZE);
    assert(worker.old_open_refs == 1 && worker.old_writer_refs == 0);
    assert(worker.new_open_refs == 1 && worker.new_writer_refs == 1);
    assert(worker.old_gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(worker.old_adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(worker.new_adaptive_state == MYFS_GENERATION_ADAPTIVE_TEST_IDLE);
    assert(worker.old_full_windows == 0 && worker.old_partial_rmw == 0);
    assert(worker.new_full_windows == 0 && worker.new_partial_rmw == 0);
    assert(worker.old_superseded);
    assert(worker.old_retired_on_reader_release);
}

struct release_gc_gate
{
    myfs_storage_t victim;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool entered;
    bool release;
};

static int release_gc_hook(enum myfs_generation_gc_test_stage stage,
                           const myfs_storage_t *storage, void *argument)
{
    struct release_gc_gate *gate = argument;
    if (stage != MYFS_GENERATION_GC_TEST_BEFORE_RESOLVE ||
        !storage_generation_equal(storage, &gate->victim))
        return 0;
    assert(pthread_mutex_lock(&gate->mu) == 0);
    gate->entered = true;
    assert(pthread_cond_broadcast(&gate->cv) == 0);
    while (!gate->release)
        assert(pthread_cond_wait(&gate->cv, &gate->mu) == 0);
    assert(pthread_mutex_unlock(&gate->mu) == 0);
    return 0;
}

struct release_thread_context
{
    const char *path;
    struct fuse_file_info *fi;
    atomic_bool started;
    atomic_bool finished;
    int result;
};

static void *release_thread_main(void *argument)
{
    struct release_thread_context *context = argument;
    atomic_store(&context->started, true);
    context->result = myfs_release(context->path, context->fi);
    atomic_store(&context->finished, true);
    return NULL;
}

static void wait_for_release_gc_gate(struct release_gc_gate *gate)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    assert(pthread_mutex_lock(&gate->mu) == 0);
    while (!gate->entered)
    {
        int status = pthread_cond_timedwait(&gate->cv, &gate->mu, &deadline);
        assert(status == 0);
    }
    assert(pthread_mutex_unlock(&gate->mu) == 0);
}

static void test_release_runs_generation_gc_synchronously(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    stop_compaction_worker();

    const char *path = "/release-synchronous-gc";
    struct fuse_file_info writer = {.flags = O_RDWR};
    assert(myfs_create(path, 0600, &writer) == 0);
    assert(myfs_write(path, "initial-data", 12, 0, &writer) == 12);
    struct fuse_file_info reader = {.flags = O_RDONLY};
    assert(myfs_open(path, &reader) == 0);
    myfs_file_handle_t *writer_handle =
        (myfs_file_handle_t *)(uintptr_t)writer.fh;
    myfs_storage_t old_storage = writer_handle->storage;
    write_partial_samples(path, &writer);
    assert(wait_for_handle_window(writer_handle, 32 * 1024) == 32 * 1024);
    struct myfs_generation_registry_test_snapshot pending =
        registry_snapshot(&old_storage);
    assert(pending.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(pending.open_refs == 1 && pending.writer_refs == 0);

    struct release_gc_gate gate = {
        .victim = old_storage,
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    myfs_generation_registry_test_set_gc_hook(release_gc_hook, &gate);
    struct release_thread_context release_context = {
        .path = path,
        .fi = &reader,
        .result = -1,
    };
    pthread_t release_thread;
    assert(pthread_create(&release_thread, NULL, release_thread_main,
                          &release_context) == 0);
    wait_for_release_gc_gate(&gate);
    assert(atomic_load(&release_context.started));
    assert(!atomic_load(&release_context.finished));
    struct myfs_generation_registry_test_snapshot claimed =
        registry_snapshot(&old_storage);
    assert(claimed.gc_state == MYFS_GENERATION_GC_TEST_CLAIMED);
    assert(claimed.gc_claim_id > pending.gc_claim_id);

    assert(pthread_mutex_lock(&gate.mu) == 0);
    gate.release = true;
    assert(pthread_cond_broadcast(&gate.cv) == 0);
    assert(pthread_mutex_unlock(&gate.mu) == 0);
    assert(pthread_join(release_thread, NULL) == 0);
    myfs_generation_registry_test_set_gc_hook(NULL, NULL);
    assert(release_context.result == 0);
    assert(atomic_load(&release_context.finished));
    assert(reader.fh == 0);
    assert(myfs_generation_registry_test_snapshot(&old_storage, NULL) ==
           -ENOENT);

    assert(myfs_release(path, &writer) == 0);
    assert(pthread_cond_destroy(&gate.cv) == 0);
    assert(pthread_mutex_destroy(&gate.mu) == 0);
    myfs_conf = NULL;
}

static void test_live_writer_is_rebound_after_periodic_resize(const char *dir)
{
    struct myfs_config conf = {0};
    assert(snprintf(conf.root, sizeof(conf.root), "%s", dir) <
           (int)sizeof(conf.root));
    myfs_conf = &conf;
    assert(start_compaction_worker() == 0);

    struct fuse_file_info fi = {0};
    fi.flags = O_RDWR;
    assert(myfs_create("/adaptive", 0600, &fi) == 0);
    enum { PAYLOAD_SIZE = 96 * 1024 };
    unsigned char *expected = malloc(PAYLOAD_SIZE);
    assert(expected != NULL);
    for (size_t i = 0; i < PAYLOAD_SIZE; i++)
        expected[i] = (unsigned char)((i * 37 + 11) % 251);
    assert(myfs_write("/adaptive", (const char *)expected, PAYLOAD_SIZE, 0,
                      &fi) == PAYLOAD_SIZE);
    struct fuse_file_info old_reader = {.flags = O_RDONLY};
    assert(myfs_open("/adaptive", &old_reader) == 0);
    struct fuse_file_info second_writer = {.flags = O_RDWR};
    assert(myfs_open("/adaptive", &second_writer) == 0);
    myfs_file_handle_t *initial_handle =
        (myfs_file_handle_t *)(uintptr_t)fi.fh;
    myfs_file_handle_t *initial_second_handle =
        (myfs_file_handle_t *)(uintptr_t)second_writer.fh;
    myfs_storage_t old_storage = initial_handle->storage;
    expected[0] = 'X';
    /* The initial 96 KiB write contributes one full-window sample. */
    for (int i = 0; i < 126; i++)
        assert(myfs_write("/adaptive", "X", 1, 0, &fi) == 1);

    assert(setenv("MYFS_TEST_READ_HOLD_MS", "100", 1) == 0);
    struct reader_context readers = {
        .path = "/adaptive",
        .fi = &fi,
        .expected = expected,
        .size = PAYLOAD_SIZE,
    };
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++)
        assert(pthread_create(&threads[i], NULL, continuous_reader,
                              &readers) == 0);
    struct timespec overlap = {.tv_nsec = 50 * 1000 * 1000};
    nanosleep(&overlap, NULL);

    struct timespec started;
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    struct handoff_test_hook hook;
    initialize_handoff_hook(
        &hook, MYFS_GENERATION_HANDOFF_TEST_AFTER_REGISTRY_TRANSACTION, 0);
    hook.pause = true;
    hook.verify_first_relink_shard = true;
    myfs_generation_registry_test_set_handoff_hook(handoff_test_hook_main,
                                                    &hook);
    assert(myfs_write("/adaptive", "X", 1, 0, &fi) == 1);
    wait_for_handoff_hook(&hook);

    /* The hook runs after the single-shard transaction.  Any registry reader
     * arriving here sees all writable handles moved together, never an
     * intermediate refcount/list state. */
    myfs_storage_t gated_active;
    assert(resolve_storage("/adaptive", &gated_active) == 0);
    struct myfs_generation_registry_test_snapshot gated_old =
        registry_snapshot(&old_storage);
    struct myfs_generation_registry_test_snapshot gated_new =
        registry_snapshot(&gated_active);
    assert(gated_old.open_refs == 1 && gated_old.writer_refs == 0);
    assert(gated_new.open_refs == 2 && gated_new.writer_refs == 2);
    assert(hook.first_relink_checks == 1);
    /* Registry relinking is complete, but both complete cache/FD bundles are
     * still protected by their cache write locks. */
    assert(pthread_rwlock_tryrdlock(&initial_handle->cache_lock) == EBUSY);
    assert(pthread_rwlock_tryrdlock(&initial_second_handle->cache_lock) ==
           EBUSY);

    unsigned char *blocked_output = malloc(PAYLOAD_SIZE);
    assert(blocked_output != NULL);
    struct blocked_read_context blocked = {
        .path = "/adaptive",
        .fi = &fi,
        .expected = expected,
        .size = PAYLOAD_SIZE,
        .out = blocked_output,
        .result = -1,
    };
    pthread_t blocked_thread;
    assert(pthread_create(&blocked_thread, NULL, blocked_reader, &blocked) ==
           0);
    for (int attempt = 0; attempt < 100 && !atomic_load(&blocked.started);
         attempt++)
    {
        struct timespec delay = {.tv_nsec = 1000 * 1000};
        nanosleep(&delay, NULL);
    }
    assert(atomic_load(&blocked.started));
    release_handoff_hook(&hook);
    assert(pthread_join(blocked_thread, NULL) == 0);
    assert(blocked.result == PAYLOAD_SIZE);
    assert(memcmp(blocked.out, blocked.expected, PAYLOAD_SIZE) == 0);
    free(blocked_output);

    myfs_file_handle_t *handle = (myfs_file_handle_t *)(uintptr_t)fi.fh;
    uint32_t window_size = 0;
    struct timespec completed = started;
    for (int attempt = 0; attempt < 200; attempt++)
    {
        assert(pthread_rwlock_rdlock(&handle->cache_lock) == 0);
        window_size = handle->cached_inode.window_size;
        pthread_rwlock_unlock(&handle->cache_lock);
        if (window_size == 32 * 1024)
        {
            assert(clock_gettime(CLOCK_MONOTONIC, &completed) == 0);
            break;
        }
        struct timespec delay = {.tv_nsec = 10 * 1000 * 1000};
        nanosleep(&delay, NULL);
    }
    assert(window_size == 32 * 1024);
    myfs_generation_registry_test_set_handoff_hook(NULL, NULL);
    assert(hook.calls == 1);
    destroy_handoff_hook(&hook);
    myfs_file_handle_t *second_handle =
        (myfs_file_handle_t *)(uintptr_t)second_writer.fh;
    assert(pthread_rwlock_rdlock(&second_handle->cache_lock) == 0);
    assert(second_handle->cached_inode.window_size == 32 * 1024);
    pthread_rwlock_unlock(&second_handle->cache_lock);
    assert(elapsed_seconds(started, completed) <= 2.0);
    atomic_store(&readers.stop, true);
    for (size_t i = 0; i < 8; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&readers.failures) == 0);
    unsetenv("MYFS_TEST_READ_HOLD_MS");

    unsigned char *out = malloc(PAYLOAD_SIZE);
    assert(out != NULL);
    assert(myfs_read("/adaptive", (char *)out, PAYLOAD_SIZE, 0, &fi) ==
           PAYLOAD_SIZE);
    assert(memcmp(out, expected, PAYLOAD_SIZE) == 0);
    memset(out, 0, PAYLOAD_SIZE);
    assert(myfs_read("/adaptive", (char *)out, PAYLOAD_SIZE, 0,
                     &old_reader) == PAYLOAD_SIZE);
    assert(memcmp(out, expected, PAYLOAD_SIZE) == 0);
    wait_for_path_quiescence("/adaptive");
    myfs_file_handle_t *old_handle =
        (myfs_file_handle_t *)(uintptr_t)old_reader.fh;
    assert(old_handle->cached_inode.window_size == 64 * 1024);
    assert(old_handle->cached_inode.chunk_map.num_chunks == 2);
    assert(handle->cached_inode.chunk_map.num_chunks == 3);
    assert(handle->generation_record == second_handle->generation_record);
    assert(handle->generation_record != old_handle->generation_record);
    assert(storage_generation_equal(&old_handle->storage, &old_storage));
    myfs_storage_t new_storage = handle->storage;
    struct myfs_generation_registry_test_snapshot old_snapshot =
        registry_snapshot(&old_storage);
    struct myfs_generation_registry_test_snapshot new_snapshot =
        registry_snapshot(&new_storage);
    assert(old_snapshot.open_refs == 1 && old_snapshot.writer_refs == 0);
    assert(old_snapshot.superseded);
    assert(old_snapshot.gc_state == MYFS_GENERATION_GC_TEST_PENDING);
    assert(new_snapshot.open_refs == 2 && new_snapshot.writer_refs == 2);
    unsigned char old_second_byte = expected[1];
    assert(myfs_write("/adaptive", "Y", 1, 1, &second_writer) == 1);
    expected[1] = 'Y';
    memset(out, 0, PAYLOAD_SIZE);
    assert(myfs_read("/adaptive", (char *)out, PAYLOAD_SIZE, 0, &fi) ==
           PAYLOAD_SIZE);
    assert(memcmp(out, expected, PAYLOAD_SIZE) == 0);
    memset(out, 0, PAYLOAD_SIZE);
    assert(myfs_read("/adaptive", (char *)out, PAYLOAD_SIZE, 0,
                     &old_reader) == PAYLOAD_SIZE);
    expected[1] = old_second_byte;
    assert(memcmp(out, expected, PAYLOAD_SIZE) == 0);
    expected[1] = 'Y';
    free(out);

    char *full_window = malloc(32 * 1024);
    assert(full_window != NULL);
    memset(full_window, 'F', 32 * 1024);
    for (int i = 0; i < 128; i++)
        assert(myfs_write("/adaptive", full_window, 32 * 1024, 0, &fi)
               == 32 * 1024);
    free(full_window);
    struct timespec cooldown_check = {.tv_nsec = 200 * 1000 * 1000};
    nanosleep(&cooldown_check, NULL);
    assert(pthread_rwlock_rdlock(&handle->cache_lock) == 0);
    window_size = handle->cached_inode.window_size;
    pthread_rwlock_unlock(&handle->cache_lock);
    assert(window_size == 32 * 1024);

    /* The read-only handle pins the old generation until release.  Its
     * release performs claim/I/O/finalize synchronously, so absence is
     * observable immediately when myfs_release returns. */
    assert(myfs_release("/adaptive", &old_reader) == 0);
    assert(myfs_generation_registry_test_snapshot(&old_storage, NULL) ==
           -ENOENT);
    assert(myfs_release("/adaptive", &fi) == 0);
    assert(myfs_release("/adaptive", &second_writer) == 0);
    stop_compaction_worker();
    free(expected);
    myfs_conf = NULL;
}

int main(void)
{
    char template[] = "/tmp/myfs-file-ops-test-XXXXXX";
    char *dir = mkdtemp(template);
    assert(dir != NULL);
    test_open_handle_reads_from_its_cache(dir);
    test_resize_policy_is_ratio_cost_and_cooldown_gated();
    char checkpoint_template[] = "/tmp/myfs-checkpoint-test-XXXXXX";
    char *checkpoint_dir = mkdtemp(checkpoint_template);
    assert(checkpoint_dir != NULL);
    test_metadata_journal_is_periodically_checkpointed(checkpoint_dir);
    char small_window_template[] = "/tmp/myfs-small-window-test-XXXXXX";
    char *small_window_dir = mkdtemp(small_window_template);
    assert(small_window_dir != NULL);
    test_file_uses_its_persisted_window_size(small_window_dir);
    char legacy_partial_template[] = "/tmp/myfs-legacy-partial-XXXXXX";
    char *legacy_partial_dir = mkdtemp(legacy_partial_template);
    assert(legacy_partial_dir != NULL);
    test_partial_legacy_repack_keeps_tolerant_reads(legacy_partial_dir);
    char readonly_release_template[] =
        "/tmp/myfs-readonly-release-test-XXXXXX";
    char *readonly_release_dir = mkdtemp(readonly_release_template);
    assert(readonly_release_dir != NULL);
    test_readonly_release_does_not_claim_writer_sample(readonly_release_dir);
    char coherence_template[] = "/tmp/myfs-coherence-test-XXXXXX";
    char *coherence_dir = mkdtemp(coherence_template);
    assert(coherence_dir != NULL);
    test_getattr_epoch_and_truncate_preserve_reader_bundle(coherence_dir);
    char adaptive_template[] = "/tmp/myfs-adaptive-test-XXXXXX";
    char *adaptive_dir = mkdtemp(adaptive_template);
    assert(adaptive_dir != NULL);
    test_live_writer_is_rebound_after_periodic_resize(adaptive_dir);
    char handoff_failure_template[] = "/tmp/myfs-handoff-failure-XXXXXX";
    char *handoff_failure_dir = mkdtemp(handoff_failure_template);
    assert(handoff_failure_dir != NULL);
    test_handoff_publication_failure_outcomes(handoff_failure_dir);
    char worker_context_template[] = "/tmp/myfs-worker-context-XXXXXX";
    char *worker_context_dir = mkdtemp(worker_context_template);
    assert(worker_context_dir != NULL);
    char fallback_context_template[] = "/tmp/myfs-fallback-context-XXXXXX";
    char *fallback_context_dir = mkdtemp(fallback_context_template);
    assert(fallback_context_dir != NULL);
    test_worker_and_fallback_have_identical_outcomes(worker_context_dir,
                                                     fallback_context_dir);
    char release_gc_template[] = "/tmp/myfs-release-gc-XXXXXX";
    char *release_gc_dir = mkdtemp(release_gc_template);
    assert(release_gc_dir != NULL);
    test_release_runs_generation_gc_synchronously(release_gc_dir);
    destroy_generation_registry();
    destroy_lock_table();
    return 0;
}
