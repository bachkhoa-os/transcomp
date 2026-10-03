#include "myfs.h"

#include <assert.h>
#include <stdatomic.h>

/* Test-only accessors are emitted by compress.c under MYFS_TEST_FAILPOINTS.
 * They are intentionally not part of myfs.h's production API. */
extern ZSTD_CCtx *zstd_test_get_thread_cctx(void);
extern ZSTD_DCtx *zstd_test_get_thread_dctx(void);
extern size_t zstd_test_get_destroy_count(void);

static void fill_pseudorandom(unsigned char *dst, size_t size, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t i = 0; i < size; i++)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        dst[i] = (unsigned char)state;
    }
}

static void assert_compression_matches_one_shot(const void *src, size_t size)
{
    size_t bound = ZSTD_compressBound(size);
    unsigned char *expected = malloc(bound);
    unsigned char *actual = malloc(bound);
    assert(expected != NULL && actual != NULL);
    memset(expected, 0xA5, bound);
    memset(actual, 0xA5, bound);

    size_t expected_size = ZSTD_compress(expected, bound, src, size,
                                         ZSTD_CLEVEL_DEFAULT);
    assert(!ZSTD_isError(expected_size));

    size_t actual_size = SIZE_MAX;
    int ret = zstd_compress(src, size, actual, bound, &actual_size);
    int expected_ret = expected_size >= size - size / 8 ? -EFBIG : 0;
    assert(ret == expected_ret);
    assert(memcmp(actual, expected, expected_size) == 0);
    if (ret == 0)
        assert(actual_size == expected_size);
    else
        assert(actual_size == SIZE_MAX);

    free(actual);
    free(expected);
}

static void test_compression_is_byte_identical_after_reuse(void)
{
    static const size_t sizes[] = {0, 1, 3, 4, 8, 1024, 4096, 16384, 65536};
    unsigned char *repeating = malloc(65536);
    unsigned char *mixed = malloc(65536);
    assert(repeating != NULL && mixed != NULL);
    memset(repeating, 'A', 65536);
    fill_pseudorandom(mixed, 65536, 0x8f31a2c7U);
    memcpy(mixed, "MYFSdata", 8); /* avoid every magic-byte fast path */
    for (size_t pass = 0; pass < 3; pass++)
    {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
            assert_compression_matches_one_shot(repeating, sizes[i]);
            assert_compression_matches_one_shot(mixed, sizes[i]);
        }
    }
    free(mixed);
    free(repeating);
}

static void test_magic_fast_path_stays_untouched(void)
{
    static const unsigned char samples[][16] = {
        {0xff, 0xd8, 0xff, 0xe0},
        {0x89, 0x50, 0x4e, 0x47},
        {0x50, 0x4b, 0x03, 0x04},
        {0x47, 0x49, 0x46, 0x38},
        {0, 0, 0, 0, 0x66, 0x74, 0x79, 0x70},
        {0xfd, 0x2f, 0xb5, 0x28},
        {0x1f, 0x8b, 0, 0},
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
    {
        unsigned char dst[64];
        unsigned char before[64];
        memset(dst, 0x5c, sizeof(dst));
        memcpy(before, dst, sizeof(dst));
        size_t compressed_size = SIZE_MAX;
        assert(zstd_compress(samples[i], sizeof(samples[i]),
                             dst, sizeof(dst), &compressed_size) == -EFBIG);
        assert(compressed_size == SIZE_MAX);
        assert(memcmp(dst, before, sizeof(dst)) == 0);
    }
}

static size_t make_frame(const void *src, size_t src_size,
                         unsigned char **frame_out)
{
    size_t bound = ZSTD_compressBound(src_size);
    unsigned char *frame = malloc(bound);
    assert(frame != NULL);
    size_t frame_size = ZSTD_compress(frame, bound, src, src_size,
                                      ZSTD_CLEVEL_DEFAULT);
    assert(!ZSTD_isError(frame_size));
    *frame_out = frame;
    return frame_size;
}

static void test_errors_discard_contexts_and_next_call_recovers(void)
{
    unsigned char src[4096];
    unsigned char compressed[ZSTD_COMPRESSBOUND(sizeof(src))];
    unsigned char decoded[sizeof(src)];
    memset(src, 'Q', sizeof(src));

    size_t compressed_size = SIZE_MAX;
    assert(zstd_compress(src, sizeof(src), compressed, 1,
                         &compressed_size) == -EIO);
    assert(compressed_size == SIZE_MAX);

    assert(zstd_compress(src, sizeof(src), compressed, sizeof(compressed),
                         &compressed_size) == 0);
    size_t decoded_size = SIZE_MAX;
    const unsigned char invalid_frame[] = {0xde, 0xad, 0xbe, 0xef};
    assert(zstd_decompress(invalid_frame, sizeof(invalid_frame),
                           decoded, sizeof(decoded), &decoded_size) == -EIO);
    assert(decoded_size == SIZE_MAX);
    assert(zstd_decompress(compressed, compressed_size,
                           decoded, sizeof(decoded), &decoded_size) == 0);
    assert(decoded_size == sizeof(src));
    assert(memcmp(decoded, src, sizeof(src)) == 0);
}

static void test_prefix_decompression_preserves_full_frame_validation(void)
{
    unsigned char src[4096];
    memset(src, 'P', sizeof(src));
    unsigned char *frame = NULL;
    size_t frame_size = make_frame(src, sizeof(src), &frame);
    unsigned char prefix[1024];
    assert(zstd_decompress_prefix(frame, frame_size,
                                  prefix, sizeof(prefix)) == 0);
    assert(memcmp(prefix, src, sizeof(prefix)) == 0);
    free(frame);
}

static void test_decompression_clears_sticky_parameters_and_dictionary(void)
{
    enum { DICT_SIZE = 8192, PAYLOAD_SIZE = 4096 };
    unsigned char dict[DICT_SIZE];
    unsigned char payload[PAYLOAD_SIZE];
    fill_pseudorandom(dict, sizeof(dict), 0x4d595346U);
    memcpy(payload, dict + 2048, sizeof(payload));

    unsigned char *plain_frame = NULL;
    size_t plain_size = make_frame(payload, sizeof(payload), &plain_frame);
    size_t frame_capacity = ZSTD_compressBound(sizeof(payload));
    unsigned char *frame = malloc(frame_capacity);
    unsigned char *decoded = malloc(sizeof(payload));
    assert(frame != NULL && decoded != NULL);

    ZSTD_CCtx *encoder = ZSTD_createCCtx();
    assert(encoder != NULL);
    size_t frame_size = ZSTD_compress_usingDict(
        encoder, frame, frame_capacity, payload, sizeof(payload),
        dict, sizeof(dict), ZSTD_CLEVEL_DEFAULT);
    assert(!ZSTD_isError(frame_size));
    ZSTD_freeCCtx(encoder);

    ZSTD_DCtx *fresh = ZSTD_createDCtx();
    assert(fresh != NULL);
    size_t fresh_result = ZSTD_decompressDCtx(
        fresh, decoded, sizeof(payload), plain_frame, plain_size);
    assert(fresh_result == sizeof(payload));

    ZSTD_DCtx *cached = zstd_test_get_thread_dctx();
    assert(cached != NULL);
    assert(!ZSTD_isError(ZSTD_DCtx_setParameter(
        cached, ZSTD_d_windowLogMax, 10)));
    size_t poisoned_result = ZSTD_decompressDCtx(
        cached, decoded, sizeof(payload), plain_frame, plain_size);
    assert(poisoned_result == fresh_result);

    size_t decoded_size = SIZE_MAX;
    assert(zstd_decompress(plain_frame, plain_size, decoded, sizeof(payload),
                           &decoded_size) == 0);
    assert(decoded_size == fresh_result);
    assert(memcmp(decoded, payload, sizeof(payload)) == 0);

    fresh_result = ZSTD_decompressDCtx(
        fresh, decoded, sizeof(payload), frame, frame_size);
    assert(ZSTD_isError(fresh_result));
    ZSTD_freeDCtx(fresh);

    assert(!ZSTD_isError(ZSTD_DCtx_loadDictionary(
        cached, dict, sizeof(dict))));
    poisoned_result = ZSTD_decompressDCtx(
        cached, decoded, sizeof(payload), frame, frame_size);
    assert(poisoned_result == sizeof(payload));
    assert(memcmp(decoded, payload, sizeof(payload)) == 0);

    decoded_size = SIZE_MAX;
    assert(zstd_decompress(frame, frame_size, decoded, sizeof(payload),
                           &decoded_size) == -EIO);
    assert(decoded_size == SIZE_MAX);

    assert(zstd_decompress(plain_frame, plain_size, decoded, sizeof(payload),
                           &decoded_size) == 0);
    assert(decoded_size == sizeof(payload));
    assert(memcmp(decoded, payload, sizeof(payload)) == 0);

    free(plain_frame);
    free(decoded);
    free(frame);
}

enum { THREAD_COUNT = 16, THREAD_ITERATIONS = 200 };

struct worker_state
{
    pthread_barrier_t *ready;
    pthread_barrier_t *release;
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    unsigned id;
    atomic_int *failures;
};

static void barrier_wait_or_die(pthread_barrier_t *barrier)
{
    int ret = pthread_barrier_wait(barrier);
    assert(ret == 0 || ret == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *codec_worker(void *arg)
{
    struct worker_state *worker = arg;
    worker->cctx = zstd_test_get_thread_cctx();
    worker->dctx = zstd_test_get_thread_dctx();
    if (!worker->cctx || !worker->dctx)
        atomic_fetch_add(worker->failures, 1);
    barrier_wait_or_die(worker->ready);
    barrier_wait_or_die(worker->release);

    unsigned char src[4096];
    unsigned char compressed[ZSTD_COMPRESSBOUND(sizeof(src))];
    unsigned char decoded[sizeof(src)];
    memset(src, (int)('A' + worker->id % 20), sizeof(src));
    for (unsigned i = 0; i < THREAD_ITERATIONS; i++)
    {
        size_t compressed_size = 0;
        size_t decoded_size = 0;
        if (zstd_compress(src, sizeof(src), compressed, sizeof(compressed),
                          &compressed_size) != 0 ||
            zstd_decompress(compressed, compressed_size,
                            decoded, sizeof(decoded), &decoded_size) != 0 ||
            decoded_size != sizeof(src) ||
            memcmp(decoded, src, sizeof(src)) != 0)
        {
            atomic_fetch_add(worker->failures, 1);
            break;
        }
    }
    return NULL;
}

static void test_contexts_are_isolated_across_threads(void)
{
    size_t destroy_count_before = zstd_test_get_destroy_count();
    pthread_barrier_t ready;
    pthread_barrier_t release;
    assert(pthread_barrier_init(&ready, NULL, THREAD_COUNT + 1) == 0);
    assert(pthread_barrier_init(&release, NULL, THREAD_COUNT + 1) == 0);
    pthread_t threads[THREAD_COUNT];
    struct worker_state workers[THREAD_COUNT] = {0};
    atomic_int failures = 0;

    for (unsigned i = 0; i < THREAD_COUNT; i++)
    {
        workers[i].ready = &ready;
        workers[i].release = &release;
        workers[i].id = i;
        workers[i].failures = &failures;
        assert(pthread_create(&threads[i], NULL, codec_worker, &workers[i]) == 0);
    }
    barrier_wait_or_die(&ready);
    for (unsigned i = 0; i < THREAD_COUNT; i++)
    {
        assert(workers[i].cctx != NULL && workers[i].dctx != NULL);
        for (unsigned j = i + 1; j < THREAD_COUNT; j++)
        {
            assert(workers[i].cctx != workers[j].cctx);
            assert(workers[i].dctx != workers[j].dctx);
        }
    }
    barrier_wait_or_die(&release);
    for (unsigned i = 0; i < THREAD_COUNT; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(zstd_test_get_destroy_count() ==
           destroy_count_before + THREAD_COUNT);
    assert(atomic_load(&failures) == 0);
    assert(pthread_barrier_destroy(&release) == 0);
    assert(pthread_barrier_destroy(&ready) == 0);
}

int main(void)
{
    test_compression_is_byte_identical_after_reuse();
    test_magic_fast_path_stays_untouched();
    test_errors_discard_contexts_and_next_call_recovers();
    test_prefix_decompression_preserves_full_frame_validation();
    test_decompression_clears_sticky_parameters_and_dictionary();
    test_contexts_are_isolated_across_threads();
    return 0;
}
