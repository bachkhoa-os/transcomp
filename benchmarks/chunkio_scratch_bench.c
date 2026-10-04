#include "myfs.h"
#include "chunkio_scratch.h"

#include <inttypes.h>
#include <stdatomic.h>

enum
{
    DEFAULT_REPETITIONS = 7,
    DEFAULT_ITERATIONS = 1000,
    DEFAULT_SAMPLES = 257,
    DEFAULT_WARMUP = 16,
    WORKLOAD_COUNT = 7,
    THREAD_COUNT_VARIANTS = 2,
};

typedef enum
{
    WORKLOAD_RAW_64K,
    WORKLOAD_COMP_BOUND_64K,
    WORKLOAD_PARTIAL_RMW_64K,
    WORKLOAD_ADAPTIVE_SHRINK,
    WORKLOAD_MIXED_WINDOWS,
    WORKLOAD_MAX_RETAINED,
    WORKLOAD_OVERSIZE,
} workload_kind_t;

typedef enum
{
    MODE_MALLOC_FREE,
    MODE_TLS_REUSE,
} benchmark_mode_t;

typedef struct
{
    workload_kind_t kind;
    const char *name;
} workload_t;

static const workload_t workloads[WORKLOAD_COUNT] = {
    {WORKLOAD_RAW_64K, "raw-64k"},
    {WORKLOAD_COMP_BOUND_64K, "comp-bound-64k"},
    {WORKLOAD_PARTIAL_RMW_64K, "partial-rmw-64k"},
    {WORKLOAD_ADAPTIVE_SHRINK, "adaptive-shrink-1m-512k"},
    {WORKLOAD_MIXED_WINDOWS, "mixed-window-sizes"},
    {WORKLOAD_MAX_RETAINED, "max-retained"},
    {WORKLOAD_OVERSIZE, "oversize-fallback"},
};

static const size_t thread_variants[THREAD_COUNT_VARIANTS] = {1, 8};

typedef struct
{
    size_t repetitions;
    size_t iterations;
    size_t samples;
    size_t warmup;
} options_t;

typedef struct
{
    bool tls_reuse;
    char *data;
    size_t capacity;
    myfs_chunkio_scratch_lease_t lease;
} benchmark_buffer_t;

typedef struct
{
    double operations_per_second;
    uint64_t p50_ns;
    uint64_t p99_ns;
    size_t retained_bytes[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
    size_t total_retained_bytes;
    size_t timed_growths;
    size_t temporary_acquisitions;
    size_t retained_delta_bytes;
} measurement_t;

typedef struct
{
    const workload_t *workload;
    size_t threads;
    size_t repetition;
    bool baseline_first;
    benchmark_mode_t mode;
    measurement_t measurement;
    double speedup;
} result_t;

typedef struct
{
    const workload_t *workload;
    benchmark_mode_t mode;
    size_t iterations;
    size_t samples_count;
    size_t warmup;
    size_t worker_index;
    pthread_barrier_t *ready;
    pthread_barrier_t *latency_done;
    pthread_barrier_t *throughput_start;
    pthread_barrier_t *throughput_done;
    uint64_t *samples;
    myfs_chunkio_scratch_snapshot_t before[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
    myfs_chunkio_scratch_snapshot_t after[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
    volatile uint64_t checksum;
    atomic_int *failed;
} worker_t;

static atomic_uint_fast64_t checksum_sink;

static void fail(const char *message)
{
    fprintf(stderr, "chunkio_scratch_bench: %s\n", message);
    exit(EXIT_FAILURE);
}

static size_t parse_size(const char *text, const char *option)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || end == text || *end != '\0' ||
        value == 0 || value > SIZE_MAX)
    {
        fprintf(stderr, "invalid value for %s: %s\n", option, text);
        exit(EXIT_FAILURE);
    }
    return (size_t)value;
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n", program);
    puts("  --repetitions N   paired repetitions (default 7)");
    puts("  --iterations N    throughput operations per worker (default 1000)");
    puts("  --samples N       latency samples per worker (default 257)");
    puts("  --warmup N        warm-up operations per worker (default 16)");
}

static options_t parse_options(int argc, char **argv)
{
    options_t options = {
        .repetitions = DEFAULT_REPETITIONS,
        .iterations = DEFAULT_ITERATIONS,
        .samples = DEFAULT_SAMPLES,
        .warmup = DEFAULT_WARMUP,
    };
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0)
        {
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (i + 1 >= argc)
            fail("option requires a value");
        const char *option = argv[i];
        const char *value = argv[++i];
        if (strcmp(option, "--repetitions") == 0)
            options.repetitions = parse_size(value, option);
        else if (strcmp(option, "--iterations") == 0)
            options.iterations = parse_size(value, option);
        else if (strcmp(option, "--samples") == 0)
            options.samples = parse_size(value, option);
        else if (strcmp(option, "--warmup") == 0)
            options.warmup = parse_size(value, option);
        else
        {
            fprintf(stderr, "unknown option: %s\n", option);
            exit(EXIT_FAILURE);
        }
    }
    return options;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        fail("clock_gettime failed");
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static uint64_t touch_buffer(char *buffer, size_t size, uint64_t seed)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;
    size_t first_line_end = size > 63 ? 63 : size - 1;
    size_t last_line_start = size > 64 ? size - 64 : 0;
    size_t last = size - 1;
    bytes[0] = (unsigned char)seed;
    bytes[first_line_end] = (unsigned char)(seed >> 8);
    bytes[last_line_start] = (unsigned char)(seed >> 16);
    bytes[last] = (unsigned char)(seed >> 24);
    return (uint64_t)bytes[0] + bytes[first_line_end] +
           bytes[last_line_start] + bytes[last];
}

static int acquire_buffer(benchmark_mode_t mode,
                          myfs_chunkio_scratch_role_t role,
                          size_t size, benchmark_buffer_t *buffer)
{
    *buffer = (benchmark_buffer_t){0};
    if (mode == MODE_TLS_REUSE)
    {
        int ret = myfs_chunkio_scratch_acquire(role, size, &buffer->lease);
        if (ret != 0)
            return ret;
        buffer->tls_reuse = true;
        buffer->data = buffer->lease.data;
        buffer->capacity = buffer->lease.capacity;
        return 0;
    }

    buffer->data = malloc(size);
    if (!buffer->data)
        return -ENOMEM;
    buffer->capacity = size;
    return 0;
}

static void release_buffer(benchmark_buffer_t *buffer)
{
    if (!buffer->data)
        return;
    if (buffer->tls_reuse)
        myfs_chunkio_scratch_release(&buffer->lease);
    else
        free(buffer->data);
    *buffer = (benchmark_buffer_t){0};
}

static int acquire_touch_release(benchmark_mode_t mode,
                                 myfs_chunkio_scratch_role_t role,
                                 size_t size, uint64_t seed,
                                 volatile uint64_t *checksum)
{
    benchmark_buffer_t buffer = {0};
    int ret = acquire_buffer(mode, role, size, &buffer);
    if (ret == 0)
        *checksum += touch_buffer(buffer.data, size, seed);
    release_buffer(&buffer);
    return ret;
}

static int run_workload(const workload_t *workload, benchmark_mode_t mode,
                        uint64_t sequence, volatile uint64_t *checksum)
{
    const size_t window_64k = 64U * 1024U;
    const size_t window_512k = 512U * 1024U;
    benchmark_buffer_t held[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT] = {0};
    int ret = 0;

    switch (workload->kind)
    {
    case WORKLOAD_RAW_64K:
        return acquire_touch_release(mode, MYFS_CHUNKIO_SCRATCH_RAW,
                                     window_64k, sequence, checksum);

    case WORKLOAD_COMP_BOUND_64K:
        return acquire_touch_release(
            mode, MYFS_CHUNKIO_SCRATCH_COMP,
            ZSTD_COMPRESSBOUND(window_64k), sequence, checksum);

    case WORKLOAD_PARTIAL_RMW_64K:
        ret = acquire_buffer(mode, MYFS_CHUNKIO_SCRATCH_WINDOW,
                             window_64k,
                             &held[MYFS_CHUNKIO_SCRATCH_WINDOW]);
        if (ret != 0)
            break;
        *checksum += touch_buffer(
            held[MYFS_CHUNKIO_SCRATCH_WINDOW].data, window_64k, sequence);
        ret = acquire_touch_release(mode, MYFS_CHUNKIO_SCRATCH_RAW,
                                    window_64k, sequence + 1, checksum);
        if (ret == 0)
            ret = acquire_touch_release(
                mode, MYFS_CHUNKIO_SCRATCH_COMP,
                ZSTD_COMPRESSBOUND(window_64k), sequence + 2, checksum);
        break;

    case WORKLOAD_ADAPTIVE_SHRINK:
        ret = acquire_buffer(mode, MYFS_CHUNKIO_SCRATCH_WINDOW,
                             window_512k,
                             &held[MYFS_CHUNKIO_SCRATCH_WINDOW]);
        if (ret != 0)
            break;
        ret = acquire_buffer(mode, MYFS_CHUNKIO_SCRATCH_CACHED,
                             MYFS_MAX_WINDOW_SIZE,
                             &held[MYFS_CHUNKIO_SCRATCH_CACHED]);
        if (ret != 0)
            break;
        *checksum += touch_buffer(
            held[MYFS_CHUNKIO_SCRATCH_WINDOW].data, window_512k, sequence);
        *checksum += touch_buffer(
            held[MYFS_CHUNKIO_SCRATCH_CACHED].data,
            MYFS_MAX_WINDOW_SIZE, sequence + 1);
        ret = acquire_touch_release(mode, MYFS_CHUNKIO_SCRATCH_RAW,
                                    MYFS_MAX_WINDOW_SIZE,
                                    sequence + 2, checksum);
        if (ret == 0)
            ret = acquire_touch_release(
                mode, MYFS_CHUNKIO_SCRATCH_COMP,
                ZSTD_COMPRESSBOUND(window_512k), sequence + 3, checksum);
        if (ret == 0)
            ret = acquire_touch_release(
                mode, MYFS_CHUNKIO_SCRATCH_COMP,
                ZSTD_COMPRESSBOUND(window_512k), sequence + 4, checksum);
        break;

    case WORKLOAD_MIXED_WINDOWS:
    {
        static const size_t sizes[] = {
            16U * 1024U, 32U * 1024U, 64U * 1024U, 128U * 1024U,
            256U * 1024U, 512U * 1024U, 1024U * 1024U,
        };
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        {
            ret = acquire_buffer(mode, MYFS_CHUNKIO_SCRATCH_WINDOW,
                                 sizes[i],
                                 &held[MYFS_CHUNKIO_SCRATCH_WINDOW]);
            if (ret != 0)
                break;
            *checksum += touch_buffer(
                held[MYFS_CHUNKIO_SCRATCH_WINDOW].data, sizes[i],
                sequence + i * 3);
            ret = acquire_touch_release(mode, MYFS_CHUNKIO_SCRATCH_RAW,
                                        sizes[i], sequence + i * 3 + 1,
                                        checksum);
            if (ret == 0)
                ret = acquire_touch_release(
                    mode, MYFS_CHUNKIO_SCRATCH_COMP,
                    ZSTD_COMPRESSBOUND(sizes[i]),
                    sequence + i * 3 + 2, checksum);
            release_buffer(&held[MYFS_CHUNKIO_SCRATCH_WINDOW]);
            if (ret != 0)
                break;
        }
        break;
    }

    case WORKLOAD_MAX_RETAINED:
        for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        {
            size_t size = myfs_chunkio_scratch_retained_limit(
                (myfs_chunkio_scratch_role_t)role);
            ret = acquire_buffer(mode, (myfs_chunkio_scratch_role_t)role,
                                 size, &held[role]);
            if (ret != 0)
                break;
            *checksum += touch_buffer(held[role].data, size,
                                      sequence + (uint64_t)role);
        }
        break;

    case WORKLOAD_OVERSIZE:
        for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        {
            size_t size = myfs_chunkio_scratch_retained_limit(
                              (myfs_chunkio_scratch_role_t)role) +
                          4096;
            ret = acquire_buffer(mode, (myfs_chunkio_scratch_role_t)role,
                                 size, &held[role]);
            if (ret != 0)
                break;
            *checksum += touch_buffer(held[role].data, size,
                                      sequence + (uint64_t)role);
        }
        break;
    }

    for (int role = MYFS_CHUNKIO_SCRATCH_ROLE_COUNT - 1; role >= 0; role--)
        release_buffer(&held[role]);
    return ret;
}

static void snapshot_worker(
    myfs_chunkio_scratch_snapshot_t snapshots[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT],
    atomic_int *failed)
{
    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
    {
        if (myfs_chunkio_scratch_test_snapshot(
                (myfs_chunkio_scratch_role_t)role,
                &snapshots[role]) != 0)
            atomic_store(failed, 1);
    }
}

static void wait_at_barrier(pthread_barrier_t *barrier, atomic_int *failed)
{
    int ret = pthread_barrier_wait(barrier);
    if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD)
        atomic_store(failed, 1);
}

static void *worker_main(void *argument)
{
    worker_t *worker = argument;
    uint64_t sequence = (uint64_t)worker->worker_index << 48;

    for (size_t i = 0; i < worker->warmup; i++)
    {
        if (run_workload(worker->workload, worker->mode,
                         sequence++, &worker->checksum) != 0)
        {
            atomic_store(worker->failed, 1);
            break;
        }
    }
    if (worker->mode == MODE_TLS_REUSE)
        snapshot_worker(worker->before, worker->failed);
    wait_at_barrier(worker->ready, worker->failed);

    if (!atomic_load(worker->failed))
    {
        for (size_t i = 0; i < worker->samples_count; i++)
        {
            uint64_t start = monotonic_ns();
            int ret = run_workload(worker->workload, worker->mode,
                                   sequence++, &worker->checksum);
            uint64_t elapsed = monotonic_ns() - start;
            if (ret != 0)
            {
                atomic_store(worker->failed, 1);
                break;
            }
            worker->samples[i] = elapsed == 0 ? 1 : elapsed;
        }
    }
    wait_at_barrier(worker->latency_done, worker->failed);
    wait_at_barrier(worker->throughput_start, worker->failed);
    if (!atomic_load(worker->failed))
    {
        for (size_t i = 0; i < worker->iterations; i++)
        {
            if (run_workload(worker->workload, worker->mode,
                             sequence++, &worker->checksum) != 0)
            {
                atomic_store(worker->failed, 1);
                break;
            }
        }
    }
    if (worker->mode == MODE_TLS_REUSE)
        snapshot_worker(worker->after, worker->failed);
    wait_at_barrier(worker->throughput_done, worker->failed);
    atomic_fetch_add(&checksum_sink, worker->checksum);
    return NULL;
}

static measurement_t measure(const workload_t *workload,
                             benchmark_mode_t mode, size_t thread_count,
                             const options_t *options)
{
    pthread_t *threads = calloc(thread_count, sizeof(*threads));
    worker_t *workers = calloc(thread_count, sizeof(*workers));
    if (options->samples > SIZE_MAX / thread_count)
        fail("latency sample count overflow");
    size_t total_samples = options->samples * thread_count;
    uint64_t *samples = calloc(total_samples, sizeof(*samples));
    if (!threads || !workers || !samples)
        fail("measurement allocation failed");

    pthread_barrier_t ready;
    pthread_barrier_t latency_done;
    pthread_barrier_t throughput_start;
    pthread_barrier_t throughput_done;
    if (pthread_barrier_init(&ready, NULL, (unsigned)thread_count + 1) != 0 ||
        pthread_barrier_init(&latency_done, NULL,
                             (unsigned)thread_count + 1) != 0 ||
        pthread_barrier_init(&throughput_start, NULL,
                             (unsigned)thread_count + 1) != 0 ||
        pthread_barrier_init(&throughput_done, NULL,
                             (unsigned)thread_count + 1) != 0)
        fail("pthread_barrier_init failed");

    atomic_int failed = 0;
    for (size_t i = 0; i < thread_count; i++)
    {
        workers[i] = (worker_t){
            .workload = workload,
            .mode = mode,
            .iterations = options->iterations,
            .samples_count = options->samples,
            .warmup = options->warmup,
            .worker_index = i,
            .ready = &ready,
            .latency_done = &latency_done,
            .throughput_start = &throughput_start,
            .throughput_done = &throughput_done,
            .samples = samples + i * options->samples,
            .failed = &failed,
        };
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0)
            fail("pthread_create failed");
    }

    wait_at_barrier(&ready, &failed);
    wait_at_barrier(&latency_done, &failed);
    uint64_t start = monotonic_ns();
    wait_at_barrier(&throughput_start, &failed);
    wait_at_barrier(&throughput_done, &failed);
    uint64_t elapsed = monotonic_ns() - start;
    for (size_t i = 0; i < thread_count; i++)
    {
        if (pthread_join(threads[i], NULL) != 0)
            fail("pthread_join failed");
    }
    if (atomic_load(&failed))
        fail("worker operation failed");

    qsort(samples, total_samples, sizeof(samples[0]), compare_u64);
    measurement_t measurement = {
        .operations_per_second =
            (double)(thread_count * options->iterations) * 1e9 /
            (double)(elapsed == 0 ? 1 : elapsed),
        .p50_ns = samples[total_samples / 2],
        .p99_ns = samples[(total_samples * 99) / 100],
    };

    if (mode == MODE_TLS_REUSE)
    {
        for (size_t worker_index = 0; worker_index < thread_count;
             worker_index++)
        {
            size_t worker_before_bytes = 0;
            size_t worker_after_bytes = 0;
            for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
            {
                myfs_chunkio_scratch_snapshot_t *before =
                    &workers[worker_index].before[role];
                myfs_chunkio_scratch_snapshot_t *after =
                    &workers[worker_index].after[role];
                if (after->retained_capacity >
                    measurement.retained_bytes[role])
                    measurement.retained_bytes[role] =
                        after->retained_capacity;
                measurement.timed_growths +=
                    after->growth_count - before->growth_count;
                measurement.temporary_acquisitions +=
                    after->temporary_acquisition_count -
                    before->temporary_acquisition_count;
                worker_before_bytes += before->retained_capacity;
                worker_after_bytes += after->retained_capacity;
            }
            size_t delta = worker_after_bytes - worker_before_bytes;
            if (delta > measurement.retained_delta_bytes)
                measurement.retained_delta_bytes = delta;
        }
        for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
            measurement.total_retained_bytes +=
                measurement.retained_bytes[role];
    }

    if (pthread_barrier_destroy(&throughput_done) != 0 ||
        pthread_barrier_destroy(&throughput_start) != 0 ||
        pthread_barrier_destroy(&latency_done) != 0 ||
        pthread_barrier_destroy(&ready) != 0)
        fail("pthread_barrier_destroy failed");
    free(samples);
    free(workers);
    free(threads);
    return measurement;
}

static const char *mode_name(benchmark_mode_t mode)
{
    return mode == MODE_MALLOC_FREE ? "malloc-free" : "tls-reuse";
}

static void print_csv_result(const result_t *result)
{
    const measurement_t *measurement = &result->measurement;
    printf("%s,%zu,%zu,%s,%s,%.3f,%" PRIu64 ",%" PRIu64
           ",%.6f,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",
           result->workload->name, result->threads, result->repetition,
           result->baseline_first ? "baseline-first" : "reuse-first",
           mode_name(result->mode), measurement->operations_per_second,
           measurement->p50_ns, measurement->p99_ns, result->speedup,
           measurement->retained_bytes[MYFS_CHUNKIO_SCRATCH_RAW],
           measurement->retained_bytes[MYFS_CHUNKIO_SCRATCH_COMP],
           measurement->retained_bytes[MYFS_CHUNKIO_SCRATCH_WINDOW],
           measurement->retained_bytes[MYFS_CHUNKIO_SCRATCH_CACHED],
           measurement->total_retained_bytes,
           measurement->timed_growths,
           measurement->temporary_acquisitions,
           measurement->retained_delta_bytes);
}

static double median_double(double *values, size_t count)
{
    qsort(values, count, sizeof(values[0]), compare_double);
    return values[count / 2];
}

static uint64_t median_u64(uint64_t *values, size_t count)
{
    qsort(values, count, sizeof(values[0]), compare_u64);
    return values[count / 2];
}

static const result_t *find_result(const result_t *results,
                                   size_t result_count,
                                   const workload_t *workload,
                                   size_t threads, size_t repetition,
                                   benchmark_mode_t mode)
{
    for (size_t i = 0; i < result_count; i++)
    {
        if (results[i].workload == workload &&
            results[i].threads == threads &&
            results[i].repetition == repetition &&
            results[i].mode == mode)
            return &results[i];
    }
    fail("missing result while building summary");
    return NULL;
}

static void print_summary(const options_t *options,
                          const result_t *results, size_t result_count)
{
    puts("+----------------------------+---------+------------------+---------------+---------+--------------+--------------+-----------------+---------+---------+");
    puts("| Workload                   | Threads | malloc-free ops/s | TLS reuse ops/s | Speedup | TLS p50 (ns) | TLS p99 (ns) | Retained/thread | Growths | Temps   |");
    puts("+----------------------------+---------+------------------+---------------+---------+--------------+--------------+-----------------+---------+---------+");
    for (size_t workload_index = 0;
         workload_index < WORKLOAD_COUNT; workload_index++)
    {
        for (size_t thread_index = 0;
             thread_index < THREAD_COUNT_VARIANTS; thread_index++)
        {
            double *baseline_ops = calloc(options->repetitions,
                                           sizeof(*baseline_ops));
            double *reuse_ops = calloc(options->repetitions,
                                        sizeof(*reuse_ops));
            uint64_t *reuse_p50 = calloc(options->repetitions,
                                          sizeof(*reuse_p50));
            uint64_t *reuse_p99 = calloc(options->repetitions,
                                          sizeof(*reuse_p99));
            if (!baseline_ops || !reuse_ops || !reuse_p50 || !reuse_p99)
                fail("summary allocation failed");
            size_t retained = 0;
            size_t growths = 0;
            size_t temporaries = 0;
            for (size_t repetition = 0;
                 repetition < options->repetitions; repetition++)
            {
                const result_t *baseline = find_result(
                    results, result_count, &workloads[workload_index],
                    thread_variants[thread_index], repetition,
                    MODE_MALLOC_FREE);
                const result_t *reuse = find_result(
                    results, result_count, &workloads[workload_index],
                    thread_variants[thread_index], repetition,
                    MODE_TLS_REUSE);
                baseline_ops[repetition] =
                    baseline->measurement.operations_per_second;
                reuse_ops[repetition] =
                    reuse->measurement.operations_per_second;
                reuse_p50[repetition] = reuse->measurement.p50_ns;
                reuse_p99[repetition] = reuse->measurement.p99_ns;
                if (reuse->measurement.total_retained_bytes > retained)
                    retained = reuse->measurement.total_retained_bytes;
                if (reuse->measurement.timed_growths > growths)
                    growths = reuse->measurement.timed_growths;
                if (reuse->measurement.temporary_acquisitions > temporaries)
                    temporaries =
                        reuse->measurement.temporary_acquisitions;
            }
            double baseline_median = median_double(
                baseline_ops, options->repetitions);
            double reuse_median = median_double(reuse_ops,
                                                options->repetitions);
            printf("| %-26s | %7zu | %16.0f | %15.0f | %6.2fx | %12" PRIu64
                   " | %12" PRIu64 " | %15zu | %7zu | %7zu |\n",
                   workloads[workload_index].name,
                   thread_variants[thread_index], baseline_median,
                   reuse_median, reuse_median / baseline_median,
                   median_u64(reuse_p50, options->repetitions),
                   median_u64(reuse_p99, options->repetitions),
                   retained, growths, temporaries);
            free(reuse_p99);
            free(reuse_p50);
            free(reuse_ops);
            free(baseline_ops);
        }
    }
    puts("+----------------------------+---------+------------------+---------------+---------+--------------+--------------+-----------------+---------+---------+");
}

int main(int argc, char **argv)
{
    options_t options = parse_options(argc, argv);
    if (options.repetitions > SIZE_MAX / WORKLOAD_COUNT /
                                  THREAD_COUNT_VARIANTS / 2)
        fail("result count overflow");
    size_t result_capacity = options.repetitions * WORKLOAD_COUNT *
                             THREAD_COUNT_VARIANTS * 2;
    result_t *results = calloc(result_capacity, sizeof(*results));
    if (!results)
        fail("result allocation failed");

    puts("Chunk-I/O allocation microbenchmark (no FUSE, I/O, or compression)");
    puts("Each operation touches the first and last cache lines; latency and throughput use separate timed passes.");
    printf("Configuration: repetitions=%zu iterations=%zu samples=%zu warmup=%zu threads=1,8\n",
           options.repetitions, options.iterations,
           options.samples, options.warmup);
    puts("CSV_BEGIN");
    puts("workload,threads,repetition,order,mode,ops_per_sec,p50_ns,p99_ns,speedup,raw_bytes,comp_bytes,window_bytes,cached_bytes,total_retained_bytes,timed_growths,temp_acquisitions,retained_delta_bytes");

    size_t result_count = 0;
    for (size_t workload_index = 0;
         workload_index < WORKLOAD_COUNT; workload_index++)
    {
        for (size_t thread_index = 0;
             thread_index < THREAD_COUNT_VARIANTS; thread_index++)
        {
            for (size_t repetition = 0;
                 repetition < options.repetitions; repetition++)
            {
                bool baseline_first = repetition % 2 == 0;
                result_t pair[2] = {0};
                for (size_t order = 0; order < 2; order++)
                {
                    benchmark_mode_t mode =
                        (baseline_first == (order == 0))
                            ? MODE_MALLOC_FREE
                            : MODE_TLS_REUSE;
                    size_t pair_index = mode == MODE_MALLOC_FREE ? 0 : 1;
                    pair[pair_index] = (result_t){
                        .workload = &workloads[workload_index],
                        .threads = thread_variants[thread_index],
                        .repetition = repetition,
                        .baseline_first = baseline_first,
                        .mode = mode,
                        .measurement = measure(
                            &workloads[workload_index], mode,
                            thread_variants[thread_index], &options),
                    };
                }
                double speedup =
                    pair[1].measurement.operations_per_second /
                    pair[0].measurement.operations_per_second;
                pair[0].speedup = speedup;
                pair[1].speedup = speedup;
                if (baseline_first)
                {
                    results[result_count++] = pair[0];
                    results[result_count++] = pair[1];
                }
                else
                {
                    results[result_count++] = pair[1];
                    results[result_count++] = pair[0];
                }
                print_csv_result(&results[result_count - 2]);
                print_csv_result(&results[result_count - 1]);
                fflush(stdout);
            }
        }
    }
    puts("CSV_END");
    putchar('\n');
    print_summary(&options, results, result_count);
    printf("checksum=%" PRIuFAST64 "\n", atomic_load(&checksum_sink));
    free(results);
    return EXIT_SUCCESS;
}
