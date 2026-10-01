#include "myfs.h"

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
    struct fuse_file_info *fi;
    atomic_bool stop;
    atomic_int failures;
};

static void *continuous_reader(void *arg)
{
    struct reader_context *ctx = arg;
    while (!atomic_load(&ctx->stop))
    {
        char out[12];
        int ret = myfs_read("/adaptive", out, sizeof(out), 0, ctx->fi);
        if (ret != 12 || out[0] != 'X')
            atomic_fetch_add(&ctx->failures, 1);
    }
    return NULL;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
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
    assert(myfs_write("/adaptive", "initial-data", 12, 0, &fi) == 12);
    struct fuse_file_info old_reader = {.flags = O_RDONLY};
    assert(myfs_open("/adaptive", &old_reader) == 0);
    struct fuse_file_info second_writer = {.flags = O_RDWR};
    assert(myfs_open("/adaptive", &second_writer) == 0);
    for (int i = 0; i < 127; i++)
        assert(myfs_write("/adaptive", "X", 1, 0, &fi) == 1);

    assert(setenv("MYFS_TEST_READ_HOLD_MS", "100", 1) == 0);
    struct reader_context readers = {.fi = &fi};
    pthread_t threads[8];
    for (size_t i = 0; i < 8; i++)
        assert(pthread_create(&threads[i], NULL, continuous_reader,
                              &readers) == 0);
    struct timespec overlap = {.tv_nsec = 50 * 1000 * 1000};
    nanosleep(&overlap, NULL);

    struct timespec started;
    assert(clock_gettime(CLOCK_MONOTONIC, &started) == 0);
    assert(myfs_write("/adaptive", "X", 1, 0, &fi) == 1);

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

    char out[13] = {0};
    assert(myfs_read("/adaptive", out, 12, 0, &fi) == 12);
    assert(memcmp(out, "Xnitial-data", 12) == 0);
    memset(out, 0, sizeof(out));
    assert(myfs_read("/adaptive", out, 12, 0, &old_reader) == 12);
    assert(memcmp(out, "Xnitial-data", 12) == 0);
    myfs_file_handle_t *old_handle =
        (myfs_file_handle_t *)(uintptr_t)old_reader.fh;
    assert(old_handle->cached_inode.window_size == 64 * 1024);
    assert(myfs_write("/adaptive", "Y", 1, 1, &second_writer) == 1);
    memset(out, 0, sizeof(out));
    assert(myfs_read("/adaptive", out, 12, 0, &fi) == 12);
    assert(memcmp(out, "XYitial-data", 12) == 0);

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

    assert(myfs_release("/adaptive", &fi) == 0);
    assert(myfs_release("/adaptive", &second_writer) == 0);
    assert(myfs_release("/adaptive", &old_reader) == 0);
    stop_compaction_worker();
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
    destroy_generation_registry();
    destroy_lock_table();
    return 0;
}
