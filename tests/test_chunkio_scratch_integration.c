#include "myfs.h"
#include "chunkio_scratch.h"

#include <assert.h>

static int temporary_file(void)
{
    char path[] = "/tmp/myfs-chunkio-scratch-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(unlink(path) == 0);
    return fd;
}

static void write_exact(int fd, const void *buffer, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t written = pwrite(fd, (const char *)buffer + done,
                                 size - done, offset + (off_t)done);
        assert(written > 0);
        done += (size_t)written;
    }
}

static void read_exact(int fd, void *buffer, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t got = pread(fd, (char *)buffer + done, size - done,
                            offset + (off_t)done);
        assert(got > 0);
        done += (size_t)got;
    }
}

static myfs_chunkio_scratch_snapshot_t snapshot(
    myfs_chunkio_scratch_role_t role)
{
    myfs_chunkio_scratch_snapshot_t result = {0};
    assert(myfs_chunkio_scratch_test_snapshot(role, &result) == 0);
    return result;
}

static void assert_all_roles_released(void)
{
    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        assert(!snapshot((myfs_chunkio_scratch_role_t)role).in_use);
}

static void test_payload_load_round_trips_raw_and_compressed(void)
{
    int fd = temporary_file();
    unsigned char raw[4096];
    unsigned char decoded[4096];
    for (size_t i = 0; i < sizeof(raw); i++)
        raw[i] = (unsigned char)(i * 37U + 11U);
    write_exact(fd, raw, sizeof(raw), 0);

    myfs_chunk_t raw_chunk = {
        .physical_offset = 0,
        .raw_size = sizeof(raw),
        .stored_size = sizeof(raw),
        .checksum = chunk_crc32(raw, sizeof(raw)),
    };
    assert(myfs_chunk_payload_load(fd, &raw_chunk, (char *)decoded) == 0);
    assert(memcmp(decoded, raw, sizeof(raw)) == 0);

    unsigned char plain[64U * 1024U];
    memset(plain, 'C', sizeof(plain));
    size_t bound = ZSTD_compressBound(sizeof(plain));
    unsigned char *frame = malloc(bound);
    assert(frame != NULL);
    size_t frame_size = ZSTD_compress(frame, bound, plain, sizeof(plain),
                                      ZSTD_CLEVEL_DEFAULT);
    assert(!ZSTD_isError(frame_size));
    off_t frame_offset = sizeof(raw);
    write_exact(fd, frame, frame_size, frame_offset);

    myfs_chunk_t compressed_chunk = {
        .physical_offset = (uint64_t)frame_offset,
        .raw_size = (uint32_t)frame_size,
        .stored_size = sizeof(plain),
        .codec_type = 1,
        .flags = 1,
        .checksum = chunk_crc32(frame, frame_size),
    };
    unsigned char *expanded = malloc(sizeof(plain));
    assert(expanded != NULL);
    assert(myfs_chunk_payload_load(fd, &compressed_chunk,
                                   (char *)expanded) == 0);
    assert(memcmp(expanded, plain, sizeof(plain)) == 0);

    myfs_chunkio_scratch_snapshot_t raw_state =
        snapshot(MYFS_CHUNKIO_SCRATCH_RAW);
    assert(raw_state.retained_capacity >= sizeof(raw));
    assert(!raw_state.in_use);
    free(expanded);
    free(frame);
    assert(close(fd) == 0);
}

static void test_payload_load_errors_release_raw_role(void)
{
    int fd = temporary_file();
    const char payload[] = "payload";
    char output[32] = {0};
    write_exact(fd, payload, sizeof(payload), 0);

    myfs_chunk_t chunk = {
        .physical_offset = 0,
        .raw_size = sizeof(payload),
        .stored_size = sizeof(payload),
        .checksum = chunk_crc32(payload, sizeof(payload)) ^ 1U,
    };
    assert(myfs_chunk_payload_load(fd, &chunk, output) == -EIO);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_RAW).in_use);

    chunk.checksum = 0;
    chunk.stored_size = chunk.raw_size + 1;
    assert(myfs_chunk_payload_load(fd, &chunk, output) == -EIO);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_RAW).in_use);

    chunk.stored_size = chunk.raw_size;
    chunk.codec_type = 99;
    assert(myfs_chunk_payload_load(fd, &chunk, output) == -EIO);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_RAW).in_use);

    chunk.codec_type = 1;
    assert(myfs_chunk_payload_load(fd, &chunk, output) == -EIO);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_RAW).in_use);

    chunk.codec_type = 0;
    assert(myfs_chunk_payload_load(-1, &chunk, output) == -EBADF);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_RAW).in_use);

    chunk.physical_offset = 1024;
    assert(myfs_chunk_payload_load(fd, &chunk, output) == -EIO);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_RAW).in_use);
    assert(close(fd) == 0);
}

static void test_blob_append_preserves_format_and_failure_atomicity(void)
{
    int fd = temporary_file();
    const unsigned char prefix[] = {0x31, 0x32, 0x33};
    write_exact(fd, prefix, sizeof(prefix), 0);
    off_t eof = sizeof(prefix);

    unsigned char compressed_payload[64U * 1024U];
    memset(compressed_payload, 'A', sizeof(compressed_payload));
    myfs_chunk_t compressed = {0};
    assert(myfs_blob_append(fd, &eof, (const char *)compressed_payload,
                            sizeof(compressed_payload), 65536,
                            &compressed) == 0);
    assert(compressed.logical_offset == 65536);
    assert(compressed.physical_offset == sizeof(prefix));
    assert(compressed.stored_size == sizeof(compressed_payload));
    assert(compressed.codec_type == 1);
    assert(eof == (off_t)(sizeof(prefix) + compressed.raw_size));
    unsigned char *blob = malloc(compressed.raw_size);
    assert(blob != NULL);
    read_exact(fd, blob, compressed.raw_size,
               (off_t)compressed.physical_offset);
    assert(compressed.checksum == chunk_crc32(blob, compressed.raw_size));
    free(blob);

    unsigned char raw_payload[4096];
    memset(raw_payload, 0x7b, sizeof(raw_payload));
    memcpy(raw_payload, "\x89PNG", 4);
    off_t raw_offset = eof;
    myfs_chunk_t raw = {0};
    assert(myfs_blob_append(fd, &eof, (const char *)raw_payload,
                            sizeof(raw_payload), 131072, &raw) == 0);
    assert(raw.codec_type == 0);
    assert(raw.raw_size == sizeof(raw_payload));
    assert(raw.stored_size == sizeof(raw_payload));
    assert(raw.physical_offset == (uint64_t)raw_offset);
    unsigned char raw_readback[sizeof(raw_payload)];
    read_exact(fd, raw_readback, sizeof(raw_readback), raw_offset);
    assert(memcmp(raw_readback, raw_payload, sizeof(raw_payload)) == 0);

    char proc_path[64];
    assert(snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd) > 0);
    int read_only_fd = open(proc_path, O_RDONLY | O_CLOEXEC);
    assert(read_only_fd >= 0);
    off_t failed_eof = eof;
    myfs_chunk_t sentinel;
    memset(&sentinel, 0x5a, sizeof(sentinel));
    myfs_chunk_t unchanged = sentinel;
    assert(myfs_blob_append(read_only_fd, &failed_eof,
                            (const char *)compressed_payload,
                            sizeof(compressed_payload), 0, &sentinel) < 0);
    assert(failed_eof == eof);
    assert(memcmp(&sentinel, &unchanged, sizeof(sentinel)) == 0);
    assert(!snapshot(MYFS_CHUNKIO_SCRATCH_COMP).in_use);

    assert(snapshot(MYFS_CHUNKIO_SCRATCH_COMP).retained_capacity >=
           ZSTD_compressBound(sizeof(compressed_payload)));
    assert(close(read_only_fd) == 0);
    assert(close(fd) == 0);
}

static void make_crossing_raw_source(int fd, unsigned char *payload,
                                     size_t payload_size,
                                     myfs_chunk_t *chunk,
                                     myfs_chunk_map_t *map)
{
    for (size_t i = 0; i < payload_size; i++)
        payload[i] = (unsigned char)('a' + i % 23);
    write_exact(fd, payload, payload_size, 0);
    *chunk = (myfs_chunk_t){
        .logical_offset = 0,
        .physical_offset = 0,
        .raw_size = (uint32_t)payload_size,
        .stored_size = (uint32_t)payload_size,
        .checksum = chunk_crc32(payload, payload_size),
    };
    *map = (myfs_chunk_map_t){
        .num_chunks = 1,
        .logical_size = payload_size,
        .chunks = chunk,
    };
}

static void test_partial_repack_uses_all_four_roles_and_preserves_data(void)
{
    const size_t payload_size = 80U * 1024U;
    unsigned char *payload = malloc(payload_size);
    unsigned char *expected = malloc(payload_size);
    unsigned char *actual = calloc(1, payload_size);
    assert(payload != NULL && expected != NULL && actual != NULL);
    int src_fd = temporary_file();
    int dst_fd = temporary_file();
    myfs_chunk_t source_chunk;
    myfs_chunk_map_t source_map;
    make_crossing_raw_source(src_fd, payload, payload_size,
                             &source_chunk, &source_map);
    memcpy(expected, payload, payload_size);
    expected[123] = 'Z';

    off_t eof = 0;
    myfs_chunk_t *entries = NULL;
    uint32_t count = 0;
    assert(myfs_repack_windows(src_fd, dst_fd, &eof, &source_map,
                               64U * 1024U, 0, 1, 0, payload_size,
                               "Z", 123, 1, &entries, &count) == 0);
    assert(count == 2);
    assert(entries[0].logical_offset == 0);
    assert(entries[1].logical_offset == 64U * 1024U);
    for (uint32_t i = 0; i < count; i++)
    {
        assert(myfs_chunk_payload_load(dst_fd, &entries[i],
                                       (char *)actual +
                                           entries[i].logical_offset) == 0);
    }
    assert(memcmp(actual, expected, payload_size) == 0);

    assert(snapshot(MYFS_CHUNKIO_SCRATCH_RAW).retained_capacity > 0);
    assert(snapshot(MYFS_CHUNKIO_SCRATCH_COMP).retained_capacity > 0);
    assert(snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW).retained_capacity >=
           64U * 1024U);
    assert(snapshot(MYFS_CHUNKIO_SCRATCH_CACHED).retained_capacity >=
           payload_size);
    assert_all_roles_released();

    free(entries);
    assert(close(dst_fd) == 0);
    assert(close(src_fd) == 0);
    free(actual);
    free(expected);
    free(payload);
}

typedef struct
{
    int result;
    size_t raw_capacity;
    size_t comp_capacity;
    size_t window_capacity;
    size_t cached_capacity;
} fast_path_result_t;

static void *full_window_fast_path_worker(void *argument)
{
    fast_path_result_t *result = argument;
    int fd = temporary_file();
    unsigned char *patch = malloc(MYFS_DEFAULT_WINDOW_SIZE);
    assert(patch != NULL);
    memset(patch, 'F', MYFS_DEFAULT_WINDOW_SIZE);
    myfs_chunk_map_t empty_map = {0};
    myfs_chunk_t *entries = NULL;
    uint32_t count = 0;
    off_t eof = 0;

    result->result = myfs_repack_windows(
        -1, fd, &eof, &empty_map, MYFS_DEFAULT_WINDOW_SIZE,
        0, 0, 0, MYFS_DEFAULT_WINDOW_SIZE,
        (const char *)patch, 0, MYFS_DEFAULT_WINDOW_SIZE,
        &entries, &count);
    result->raw_capacity = snapshot(MYFS_CHUNKIO_SCRATCH_RAW).retained_capacity;
    result->comp_capacity = snapshot(MYFS_CHUNKIO_SCRATCH_COMP).retained_capacity;
    result->window_capacity =
        snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW).retained_capacity;
    result->cached_capacity =
        snapshot(MYFS_CHUNKIO_SCRATCH_CACHED).retained_capacity;
    if (result->result == 0 && count != 1)
        result->result = -EIO;

    free(entries);
    free(patch);
    close(fd);
    return NULL;
}

static void test_full_window_fast_path_does_not_grow_window_role(void)
{
    pthread_t thread;
    fast_path_result_t result = {0};
    assert(pthread_create(&thread, NULL, full_window_fast_path_worker,
                          &result) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(result.result == 0);
    assert(result.raw_capacity == 0);
    assert(result.comp_capacity > 0);
    assert(result.window_capacity == 0);
    assert(result.cached_capacity == 0);
}

static void test_repack_error_releases_window_cached_and_raw_roles(void)
{
    const size_t payload_size = 80U * 1024U;
    unsigned char *payload = malloc(payload_size);
    assert(payload != NULL);
    int src_fd = temporary_file();
    int dst_fd = temporary_file();
    myfs_chunk_t source_chunk;
    myfs_chunk_map_t source_map;
    make_crossing_raw_source(src_fd, payload, payload_size,
                             &source_chunk, &source_map);

    myfs_chunk_t *entries = (myfs_chunk_t *)(uintptr_t)1;
    uint32_t count = UINT32_MAX;
    off_t eof = 0;
    assert(myfs_repack_windows(-1, dst_fd, &eof, &source_map,
                               MYFS_DEFAULT_WINDOW_SIZE, 0, 1,
                               0, payload_size, NULL, 0, 0,
                               &entries, &count) == -EBADF);
    assert(entries == NULL);
    assert(count == 0);
    assert_all_roles_released();

    assert(close(dst_fd) == 0);
    assert(close(src_fd) == 0);
    free(payload);
}

typedef struct
{
    int result;
    size_t raw_capacity;
    size_t cached_capacity;
    size_t raw_temporaries;
    size_t cached_temporaries;
} oversize_result_t;

static void *oversize_legacy_worker(void *argument)
{
    oversize_result_t *result = argument;
    const size_t payload_size = (size_t)MYFS_MAX_WINDOW_SIZE + 1;
    unsigned char *payload = malloc(payload_size);
    assert(payload != NULL);
    int src_fd = temporary_file();
    int dst_fd = temporary_file();
    myfs_chunk_t source_chunk;
    myfs_chunk_map_t source_map;
    make_crossing_raw_source(src_fd, payload, payload_size,
                             &source_chunk, &source_map);
    off_t eof = 0;
    myfs_chunk_t *entries = NULL;
    uint32_t count = 0;
    result->result = myfs_repack_windows(
        src_fd, dst_fd, &eof, &source_map, MYFS_MAX_WINDOW_SIZE,
        0, 1, 0, (off_t)payload_size, NULL, 0, 0, &entries, &count);
    myfs_chunkio_scratch_snapshot_t raw =
        snapshot(MYFS_CHUNKIO_SCRATCH_RAW);
    myfs_chunkio_scratch_snapshot_t cached =
        snapshot(MYFS_CHUNKIO_SCRATCH_CACHED);
    result->raw_capacity = raw.retained_capacity;
    result->cached_capacity = cached.retained_capacity;
    result->raw_temporaries = raw.temporary_acquisition_count;
    result->cached_temporaries = cached.temporary_acquisition_count;
    if (result->result == 0 && count != 2)
        result->result = -EIO;

    free(entries);
    close(dst_fd);
    close(src_fd);
    free(payload);
    return NULL;
}

static void test_oversize_legacy_payload_uses_temporary_without_retention(void)
{
    pthread_t thread;
    oversize_result_t result = {0};
    assert(pthread_create(&thread, NULL, oversize_legacy_worker, &result) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(result.result == 0);
    assert(result.raw_capacity == 0);
    assert(result.cached_capacity == 0);
    assert(result.raw_temporaries == 1);
    assert(result.cached_temporaries == 1);
}

static void test_foreground_then_fallback_repack_has_no_further_growth(void)
{
    const size_t payload_size = 80U * 1024U;
    unsigned char *payload = malloc(payload_size);
    assert(payload != NULL);
    int src_fd = temporary_file();
    myfs_chunk_t source_chunk;
    myfs_chunk_map_t source_map;
    make_crossing_raw_source(src_fd, payload, payload_size,
                             &source_chunk, &source_map);

    myfs_chunkio_scratch_snapshot_t warm[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
    for (int pass = 0; pass < 2; pass++)
    {
        int dst_fd = temporary_file();
        off_t eof = 0;
        myfs_chunk_t *entries = NULL;
        uint32_t count = 0;
        const char *patch = pass == 0 ? "Q" : NULL;
        size_t patch_len = pass == 0 ? 1 : 0;
        assert(myfs_repack_windows(src_fd, dst_fd, &eof, &source_map,
                                   MYFS_DEFAULT_WINDOW_SIZE, 0, 1,
                                   0, payload_size, patch, 17, patch_len,
                                   &entries, &count) == 0);
        assert(count == 2);
        free(entries);
        assert(close(dst_fd) == 0);
        if (pass == 0)
        {
            for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
                warm[role] = snapshot((myfs_chunkio_scratch_role_t)role);
        }
    }

    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
    {
        myfs_chunkio_scratch_snapshot_t after =
            snapshot((myfs_chunkio_scratch_role_t)role);
        assert(after.growth_count == warm[role].growth_count);
        assert(after.temporary_acquisition_count ==
               warm[role].temporary_acquisition_count);
        assert(!after.in_use);
    }
    assert(close(src_fd) == 0);
    free(payload);
}

enum
{
    REPACK_THREAD_COUNT = 9,
    REPACK_ITERATIONS = 30,
};

typedef struct
{
    pthread_barrier_t *ready;
    pthread_barrier_t *release;
    bool compaction_like;
    void *addresses[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
    int result;
} repack_worker_t;

static int run_repack_worker_iterations(repack_worker_t *worker)
{
    const size_t payload_size = 80U * 1024U;
    unsigned char *payload = malloc(payload_size);
    if (!payload)
        return -ENOMEM;
    int src_fd = temporary_file();
    myfs_chunk_t source_chunk;
    myfs_chunk_map_t source_map;
    make_crossing_raw_source(src_fd, payload, payload_size,
                             &source_chunk, &source_map);

    int result = 0;
    for (unsigned iteration = 0; iteration < REPACK_ITERATIONS; iteration++)
    {
        int dst_fd = temporary_file();
        off_t eof = 0;
        myfs_chunk_t *entries = NULL;
        uint32_t count = 0;
        const char *patch = worker->compaction_like ? NULL : "R";
        size_t patch_len = worker->compaction_like ? 0 : 1;
        result = myfs_repack_windows(
            src_fd, dst_fd, &eof, &source_map, MYFS_DEFAULT_WINDOW_SIZE,
            0, 1, 0, payload_size, patch, 29, patch_len, &entries, &count);
        free(entries);
        close(dst_fd);
        if (result != 0 || count != 2)
        {
            if (result == 0)
                result = -EIO;
            break;
        }
    }

    if (result == 0)
    {
        for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        {
            myfs_chunkio_scratch_snapshot_t state =
                snapshot((myfs_chunkio_scratch_role_t)role);
            if (state.in_use || state.retained_address == NULL)
            {
                result = -EIO;
                break;
            }
            worker->addresses[role] = (void *)state.retained_address;
        }
    }
    close(src_fd);
    free(payload);
    return result;
}

static void *repack_worker_main(void *argument)
{
    repack_worker_t *worker = argument;
    worker->result = run_repack_worker_iterations(worker);
    int ret = pthread_barrier_wait(worker->ready);
    if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD)
        worker->result = -EIO;
    ret = pthread_barrier_wait(worker->release);
    if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD)
        worker->result = -EIO;
    return NULL;
}

static void test_request_and_dedicated_compaction_repack_threads_are_isolated(void)
{
    pthread_barrier_t ready;
    pthread_barrier_t release;
    pthread_t threads[REPACK_THREAD_COUNT];
    repack_worker_t workers[REPACK_THREAD_COUNT] = {0};
    size_t destroy_before = myfs_chunkio_scratch_test_destroy_count();
    assert(pthread_barrier_init(&ready, NULL, REPACK_THREAD_COUNT + 1) == 0);
    assert(pthread_barrier_init(&release, NULL, REPACK_THREAD_COUNT + 1) == 0);

    for (unsigned i = 0; i < REPACK_THREAD_COUNT; i++)
    {
        workers[i] = (repack_worker_t){
            .ready = &ready,
            .release = &release,
            .compaction_like = i == REPACK_THREAD_COUNT - 1,
        };
        assert(pthread_create(&threads[i], NULL, repack_worker_main,
                              &workers[i]) == 0);
    }
    int barrier_ret = pthread_barrier_wait(&ready);
    assert(barrier_ret == 0 || barrier_ret == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < REPACK_THREAD_COUNT; i++)
    {
        assert(workers[i].result == 0);
        for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        {
            assert(workers[i].addresses[role] != NULL);
            for (unsigned j = i + 1; j < REPACK_THREAD_COUNT; j++)
                assert(workers[i].addresses[role] !=
                       workers[j].addresses[role]);
        }
    }
    barrier_ret = pthread_barrier_wait(&release);
    assert(barrier_ret == 0 || barrier_ret == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < REPACK_THREAD_COUNT; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(myfs_chunkio_scratch_test_destroy_count() ==
           destroy_before + REPACK_THREAD_COUNT);
    assert(pthread_barrier_destroy(&release) == 0);
    assert(pthread_barrier_destroy(&ready) == 0);
}

int main(void)
{
    test_payload_load_round_trips_raw_and_compressed();
    test_payload_load_errors_release_raw_role();
    test_blob_append_preserves_format_and_failure_atomicity();
    test_partial_repack_uses_all_four_roles_and_preserves_data();
    test_full_window_fast_path_does_not_grow_window_role();
    test_repack_error_releases_window_cached_and_raw_roles();
    test_oversize_legacy_payload_uses_temporary_without_retention();
    test_foreground_then_fallback_repack_has_no_further_growth();
    test_request_and_dedicated_compaction_repack_threads_are_isolated();
    return 0;
}
