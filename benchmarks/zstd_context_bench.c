#include "myfs.h"

#include <inttypes.h>
#include <stdatomic.h>

enum
{
    LATENCY_SAMPLE_COUNT = 5001,
    THREAD_COUNT = 8,
    THREAD_ITERATIONS = 4000,
    WARMUP_ITERATIONS = 100,
};

typedef enum
{
    OP_COMPRESS,
    OP_DECOMPRESS,
} operation_t;

typedef struct
{
    unsigned char *source;
    unsigned char *frame;
    size_t source_size;
    size_t frame_size;
    size_t frame_capacity;
} benchmark_case_t;

static atomic_size_t result_sink;

static void fail_zstd(const char *operation, size_t result)
{
    fprintf(stderr, "%s failed: %s\n", operation, ZSTD_getErrorName(result));
    exit(EXIT_FAILURE);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void fill_compressible_data(unsigned char *buffer, size_t size)
{
    static const char pattern[] =
        "myfs context reuse benchmark: repeated source-code-like payload\n";
    for (size_t i = 0; i < size; i++)
        buffer[i] = (unsigned char)pattern[i % (sizeof(pattern) - 1)];
}

static benchmark_case_t make_case(size_t source_size)
{
    benchmark_case_t test_case = {0};
    test_case.source_size = source_size;
    test_case.frame_capacity = ZSTD_compressBound(source_size);
    test_case.source = malloc(source_size);
    test_case.frame = malloc(test_case.frame_capacity);
    if (!test_case.source || !test_case.frame)
    {
        fprintf(stderr, "benchmark allocation failed\n");
        exit(EXIT_FAILURE);
    }

    fill_compressible_data(test_case.source, source_size);
    test_case.frame_size = ZSTD_compress(
        test_case.frame, test_case.frame_capacity,
        test_case.source, source_size, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(test_case.frame_size))
        fail_zstd("fixture compression", test_case.frame_size);
    return test_case;
}

static void free_case(benchmark_case_t *test_case)
{
    free(test_case->frame);
    free(test_case->source);
}

static size_t run_call(const benchmark_case_t *test_case,
                       operation_t operation, bool reuse,
                       unsigned char *destination)
{
    size_t result = 0;
    if (operation == OP_COMPRESS)
    {
        if (reuse)
        {
            if (zstd_compress(test_case->source, test_case->source_size,
                              destination, test_case->frame_capacity,
                              &result) != 0)
            {
                fprintf(stderr, "zstd_compress wrapper failed\n");
                exit(EXIT_FAILURE);
            }
        }
        else
        {
            result = ZSTD_compress(destination, test_case->frame_capacity,
                                   test_case->source, test_case->source_size,
                                   ZSTD_CLEVEL_DEFAULT);
            if (ZSTD_isError(result))
                fail_zstd("one-shot compression", result);
        }
    }
    else if (reuse)
    {
        if (zstd_decompress(test_case->frame, test_case->frame_size,
                            destination, test_case->source_size,
                            &result) != 0)
        {
            fprintf(stderr, "zstd_decompress wrapper failed\n");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        result = ZSTD_decompress(destination, test_case->source_size,
                                 test_case->frame, test_case->frame_size);
        if (ZSTD_isError(result))
            fail_zstd("one-shot decompression", result);
    }
    if (operation == OP_DECOMPRESS && result != test_case->source_size)
    {
        fprintf(stderr, "decompression returned an unexpected size\n");
        exit(EXIT_FAILURE);
    }
    return result;
}

static int compare_double(const void *lhs, const void *rhs)
{
    double a = *(const double *)lhs;
    double b = *(const double *)rhs;
    return (a > b) - (a < b);
}

static void measure_latency(const benchmark_case_t *test_case,
                            operation_t operation, bool reuse,
                            size_t throughput_iterations,
                            double samples[LATENCY_SAMPLE_COUNT],
                            double *operations_per_second)
{
    size_t destination_size = operation == OP_COMPRESS
                                  ? test_case->frame_capacity
                                  : test_case->source_size;
    unsigned char *destination = malloc(destination_size);
    if (!destination)
    {
        fprintf(stderr, "benchmark allocation failed\n");
        exit(EXIT_FAILURE);
    }

    size_t local_sink = 0;
    for (unsigned i = 0; i < WARMUP_ITERATIONS; i++)
        local_sink += run_call(test_case, operation, reuse, destination);

    for (size_t sample = 0; sample < LATENCY_SAMPLE_COUNT; sample++)
    {
        uint64_t start = monotonic_ns();
        local_sink += run_call(test_case, operation, reuse, destination);
        samples[sample] = (double)(monotonic_ns() - start);
    }

    uint64_t throughput_start = monotonic_ns();
    for (size_t iteration = 0; iteration < throughput_iterations; iteration++)
        local_sink += run_call(test_case, operation, reuse, destination);
    uint64_t throughput_ns = monotonic_ns() - throughput_start;
    *operations_per_second =
        (double)throughput_iterations * 1e9 / (double)throughput_ns;
    qsort(samples, LATENCY_SAMPLE_COUNT, sizeof(samples[0]), compare_double);
    atomic_fetch_add(&result_sink, local_sink);
    free(destination);
}

static void print_latency_pair(const benchmark_case_t *test_case,
                               operation_t operation,
                               size_t throughput_iterations)
{
    double one_shot[LATENCY_SAMPLE_COUNT];
    double reused[LATENCY_SAMPLE_COUNT];
    double one_shot_ops = 0.0;
    double reused_ops = 0.0;
    size_t p50 = LATENCY_SAMPLE_COUNT / 2;
    size_t p99 = (LATENCY_SAMPLE_COUNT * 99) / 100;

    measure_latency(test_case, operation, false, throughput_iterations,
                    one_shot, &one_shot_ops);
    measure_latency(test_case, operation, true, throughput_iterations,
                    reused, &reused_ops);

    const char *operation_name =
        operation == OP_COMPRESS ? "compress" : "decompress";
    char speedup[64];
    snprintf(speedup, sizeof(speedup), "%.2fx ops/s, %.2fx p50, %.2fx p99",
             reused_ops / one_shot_ops,
             one_shot[p50] / reused[p50],
             one_shot[p99] / reused[p99]);

    printf("| %-10s | %7zu | %-9s | %12.0f | %10.0f | %10.0f | %-33s |\n",
           operation_name, test_case->source_size, "one-shot", one_shot_ops,
           one_shot[p50], one_shot[p99], "-");
    printf("| %-10s | %7zu | %-9s | %12.0f | %10.0f | %10.0f | %-33s |\n",
           operation_name, test_case->source_size, "TLS-reuse", reused_ops,
           reused[p50], reused[p99], speedup);
}

typedef struct
{
    const benchmark_case_t *test_case;
    pthread_barrier_t *ready;
    pthread_barrier_t *start;
    pthread_barrier_t *finish;
    operation_t operation;
    bool reuse;
    atomic_int *failed;
} throughput_worker_t;

static void *run_throughput_worker(void *argument)
{
    throughput_worker_t *worker = argument;
    size_t destination_size = worker->operation == OP_COMPRESS
                                  ? worker->test_case->frame_capacity
                                  : worker->test_case->source_size;
    unsigned char *destination = malloc(destination_size);
    if (!destination)
    {
        atomic_store(worker->failed, 1);
        pthread_barrier_wait(worker->ready);
        pthread_barrier_wait(worker->start);
        pthread_barrier_wait(worker->finish);
        return NULL;
    }

    size_t local_sink = 0;
    for (unsigned i = 0; i < WARMUP_ITERATIONS; i++)
        local_sink += run_call(worker->test_case, worker->operation,
                               worker->reuse, destination);

    pthread_barrier_wait(worker->ready);
    pthread_barrier_wait(worker->start);
    for (unsigned i = 0; i < THREAD_ITERATIONS; i++)
        local_sink += run_call(worker->test_case, worker->operation,
                               worker->reuse, destination);
    atomic_fetch_add(&result_sink, local_sink);
    pthread_barrier_wait(worker->finish);
    free(destination);
    return NULL;
}

static double measure_threaded_throughput(const benchmark_case_t *test_case,
                                          operation_t operation, bool reuse)
{
    pthread_t threads[THREAD_COUNT];
    throughput_worker_t workers[THREAD_COUNT];
    pthread_barrier_t ready;
    pthread_barrier_t start;
    pthread_barrier_t finish;
    atomic_int failed = 0;
    if (pthread_barrier_init(&ready, NULL, THREAD_COUNT + 1) != 0 ||
        pthread_barrier_init(&start, NULL, THREAD_COUNT + 1) != 0 ||
        pthread_barrier_init(&finish, NULL, THREAD_COUNT + 1) != 0)
    {
        fprintf(stderr, "pthread_barrier_init failed\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < THREAD_COUNT; i++)
    {
        workers[i] = (throughput_worker_t){
            .test_case = test_case,
            .ready = &ready,
            .start = &start,
            .finish = &finish,
            .operation = operation,
            .reuse = reuse,
            .failed = &failed,
        };
        if (pthread_create(&threads[i], NULL,
                           run_throughput_worker, &workers[i]) != 0)
        {
            fprintf(stderr, "pthread_create failed\n");
            exit(EXIT_FAILURE);
        }
    }

    pthread_barrier_wait(&ready);
    uint64_t start_ns = monotonic_ns();
    pthread_barrier_wait(&start);
    pthread_barrier_wait(&finish);
    uint64_t elapsed_ns = monotonic_ns() - start_ns;
    for (size_t i = 0; i < THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&finish);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&ready);

    if (atomic_load(&failed))
    {
        fprintf(stderr, "threaded benchmark allocation failed\n");
        exit(EXIT_FAILURE);
    }
    return (double)THREAD_COUNT * (double)THREAD_ITERATIONS * 1e9 /
           (double)elapsed_ns;
}

static void print_threaded_pair(const benchmark_case_t *test_case,
                                operation_t operation)
{
    double one_shot = measure_threaded_throughput(test_case, operation, false);
    double reused = measure_threaded_throughput(test_case, operation, true);
    printf("| %-10s | %7zu | %7d | %16.0f | %17.0f | %6.2fx |\n",
           operation == OP_COMPRESS ? "compress" : "decompress",
           test_case->source_size, THREAD_COUNT,
           one_shot, reused, reused / one_shot);
}

int main(void)
{
    static const size_t sizes[] = {1024, 4096, 65536};

    puts("Zstd context allocation microbenchmark");
    puts("Workload: compressible buffers; same process, library, and compiler");
    puts("Latency percentiles time individual calls; ops/sec uses a separate tight loop");
    puts("+------------+---------+-----------+--------------+------------+------------+-----------------------------------+");
    puts("| Operation  | Bytes   | Mode      | Ops/sec      | p50 (ns)   | p99 (ns)   | Speedup                           |");
    puts("+------------+---------+-----------+--------------+------------+------------+-----------------------------------+");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        benchmark_case_t test_case = make_case(sizes[i]);
        size_t throughput_iterations = sizes[i] <= 4096 ? 30000 : 3000;
        print_latency_pair(&test_case, OP_COMPRESS, throughput_iterations);
        print_latency_pair(&test_case, OP_DECOMPRESS, throughput_iterations);
        free_case(&test_case);
    }
    puts("+------------+---------+-----------+--------------+------------+------------+-----------------------------------+");

    puts("\nThread-pool-style steady-state throughput (4 KiB, 8 fresh workers)");
    puts("+------------+---------+---------+------------------+-------------------+---------+");
    puts("| Operation  | Bytes   | Workers | One-shot ops/sec | TLS reuse ops/sec | Speedup |");
    puts("+------------+---------+---------+------------------+-------------------+---------+");
    benchmark_case_t threaded_case = make_case(4096);
    print_threaded_pair(&threaded_case, OP_COMPRESS);
    print_threaded_pair(&threaded_case, OP_DECOMPRESS);
    free_case(&threaded_case);
    puts("+------------+---------+---------+------------------+-------------------+---------+");

    return atomic_load(&result_sink) == SIZE_MAX ? EXIT_FAILURE : EXIT_SUCCESS;
}
