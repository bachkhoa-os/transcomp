#include "myfs.h"

#include <inttypes.h>
#include <stdatomic.h>

enum
{
    DEFAULT_REPETITIONS = 7,
    DEFAULT_WARMUP_MS = 100,
    DEFAULT_DURATION_MS = 500,
    DEFAULT_LATENCY_SAMPLES = 5001,
    WORKER_PATH_COUNT = 64,
    MAX_LIST_VALUES = 32,
};

void *linear_lock_file(const char *path);
void linear_unlock_file(void *lock);
void linear_destroy_lock_table(void);

struct backend
{
    const char *name;
    void *(*lock)(const char *path);
    void (*unlock)(void *lock);
    void (*destroy)(void);
};

struct options
{
    size_t workers[MAX_LIST_VALUES];
    size_t worker_count;
    size_t resident_paths[MAX_LIST_VALUES];
    size_t resident_count;
    size_t repetitions;
    uint64_t warmup_ms;
    uint64_t duration_ms;
    size_t latency_samples;
};

struct benchmark_result
{
    const char *implementation;
    size_t workers;
    size_t resident_paths;
    size_t repetition;
    uint64_t operations;
    uint64_t elapsed_ns;
    double operations_per_second;
    uint64_t p50_ns;
    uint64_t p99_ns;
    size_t latency_samples;
};

static void *sharded_lock_file(const char *path)
{
    return myfs_lock_file(path);
}

static void sharded_unlock_file(void *lock)
{
    myfs_unlock_file(lock);
}

static const struct backend backends[] = {
    {
        .name = "linear",
        .lock = linear_lock_file,
        .unlock = linear_unlock_file,
        .destroy = linear_destroy_lock_table,
    },
    {
        .name = "sharded",
        .lock = sharded_lock_file,
        .unlock = sharded_unlock_file,
        .destroy = destroy_lock_table,
    },
};

static void fail(const char *message)
{
    fprintf(stderr, "lock-table benchmark: %s\n", message);
    exit(EXIT_FAILURE);
}

static void check_pthread(int status, const char *operation)
{
    if (status != 0)
    {
        fprintf(stderr, "lock-table benchmark: %s: %s\n",
                operation, strerror(status));
        exit(EXIT_FAILURE);
    }
}

static void wait_at_barrier(pthread_barrier_t *barrier)
{
    int status = pthread_barrier_wait(barrier);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
        check_pthread(status, "pthread_barrier_wait");
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

static void sleep_ms(uint64_t milliseconds)
{
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000),
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0)
    {
        if (errno != EINTR)
        {
            perror("nanosleep");
            exit(EXIT_FAILURE);
        }
    }
}

static size_t parse_size(const char *text, const char *option, bool allow_zero)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || end == text ||
        value > SIZE_MAX || (!allow_zero && value == 0))
    {
        fprintf(stderr, "invalid value for %s: %s\n", option, text);
        exit(EXIT_FAILURE);
    }
    return (size_t)value;
}

static void parse_list(const char *text, const char *option,
                       bool allow_zero, size_t values[MAX_LIST_VALUES],
                       size_t *value_count)
{
    char *copy = strdup(text);
    if (!copy)
        fail("allocation failed");

    *value_count = 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save);
         part;
         part = strtok_r(NULL, ",", &save))
    {
        if (*value_count == MAX_LIST_VALUES)
            fail("too many comma-separated values");
        values[(*value_count)++] = parse_size(part, option, allow_zero);
    }
    free(copy);
    if (*value_count == 0)
        fail("an option list cannot be empty");
}

static void usage(const char *program)
{
    printf("Usage: %s [options]\n", program);
    puts("  --workers LIST          comma-separated worker counts");
    puts("  --resident-paths LIST   comma-separated resident path counts");
    puts("  --repetitions N         paired repetitions (default 7)");
    puts("  --warmup-ms N           throughput warmup milliseconds (default 100)");
    puts("  --duration-ms N         throughput measurement milliseconds (default 500)");
    puts("  --latency-samples N     samples per worker (default 5001)");
}

static struct options parse_options(int argc, char **argv)
{
    struct options options = {
        .workers = {1, 4, 8, 16},
        .worker_count = 4,
        .resident_paths = {0, 64, 256},
        .resident_count = 3,
        .repetitions = DEFAULT_REPETITIONS,
        .warmup_ms = DEFAULT_WARMUP_MS,
        .duration_ms = DEFAULT_DURATION_MS,
        .latency_samples = DEFAULT_LATENCY_SAMPLES,
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
        if (strcmp(option, "--workers") == 0)
            parse_list(value, option, false,
                       options.workers, &options.worker_count);
        else if (strcmp(option, "--resident-paths") == 0)
            parse_list(value, option, true,
                       options.resident_paths, &options.resident_count);
        else if (strcmp(option, "--repetitions") == 0)
            options.repetitions = parse_size(value, option, false);
        else if (strcmp(option, "--warmup-ms") == 0)
            options.warmup_ms = parse_size(value, option, true);
        else if (strcmp(option, "--duration-ms") == 0)
            options.duration_ms = parse_size(value, option, false);
        else if (strcmp(option, "--latency-samples") == 0)
            options.latency_samples = parse_size(value, option, false);
        else
        {
            fprintf(stderr, "unknown option: %s\n", option);
            exit(EXIT_FAILURE);
        }
    }

    return options;
}

struct resident_group;

struct resident_context
{
    struct resident_group *group;
    char path[96];
};

struct resident_group
{
    const struct backend *backend;
    size_t count;
    pthread_t *threads;
    struct resident_context *contexts;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t ready;
    bool release;
    bool failed;
};

static void *resident_main(void *argument)
{
    struct resident_context *context = argument;
    struct resident_group *group = context->group;
    void *lock = group->backend->lock(context->path);

    check_pthread(pthread_mutex_lock(&group->mu), "pthread_mutex_lock");
    if (!lock)
        group->failed = true;
    group->ready++;
    check_pthread(pthread_cond_broadcast(&group->cv), "pthread_cond_broadcast");
    while (!group->release)
        check_pthread(pthread_cond_wait(&group->cv, &group->mu),
                      "pthread_cond_wait");
    check_pthread(pthread_mutex_unlock(&group->mu), "pthread_mutex_unlock");

    if (lock)
        group->backend->unlock(lock);
    return NULL;
}

static void start_residents(struct resident_group *group,
                            const struct backend *backend, size_t count)
{
    *group = (struct resident_group){
        .backend = backend,
        .count = count,
    };
    if (count == 0)
        return;

    group->threads = calloc(count, sizeof(*group->threads));
    group->contexts = calloc(count, sizeof(*group->contexts));
    if (!group->threads || !group->contexts)
        fail("resident allocation failed");
    check_pthread(pthread_mutex_init(&group->mu, NULL), "pthread_mutex_init");
    check_pthread(pthread_cond_init(&group->cv, NULL), "pthread_cond_init");

    for (size_t i = 0; i < count; i++)
    {
        group->contexts[i].group = group;
        if (snprintf(group->contexts[i].path,
                     sizeof(group->contexts[i].path),
                     "/lock-bench/resident-%zu", i) >=
            (int)sizeof(group->contexts[i].path))
            fail("resident path overflow");
        check_pthread(pthread_create(&group->threads[i], NULL,
                                     resident_main, &group->contexts[i]),
                      "pthread_create");
    }

    check_pthread(pthread_mutex_lock(&group->mu), "pthread_mutex_lock");
    while (group->ready != count)
        check_pthread(pthread_cond_wait(&group->cv, &group->mu),
                      "pthread_cond_wait");
    bool failed = group->failed;
    check_pthread(pthread_mutex_unlock(&group->mu), "pthread_mutex_unlock");
    if (failed)
        fail("resident lock acquisition failed");
}

static void stop_residents(struct resident_group *group)
{
    if (group->count == 0)
        return;

    check_pthread(pthread_mutex_lock(&group->mu), "pthread_mutex_lock");
    group->release = true;
    check_pthread(pthread_cond_broadcast(&group->cv), "pthread_cond_broadcast");
    check_pthread(pthread_mutex_unlock(&group->mu), "pthread_mutex_unlock");
    for (size_t i = 0; i < group->count; i++)
        check_pthread(pthread_join(group->threads[i], NULL), "pthread_join");
    check_pthread(pthread_cond_destroy(&group->cv), "pthread_cond_destroy");
    check_pthread(pthread_mutex_destroy(&group->mu), "pthread_mutex_destroy");
    free(group->contexts);
    free(group->threads);
}

enum throughput_phase
{
    PHASE_WARMUP,
    PHASE_MEASURE,
    PHASE_STOP,
};

struct throughput_context
{
    const struct backend *backend;
    size_t worker_index;
    pthread_barrier_t *ready;
    atomic_int *phase;
    atomic_int *failed;
    uint64_t operations;
};

static void *throughput_main(void *argument)
{
    struct throughput_context *context = argument;
    char paths[WORKER_PATH_COUNT][96];
    for (size_t i = 0; i < WORKER_PATH_COUNT; i++)
    {
        if (snprintf(paths[i], sizeof(paths[i]),
                     "/lock-bench/worker-%zu/path-%zu",
                     context->worker_index, i) >= (int)sizeof(paths[i]))
            atomic_store(context->failed, 1);
    }

    wait_at_barrier(context->ready);
    size_t path_index = 0;
    uint64_t operations = 0;
    for (;;)
    {
        int phase = atomic_load(context->phase);
        if (phase == PHASE_STOP || atomic_load(context->failed))
            break;
        void *lock = context->backend->lock(paths[path_index]);
        if (!lock)
        {
            atomic_store(context->failed, 1);
            break;
        }
        context->backend->unlock(lock);
        if (phase == PHASE_MEASURE &&
            atomic_load(context->phase) == PHASE_MEASURE)
            operations++;
        path_index = (path_index + 1) % WORKER_PATH_COUNT;
    }
    context->operations = operations;
    return NULL;
}

static void measure_throughput(const struct backend *backend, size_t workers,
                               uint64_t warmup_ms, uint64_t duration_ms,
                               uint64_t *operations, uint64_t *elapsed_ns)
{
    pthread_t *threads = calloc(workers, sizeof(*threads));
    struct throughput_context *contexts =
        calloc(workers, sizeof(*contexts));
    if (!threads || !contexts)
        fail("throughput allocation failed");

    pthread_barrier_t ready;
    check_pthread(pthread_barrier_init(&ready, NULL, (unsigned)workers + 1),
                  "pthread_barrier_init");
    atomic_int phase = PHASE_WARMUP;
    atomic_int failed = 0;
    for (size_t i = 0; i < workers; i++)
    {
        contexts[i] = (struct throughput_context){
            .backend = backend,
            .worker_index = i,
            .ready = &ready,
            .phase = &phase,
            .failed = &failed,
        };
        check_pthread(pthread_create(&threads[i], NULL,
                                     throughput_main, &contexts[i]),
                      "pthread_create");
    }

    wait_at_barrier(&ready);
    sleep_ms(warmup_ms);
    uint64_t start_ns = monotonic_ns();
    atomic_store(&phase, PHASE_MEASURE);
    sleep_ms(duration_ms);
    uint64_t end_ns = monotonic_ns();
    atomic_store(&phase, PHASE_STOP);

    *operations = 0;
    for (size_t i = 0; i < workers; i++)
    {
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
        *operations += contexts[i].operations;
    }
    *elapsed_ns = end_ns - start_ns;
    check_pthread(pthread_barrier_destroy(&ready), "pthread_barrier_destroy");
    free(contexts);
    free(threads);
    if (atomic_load(&failed))
        fail("throughput lock acquisition failed");
}

struct latency_context
{
    const struct backend *backend;
    size_t worker_index;
    size_t sample_count;
    uint64_t *samples;
    pthread_barrier_t *ready;
    atomic_int *failed;
};

static void *latency_main(void *argument)
{
    struct latency_context *context = argument;
    char paths[WORKER_PATH_COUNT][96];
    for (size_t i = 0; i < WORKER_PATH_COUNT; i++)
    {
        if (snprintf(paths[i], sizeof(paths[i]),
                     "/lock-bench/worker-%zu/path-%zu",
                     context->worker_index, i) >= (int)sizeof(paths[i]))
            atomic_store(context->failed, 1);
    }

    wait_at_barrier(context->ready);
    for (size_t i = 0; i < context->sample_count; i++)
    {
        uint64_t start_ns = monotonic_ns();
        void *lock = context->backend->lock(paths[i % WORKER_PATH_COUNT]);
        if (!lock)
        {
            atomic_store(context->failed, 1);
            return NULL;
        }
        context->backend->unlock(lock);
        context->samples[i] = monotonic_ns() - start_ns;
    }
    return NULL;
}

static int compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static size_t percentile_index(size_t count, size_t percentile)
{
    return (count * percentile + 99) / 100 - 1;
}

static void measure_latency(const struct backend *backend, size_t workers,
                            size_t samples_per_worker,
                            uint64_t *p50_ns, uint64_t *p99_ns)
{
    if (workers > SIZE_MAX / samples_per_worker)
        fail("latency sample count overflow");
    size_t sample_count = workers * samples_per_worker;
    uint64_t *samples = calloc(sample_count, sizeof(*samples));
    pthread_t *threads = calloc(workers, sizeof(*threads));
    struct latency_context *contexts = calloc(workers, sizeof(*contexts));
    if (!samples || !threads || !contexts)
        fail("latency allocation failed");

    pthread_barrier_t ready;
    check_pthread(pthread_barrier_init(&ready, NULL, (unsigned)workers + 1),
                  "pthread_barrier_init");
    atomic_int failed = 0;
    for (size_t i = 0; i < workers; i++)
    {
        contexts[i] = (struct latency_context){
            .backend = backend,
            .worker_index = i,
            .sample_count = samples_per_worker,
            .samples = samples + i * samples_per_worker,
            .ready = &ready,
            .failed = &failed,
        };
        check_pthread(pthread_create(&threads[i], NULL,
                                     latency_main, &contexts[i]),
                      "pthread_create");
    }
    wait_at_barrier(&ready);
    for (size_t i = 0; i < workers; i++)
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    check_pthread(pthread_barrier_destroy(&ready), "pthread_barrier_destroy");
    if (atomic_load(&failed))
        fail("latency lock acquisition failed");

    qsort(samples, sample_count, sizeof(*samples), compare_u64);
    *p50_ns = samples[percentile_index(sample_count, 50)];
    *p99_ns = samples[percentile_index(sample_count, 99)];
    free(contexts);
    free(threads);
    free(samples);
}

static struct benchmark_result run_backend(const struct backend *backend,
                                           size_t workers,
                                           size_t resident_paths,
                                           size_t repetition,
                                           const struct options *options)
{
    struct resident_group residents;
    start_residents(&residents, backend, resident_paths);

    struct benchmark_result result = {
        .implementation = backend->name,
        .workers = workers,
        .resident_paths = resident_paths,
        .repetition = repetition,
        .latency_samples = options->latency_samples,
    };
    measure_throughput(backend, workers, options->warmup_ms,
                       options->duration_ms, &result.operations,
                       &result.elapsed_ns);
    result.operations_per_second =
        (double)result.operations * 1e9 / (double)result.elapsed_ns;
    measure_latency(backend, workers, options->latency_samples,
                    &result.p50_ns, &result.p99_ns);

    stop_residents(&residents);
    backend->destroy();
    return result;
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median_double(double *values, size_t count)
{
    qsort(values, count, sizeof(*values), compare_double);
    if (count % 2 != 0)
        return values[count / 2];
    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static void collect_medians(const struct benchmark_result *results,
                            size_t result_count, size_t workers,
                            size_t resident_paths, const char *implementation,
                            size_t repetitions, double *throughput,
                            double *p50, double *p99)
{
    double *throughputs = calloc(repetitions, sizeof(*throughputs));
    double *p50_values = calloc(repetitions, sizeof(*p50_values));
    double *p99_values = calloc(repetitions, sizeof(*p99_values));
    if (!throughputs || !p50_values || !p99_values)
        fail("summary allocation failed");

    size_t found = 0;
    for (size_t i = 0; i < result_count; i++)
    {
        if (results[i].workers == workers &&
            results[i].resident_paths == resident_paths &&
            strcmp(results[i].implementation, implementation) == 0)
        {
            throughputs[found] = results[i].operations_per_second;
            p50_values[found] = (double)results[i].p50_ns;
            p99_values[found] = (double)results[i].p99_ns;
            found++;
        }
    }
    if (found != repetitions)
        fail("incomplete summary data");
    *throughput = median_double(throughputs, found);
    *p50 = median_double(p50_values, found);
    *p99 = median_double(p99_values, found);
    free(p99_values);
    free(p50_values);
    free(throughputs);
}

static void print_summary(const struct options *options,
                          const struct benchmark_result *results,
                          size_t result_count)
{
    puts("+---------+----------------+----------------+--------------+----------+----------+---------+");
    puts("| Workers | Resident paths | Implementation | Ops/sec      | p50 (ns) | p99 (ns) | Speedup |");
    puts("+---------+----------------+----------------+--------------+----------+----------+---------+");
    for (size_t worker_index = 0;
         worker_index < options->worker_count;
         worker_index++)
    {
        for (size_t resident_index = 0;
             resident_index < options->resident_count;
             resident_index++)
        {
            size_t workers = options->workers[worker_index];
            size_t resident_paths = options->resident_paths[resident_index];
            double linear_ops, linear_p50, linear_p99;
            double sharded_ops, sharded_p50, sharded_p99;
            collect_medians(results, result_count, workers, resident_paths,
                            "linear", options->repetitions,
                            &linear_ops, &linear_p50, &linear_p99);
            collect_medians(results, result_count, workers, resident_paths,
                            "sharded", options->repetitions,
                            &sharded_ops, &sharded_p50, &sharded_p99);
            printf("| %7zu | %14zu | %-14s | %12.0f | %8.0f | %8.0f | %7s |\n",
                   workers, resident_paths, "linear", linear_ops,
                   linear_p50, linear_p99, "-");
            char speedup[32];
            snprintf(speedup, sizeof(speedup), "%.2fx",
                     sharded_ops / linear_ops);
            printf("| %7zu | %14zu | %-14s | %12.0f | %8.0f | %8.0f | %7s |\n",
                   workers, resident_paths, "sharded", sharded_ops,
                   sharded_p50, sharded_p99, speedup);
        }
    }
    puts("+---------+----------------+----------------+--------------+----------+----------+---------+");
}

int main(int argc, char **argv)
{
    struct options options = parse_options(argc, argv);
    for (size_t i = 0; i < options.worker_count; i++)
    {
        if (options.workers[i] > UINT_MAX - 1)
            fail("worker count exceeds pthread barrier capacity");
    }
    if (options.worker_count > SIZE_MAX / options.resident_count ||
        options.worker_count * options.resident_count >
            SIZE_MAX / options.repetitions / 2)
        fail("result count overflow");

    size_t result_capacity = options.worker_count * options.resident_count *
                             options.repetitions * 2;
    struct benchmark_result *results =
        calloc(result_capacity, sizeof(*results));
    if (!results)
        fail("result allocation failed");

    puts("Lock-table linear-vs-sharded microbenchmark");
    puts("Resident holders pin one distinct path each; workers rotate 64 private paths");
    puts("Throughput and per-operation latency are measured in separate passes");
    puts("Defaults: workers=1,4,8,16; resident_paths=0,64,256; repetitions=7; warmup=100ms; throughput=500ms; latency_samples=5001/worker");
    puts("implementation,workers,resident_paths,repetition,operations,elapsed_ns,ops_per_sec,p50_ns,p99_ns,latency_samples");

    size_t result_count = 0;
    for (size_t worker_index = 0;
         worker_index < options.worker_count;
         worker_index++)
    {
        for (size_t resident_index = 0;
             resident_index < options.resident_count;
             resident_index++)
        {
            for (size_t repetition = 1;
                 repetition <= options.repetitions;
                 repetition++)
            {
                size_t first = repetition % 2 == 1 ? 0 : 1;
                for (size_t order = 0; order < 2; order++)
                {
                    const struct backend *backend = &backends[first ^ order];
                    struct benchmark_result result =
                        run_backend(backend,
                                    options.workers[worker_index],
                                    options.resident_paths[resident_index],
                                    repetition, &options);
                    results[result_count++] = result;
                    printf("%s,%zu,%zu,%zu,%" PRIu64 ",%" PRIu64
                           ",%.3f,%" PRIu64 ",%" PRIu64 ",%zu\n",
                           result.implementation, result.workers,
                           result.resident_paths, result.repetition,
                           result.operations, result.elapsed_ns,
                           result.operations_per_second, result.p50_ns,
                           result.p99_ns, result.latency_samples);
                    fflush(stdout);
                }
            }
        }
    }

    putchar('\n');
    print_summary(&options, results, result_count);
    free(results);
    return EXIT_SUCCESS;
}
