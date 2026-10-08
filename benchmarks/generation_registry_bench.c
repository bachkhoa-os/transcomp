#include "myfs.h"

#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#ifndef MYFS_BENCH_TREE_HASH
#define MYFS_BENCH_TREE_HASH "unknown"
#endif

enum
{
    DEFAULT_REPETITIONS = 7,
    DEFAULT_WARMUP = 128,
    DEFAULT_ITERATIONS = 4000,
    DEFAULT_THRESHOLD_US = 1000,
    DEFAULT_SYNTHETIC_DELAY_US = 250,
    MAX_THREAD_VALUES = 32,
    CONTROL_STEPS = 64,
};

typedef enum
{
    WORKLOAD_CONTROL,
    WORKLOAD_SCALABILITY,
    WORKLOAD_HOT_PATH,
    WORKLOAD_GC_INTERFERENCE,
} workload_kind_t;

typedef enum
{
    IO_MODE_NONE,
    IO_MODE_REAL,
    IO_MODE_SYNTHETIC_DELAY,
} io_mode_t;

struct options
{
    char workdir[PATH_MAX];
    char cpu_text[256];
    size_t threads[MAX_THREAD_VALUES];
    size_t thread_count;
    size_t repetitions;
    size_t warmup;
    size_t iterations;
    size_t gc_threads;
    uint64_t threshold_us;
    uint64_t synthetic_delay_us;
    uint64_t seed;
    bool cpu_list_given;
    bool aa;
    bool short_run;
    bool run_real;
    bool run_synthetic;
    bool noise_floor_given;
    double noise_floor_pct;
};

struct measurement
{
    uint64_t operations;
    uint64_t elapsed_ns;
    double operations_per_second;
    double per_thread_operations_per_second;
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t p999_ns;
    uint64_t max_ns;
    double delayed_fraction;
    uint64_t gc_cycles;
    double gc_cycles_per_second;
    bool self_check;
};

struct summary
{
    workload_kind_t workload;
    io_mode_t io_mode;
    size_t threads;
    size_t gc_threads;
    size_t repetitions;
    uint64_t operations;
    double median_ops;
    double min_ops;
    double max_ops;
    double median_per_thread_ops;
    uint64_t median_p50_ns;
    uint64_t median_p99_ns;
    uint64_t median_p999_ns;
    uint64_t max_ns;
    double median_delayed_fraction;
    double median_gc_cycles_per_second;
    uint64_t min_gc_cycles;
    double aa_median_spread_pct;
    double aa_max_spread_pct;
    double noise_floor_pct;
    const char *win_gate;
    bool self_check;
};

struct gc_io_gate
{
    atomic_size_t io_entered;
    atomic_size_t foreground_ready;
    atomic_bool release_io;
    atomic_bool timed_out;
};

static atomic_uint_fast64_t control_sink;
static atomic_uint_fast64_t synthetic_delay_ns;
static atomic_uint_fast64_t synthetic_delay_calls;
static _Thread_local bool inside_measured_gc;
static _Thread_local bool announced_measured_gc_io;
static _Thread_local struct gc_io_gate *measured_gc_gate;

static void failf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("generation registry benchmark: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(EXIT_FAILURE);
}

static void check_pthread(int status, const char *operation)
{
    if (status != 0)
        failf("%s: %s", operation, strerror(status));
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        failf("clock_gettime: %s", strerror(errno));
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void measured_gc_before_io(void)
{
    if (!inside_measured_gc)
        return;
    if (!announced_measured_gc_io)
    {
        announced_measured_gc_io = true;
        atomic_fetch_add_explicit(&measured_gc_gate->io_entered, 1,
                                  memory_order_release);
        uint64_t deadline = monotonic_ns() + UINT64_C(10000000000);
        while (!atomic_load_explicit(&measured_gc_gate->release_io,
                                     memory_order_acquire))
        {
            if (monotonic_ns() >= deadline)
            {
                atomic_store(&measured_gc_gate->timed_out, true);
                atomic_store_explicit(&measured_gc_gate->release_io, true,
                                      memory_order_release);
                break;
            }
            sched_yield();
        }
    }
    uint64_t delay_ns = atomic_load_explicit(&synthetic_delay_ns,
                                             memory_order_relaxed);
    if (delay_ns == 0)
        return;
    atomic_fetch_add_explicit(&synthetic_delay_calls, 1,
                              memory_order_relaxed);
    struct timespec delay = {
        .tv_sec = (time_t)(delay_ns / UINT64_C(1000000000)),
        .tv_nsec = (long)(delay_ns % UINT64_C(1000000000)),
    };
    while (nanosleep(&delay, &delay) != 0)
    {
        if (errno != EINTR)
            failf("nanosleep: %s", strerror(errno));
    }
}

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    measured_gc_before_io();
    return __real_fsync(fd);
}

int __real_rename(const char *old_path, const char *new_path);
int __wrap_rename(const char *old_path, const char *new_path)
{
    measured_gc_before_io();
    return __real_rename(old_path, new_path);
}

int __real_unlink(const char *path);
int __wrap_unlink(const char *path)
{
    measured_gc_before_io();
    return __real_unlink(path);
}

int __real_rmdir(const char *path);
int __wrap_rmdir(const char *path)
{
    measured_gc_before_io();
    return __real_rmdir(path);
}

ssize_t __real_readlink(const char *path, char *buffer, size_t size);
ssize_t __wrap_readlink(const char *path, char *buffer, size_t size)
{
    measured_gc_before_io();
    return __real_readlink(path, buffer, size);
}

int __real_lstat(const char *path, struct stat *status);
int __wrap_lstat(const char *path, struct stat *status)
{
    measured_gc_before_io();
    return __real_lstat(path, status);
}

int __real_link(const char *old_path, const char *new_path);
int __wrap_link(const char *old_path, const char *new_path)
{
    measured_gc_before_io();
    return __real_link(old_path, new_path);
}

int __real_symlink(const char *target, const char *path);
int __wrap_symlink(const char *target, const char *path)
{
    measured_gc_before_io();
    return __real_symlink(target, path);
}

static size_t parse_size(const char *text, const char *option,
                         bool allow_zero)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || end == text || *end != '\0' ||
        value > SIZE_MAX || (!allow_zero && value == 0))
        failf("invalid value for %s: %s", option, text);
    return (size_t)value;
}

static uint64_t parse_u64(const char *text, const char *option,
                          bool allow_zero)
{
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || end == text || *end != '\0' ||
        (!allow_zero && value == 0))
        failf("invalid value for %s: %s", option, text);
    return (uint64_t)value;
}

static double parse_nonnegative_double(const char *text, const char *option)
{
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || end == text || *end != '\0' ||
        value < 0.0 || value > 1000000.0)
        failf("invalid value for %s: %s", option, text);
    return value;
}

static void parse_size_list(const char *text, const char *option,
                            size_t values[MAX_THREAD_VALUES],
                            size_t *count)
{
    char *copy = strdup(text);
    if (!copy)
        failf("allocation failed");
    *count = 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save); part;
         part = strtok_r(NULL, ",", &save))
    {
        if (*count == MAX_THREAD_VALUES)
            failf("too many values for %s", option);
        values[(*count)++] = parse_size(part, option, false);
    }
    free(copy);
    if (*count == 0)
        failf("empty value list for %s", option);
}

static void usage(const char *program)
{
    printf("Usage: %s --workdir DIR [options]\n", program);
    puts("  --cpus LIST                 pin to an explicit CPU list/ranges");
    puts("  --threads LIST              worker counts (default 1,2,4,8,16)");
    puts("  --repetitions N             repetitions (default 7)");
    puts("  --warmup N                  warm-up operations/worker (default 128)");
    puts("  --iterations N              measured operations/worker (default 4000)");
    puts("  --gc-threads N              concurrent GC workers (default 1)");
    puts("  --threshold-us N            delayed-op threshold (default 1000)");
    puts("  --synthetic-delay-us N      delay at wrapped GC I/O calls (default 250)");
    puts("  --gc-mode MODE              real, synthetic, or both (default both)");
    puts("  --seed N                    deterministic seed (default 25228)");
    puts("  --aa                        paired same-binary A/A measurements");
    puts("  --noise-floor-pct P         previously observed A/A gate");
    puts("  --short                     short output/TSan configuration");
}

static struct options parse_options(int argc, char **argv)
{
    struct options options = {
        .threads = {1, 2, 4, 8, 16},
        .thread_count = 5,
        .repetitions = DEFAULT_REPETITIONS,
        .warmup = DEFAULT_WARMUP,
        .iterations = DEFAULT_ITERATIONS,
        .gc_threads = 1,
        .threshold_us = DEFAULT_THRESHOLD_US,
        .synthetic_delay_us = DEFAULT_SYNTHETIC_DELAY_US,
        .seed = 25228,
        .run_real = true,
        .run_synthetic = true,
    };
    static const struct option long_options[] = {
        {"workdir", required_argument, NULL, 'w'},
        {"cpus", required_argument, NULL, 'c'},
        {"threads", required_argument, NULL, 't'},
        {"repetitions", required_argument, NULL, 'r'},
        {"warmup", required_argument, NULL, 'u'},
        {"iterations", required_argument, NULL, 'i'},
        {"gc-threads", required_argument, NULL, 'g'},
        {"threshold-us", required_argument, NULL, 'd'},
        {"synthetic-delay-us", required_argument, NULL, 's'},
        {"gc-mode", required_argument, NULL, 'm'},
        {"seed", required_argument, NULL, 'e'},
        {"aa", no_argument, NULL, 'a'},
        {"noise-floor-pct", required_argument, NULL, 'n'},
        {"short", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    for (;;)
    {
        int option = getopt_long(argc, argv, "", long_options, NULL);
        if (option == -1)
            break;
        switch (option)
        {
        case 'w':
            if (snprintf(options.workdir, sizeof(options.workdir), "%s",
                         optarg) >= (int)sizeof(options.workdir))
                failf("--workdir path is too long");
            break;
        case 'c':
            if (snprintf(options.cpu_text, sizeof(options.cpu_text), "%s",
                         optarg) >= (int)sizeof(options.cpu_text))
                failf("--cpus value is too long");
            options.cpu_list_given = true;
            break;
        case 't':
            parse_size_list(optarg, "--threads", options.threads,
                            &options.thread_count);
            break;
        case 'r':
            options.repetitions = parse_size(optarg, "--repetitions", false);
            break;
        case 'u':
            options.warmup = parse_size(optarg, "--warmup", true);
            break;
        case 'i':
            options.iterations = parse_size(optarg, "--iterations", false);
            break;
        case 'g':
            options.gc_threads = parse_size(optarg, "--gc-threads", false);
            break;
        case 'd':
            options.threshold_us = parse_u64(optarg, "--threshold-us", false);
            break;
        case 's':
            options.synthetic_delay_us = parse_u64(
                optarg, "--synthetic-delay-us", false);
            break;
        case 'm':
            if (strcmp(optarg, "real") == 0)
            {
                options.run_real = true;
                options.run_synthetic = false;
            }
            else if (strcmp(optarg, "synthetic") == 0)
            {
                options.run_real = false;
                options.run_synthetic = true;
            }
            else if (strcmp(optarg, "both") == 0)
            {
                options.run_real = true;
                options.run_synthetic = true;
            }
            else
                failf("invalid --gc-mode: %s", optarg);
            break;
        case 'e':
            options.seed = parse_u64(optarg, "--seed", true);
            break;
        case 'a':
            options.aa = true;
            break;
        case 'n':
            options.noise_floor_pct = parse_nonnegative_double(
                optarg, "--noise-floor-pct");
            options.noise_floor_given = true;
            break;
        case 'q':
            options.short_run = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    if (optind != argc)
        failf("unexpected positional argument: %s", argv[optind]);
    if (options.workdir[0] == '\0')
        failf("--workdir DIR is required");
    if (options.short_run)
    {
        options.threads[0] = 1;
        options.threads[1] = 2;
        options.thread_count = 2;
        options.repetitions = 1;
        options.warmup = 16;
        options.iterations = 5000;
        options.gc_threads = 1;
        options.synthetic_delay_us = 50;
        options.run_real = true;
        options.run_synthetic = true;
    }
    for (size_t i = 0; i < options.thread_count; i++)
    {
        if (options.threads[i] > 256)
            failf("thread count %zu exceeds benchmark limit 256",
                  options.threads[i]);
    }
    if (options.gc_threads > 64)
        failf("GC thread count exceeds benchmark limit 64");
    return options;
}

static void add_cpu_range(cpu_set_t *set, const char *part)
{
    errno = 0;
    char *end = NULL;
    unsigned long first = strtoul(part, &end, 10);
    if (errno != 0 || end == part || first >= CPU_SETSIZE)
        failf("invalid CPU list element: %s", part);
    unsigned long last = first;
    if (*end == '-')
    {
        const char *last_text = end + 1;
        errno = 0;
        last = strtoul(last_text, &end, 10);
        if (errno != 0 || end == last_text || last < first ||
            last >= CPU_SETSIZE)
            failf("invalid CPU range: %s", part);
    }
    if (*end != '\0')
        failf("invalid CPU list element: %s", part);
    for (unsigned long cpu = first; cpu <= last; cpu++)
        CPU_SET((int)cpu, set);
}

static void parse_cpu_set(const char *text, cpu_set_t *set)
{
    CPU_ZERO(set);
    char *copy = strdup(text);
    if (!copy)
        failf("allocation failed");
    char *save = NULL;
    for (char *part = strtok_r(copy, ",", &save); part;
         part = strtok_r(NULL, ",", &save))
        add_cpu_range(set, part);
    free(copy);
    if (CPU_COUNT(set) == 0)
        failf("--cpus cannot be empty");
}

static void format_cpu_set(const cpu_set_t *set, char *output,
                           size_t output_size)
{
    size_t used = 0;
    bool first = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
    {
        if (!CPU_ISSET(cpu, set))
            continue;
        int written = snprintf(output + used, output_size - used,
                               "%s%d", first ? "" : ";", cpu);
        if (written < 0 || (size_t)written >= output_size - used)
            failf("effective CPU list is too long");
        used += (size_t)written;
        first = false;
    }
    if (first)
        failf("effective CPU affinity is empty");
}

static void configure_affinity(const struct options *options,
                               char effective_text[1024])
{
    if (options->cpu_list_given)
    {
        cpu_set_t requested;
        parse_cpu_set(options->cpu_text, &requested);
        if (sched_setaffinity(0, sizeof(requested), &requested) != 0)
            failf("sched_setaffinity(%s): %s", options->cpu_text,
                  strerror(errno));
    }
    else
        fprintf(stderr, "WARNING: benchmark is unpinned; results may be noisy\n");

    cpu_set_t effective;
    if (sched_getaffinity(0, sizeof(effective), &effective) != 0)
        failf("sched_getaffinity: %s", strerror(errno));
    format_cpu_set(&effective, effective_text, 1024);
}

static void wait_at_barrier(pthread_barrier_t *barrier)
{
    int status = pthread_barrier_wait(barrier);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
        check_pthread(status, "pthread_barrier_wait");
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

static uint64_t percentile(const uint64_t *sorted, size_t count,
                           unsigned numerator, unsigned denominator)
{
    if (count == 0)
        return 0;
    size_t rank = (count * numerator + denominator - 1) / denominator;
    if (rank == 0)
        rank = 1;
    if (rank > count)
        rank = count;
    return sorted[rank - 1];
}

static double median_double(const double *values, size_t count)
{
    double *copy = malloc(count * sizeof(*copy));
    if (!copy)
        failf("allocation failed");
    memcpy(copy, values, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_double);
    double result = count % 2 == 0
        ? (copy[count / 2 - 1] + copy[count / 2]) / 2.0
        : copy[count / 2];
    free(copy);
    return result;
}

static uint64_t median_u64(const uint64_t *values, size_t count)
{
    uint64_t *copy = malloc(count * sizeof(*copy));
    if (!copy)
        failf("allocation failed");
    memcpy(copy, values, count * sizeof(*copy));
    qsort(copy, count, sizeof(*copy), compare_u64);
    uint64_t result = count % 2 == 0
        ? copy[count / 2 - 1] +
              (copy[count / 2] - copy[count / 2 - 1]) / 2
        : copy[count / 2];
    free(copy);
    return result;
}

static const char *workload_name(workload_kind_t workload)
{
    switch (workload)
    {
    case WORKLOAD_CONTROL:
        return "control";
    case WORKLOAD_SCALABILITY:
        return "scalability";
    case WORKLOAD_HOT_PATH:
        return "hot-path";
    case WORKLOAD_GC_INTERFERENCE:
        return "gc-interference";
    }
    return "unknown";
}

static const char *io_mode_name(io_mode_t mode)
{
    switch (mode)
    {
    case IO_MODE_NONE:
        return "none";
    case IO_MODE_REAL:
        return "real";
    case IO_MODE_SYNTHETIC_DELAY:
        return "synthetic-delay";
    }
    return "unknown";
}

static void make_fake_storage(myfs_storage_t *storage, const char *path,
                              uint64_t seed, size_t worker)
{
    memset(storage, 0, sizeof(*storage));
    if (snprintf(storage->logical_path, sizeof(storage->logical_path),
                 "%s", path) >= (int)sizeof(storage->logical_path))
        failf("logical path too long");
    uint64_t first = seed ^ (UINT64_C(0x9e3779b97f4a7c15) * (worker + 1));
    uint64_t second = first ^ UINT64_C(0xd1b54a32d192ed03);
    if (snprintf(storage->generation_id, sizeof(storage->generation_id),
                 "%016" PRIx64 "%016" PRIx64, first, second) !=
        MYFS_GENERATION_HEX_LEN)
        failf("generation id formatting failed");
    storage->is_legacy = false;
    if (snprintf(storage->data_path, sizeof(storage->data_path),
                 "/unused%s.%zu", path, worker) >=
        (int)sizeof(storage->data_path))
        failf("fake data path too long");
}

static void initialize_handle(myfs_file_handle_t *handle,
                              const myfs_storage_t *storage)
{
    memset(handle, 0, sizeof(*handle));
    handle->data_fd = -1;
    handle->meta_fd = -1;
    handle->flags = O_RDWR;
    handle->storage = *storage;
    check_pthread(pthread_rwlock_init(&handle->cache_lock, NULL),
                  "pthread_rwlock_init");
}

static bool registry_transaction(myfs_file_handle_t *handle)
{
    myfs_file_lock_t *path_lock = myfs_lock_file(
        handle->storage.logical_path);
    if (!path_lock)
        return false;
    bool ok = register_generation_handle_locked(handle) == 0;
    uint64_t epoch = 0;
    bool superseded = false;
    if (ok)
    {
        check_pthread(pthread_rwlock_rdlock(&handle->cache_lock),
                      "pthread_rwlock_rdlock");
        generation_state_snapshot(handle, &epoch, &superseded);
        check_pthread(pthread_rwlock_unlock(&handle->cache_lock),
                      "pthread_rwlock_unlock");
        check_pthread(pthread_rwlock_wrlock(&handle->cache_lock),
                      "pthread_rwlock_wrlock");
        uint64_t bumped = generation_bump_metadata_epoch_locked(handle);
        unregister_generation_handle_locked(handle);
        check_pthread(pthread_rwlock_unlock(&handle->cache_lock),
                      "pthread_rwlock_unlock");
        ok = !superseded && epoch != 0 && bumped == epoch + 1 &&
             handle->generation_record == NULL &&
             handle->registry_prev == NULL && handle->registry_next == NULL;
    }
    myfs_unlock_file(path_lock);
    return ok;
}

static uint64_t control_operation(uint64_t state)
{
    for (unsigned step = 0; step < CONTROL_STEPS; step++)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
    }
    return state;
}

struct basic_group;

struct basic_worker
{
    struct basic_group *group;
    size_t index;
    myfs_file_handle_t handle;
    uint64_t control_state;
};

struct basic_group
{
    workload_kind_t workload;
    size_t threads;
    size_t warmup;
    size_t iterations;
    uint64_t threshold_ns;
    pthread_barrier_t ready;
    pthread_barrier_t start;
    uint64_t *latencies;
    atomic_uint_fast64_t operations;
    atomic_bool failed;
};

static bool basic_operation(struct basic_worker *worker)
{
    if (worker->group->workload == WORKLOAD_CONTROL)
    {
        worker->control_state = control_operation(worker->control_state);
        return true;
    }
    return registry_transaction(&worker->handle);
}

static void *basic_worker_main(void *argument)
{
    struct basic_worker *worker = argument;
    struct basic_group *group = worker->group;
    for (size_t i = 0; i < group->warmup; i++)
    {
        if (!basic_operation(worker))
            atomic_store(&group->failed, true);
    }
    wait_at_barrier(&group->ready);
    wait_at_barrier(&group->start);
    uint64_t *samples = group->latencies + worker->index * group->iterations;
    uint64_t completed = 0;
    for (size_t i = 0; i < group->iterations; i++)
    {
        uint64_t start_ns = monotonic_ns();
        bool ok = basic_operation(worker);
        samples[i] = monotonic_ns() - start_ns;
        if (!ok)
            atomic_store(&group->failed, true);
        else
            completed++;
    }
    atomic_fetch_add(&group->operations, completed);
    atomic_fetch_xor(&control_sink, worker->control_state);
    return NULL;
}

static void calculate_latency(struct measurement *measurement,
                              uint64_t *latencies, size_t count,
                              uint64_t threshold_ns)
{
    qsort(latencies, count, sizeof(*latencies), compare_u64);
    measurement->p50_ns = percentile(latencies, count, 50, 100);
    measurement->p99_ns = percentile(latencies, count, 99, 100);
    measurement->p999_ns = percentile(latencies, count, 999, 1000);
    measurement->max_ns = count ? latencies[count - 1] : 0;
    size_t delayed = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (latencies[i] > threshold_ns)
            delayed++;
    }
    measurement->delayed_fraction = count
        ? (double)delayed / (double)count : 0.0;
}

static bool verify_handle_reclaimed(struct basic_worker *worker)
{
    if (worker->group->workload == WORKLOAD_CONTROL)
        return true;
    return worker->handle.generation_record == NULL &&
           worker->handle.registry_prev == NULL &&
           worker->handle.registry_next == NULL &&
           generation_open_refs_locked(&worker->handle.storage) == 0 &&
           generation_writer_refs_locked(&worker->handle.storage) == 0;
}

static struct measurement run_basic_measurement(
    workload_kind_t workload, size_t threads, const struct options *options,
    size_t repetition, unsigned arm)
{
    destroy_generation_registry();
    destroy_lock_table();
    struct measurement measurement = {0};
    struct basic_group group = {
        .workload = workload,
        .threads = threads,
        .warmup = options->warmup,
        .iterations = options->iterations,
        .threshold_ns = options->threshold_us * UINT64_C(1000),
    };
    size_t sample_count = threads * options->iterations;
    group.latencies = calloc(sample_count, sizeof(*group.latencies));
    struct basic_worker *workers = calloc(threads, sizeof(*workers));
    pthread_t *thread_ids = calloc(threads, sizeof(*thread_ids));
    if (!group.latencies || !workers || !thread_ids)
        failf("allocation failed");
    check_pthread(pthread_barrier_init(&group.ready, NULL,
                                      (unsigned)threads + 1),
                  "pthread_barrier_init");
    check_pthread(pthread_barrier_init(&group.start, NULL,
                                      (unsigned)threads + 1),
                  "pthread_barrier_init");

    for (size_t i = 0; i < threads; i++)
    {
        workers[i].group = &group;
        workers[i].index = i;
        workers[i].control_state = options->seed ^
            (UINT64_C(0xa0761d6478bd642f) * (i + 1)) ^
            (UINT64_C(0xe7037ed1a0b428db) * (repetition + 1)) ^ arm;
        if (workload != WORKLOAD_CONTROL)
        {
            char path[128];
            if (workload == WORKLOAD_HOT_PATH)
                snprintf(path, sizeof(path), "/bench-hot");
            else
                snprintf(path, sizeof(path), "/bench-scale-%zu", i);
            myfs_storage_t storage;
            make_fake_storage(&storage, path, options->seed, i);
            initialize_handle(&workers[i].handle, &storage);
        }
        check_pthread(pthread_create(&thread_ids[i], NULL, basic_worker_main,
                                     &workers[i]),
                      "pthread_create");
    }

    wait_at_barrier(&group.ready);
    uint64_t start_ns = monotonic_ns();
    wait_at_barrier(&group.start);
    for (size_t i = 0; i < threads; i++)
        check_pthread(pthread_join(thread_ids[i], NULL), "pthread_join");
    measurement.elapsed_ns = monotonic_ns() - start_ns;
    measurement.operations = atomic_load(&group.operations);
    uint64_t expected = (uint64_t)threads * options->iterations;
    bool reclaimed = true;
    for (size_t i = 0; i < threads; i++)
    {
        reclaimed = verify_handle_reclaimed(&workers[i]) && reclaimed;
        if (workload != WORKLOAD_CONTROL)
            check_pthread(pthread_rwlock_destroy(&workers[i].handle.cache_lock),
                          "pthread_rwlock_destroy");
    }
    measurement.self_check = !atomic_load(&group.failed) &&
        measurement.operations == expected && reclaimed;
    measurement.operations_per_second = measurement.elapsed_ns
        ? (double)measurement.operations * 1e9 / (double)measurement.elapsed_ns
        : 0.0;
    measurement.per_thread_operations_per_second =
        measurement.operations_per_second / (double)threads;
    calculate_latency(&measurement, group.latencies, sample_count,
                      group.threshold_ns);

    check_pthread(pthread_barrier_destroy(&group.start),
                  "pthread_barrier_destroy");
    check_pthread(pthread_barrier_destroy(&group.ready),
                  "pthread_barrier_destroy");
    free(thread_ids);
    free(workers);
    free(group.latencies);
    destroy_generation_registry();
    destroy_lock_table();
    if (!measurement.self_check)
        failf("self-check failed for %s/%zu", workload_name(workload), threads);
    return measurement;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const char *bytes = buffer;
    size_t done = 0;
    while (done < size)
    {
        ssize_t written = write(fd, bytes + done, size - done);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (written == 0)
            return -EIO;
        done += (size_t)written;
    }
    return 0;
}

static int create_fixture_generation(const char *path,
                                     myfs_storage_t *storage)
{
    int ret = create_generation_storage(path, 0600, storage);
    if (ret != 0)
        return ret;
    static const char data_payload[] = "generation-registry-benchmark-data";
    static const char meta_payload[] = "generation-registry-benchmark-meta";
    int data_fd = open(storage->data_path, O_WRONLY | O_CLOEXEC);
    if (data_fd < 0)
        ret = -errno;
    if (ret == 0)
        ret = write_all(data_fd, data_payload, sizeof(data_payload));
    if (ret == 0 && fsync(data_fd) != 0)
        ret = -errno;
    if (data_fd >= 0 && close(data_fd) != 0 && ret == 0)
        ret = -errno;

    int meta_fd = -1;
    if (ret == 0)
    {
        meta_fd = open(storage->meta_path,
                       O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (meta_fd < 0)
            ret = -errno;
    }
    if (ret == 0)
        ret = write_all(meta_fd, meta_payload, sizeof(meta_payload));
    if (ret == 0 && fsync(meta_fd) != 0)
        ret = -errno;
    if (meta_fd >= 0 && close(meta_fd) != 0 && ret == 0)
        ret = -errno;
    if (ret == 0)
        ret = fsync_parent_path(storage->meta_path);
    if (ret != 0)
        remove_generation_storage(storage);
    return ret;
}

static bool same_inode(const char *left, const char *right)
{
    struct stat left_status;
    struct stat right_status;
    return stat(left, &left_status) == 0 && stat(right, &right_status) == 0 &&
           left_status.st_dev == right_status.st_dev &&
           left_status.st_ino == right_status.st_ino;
}

static bool storage_retired(const myfs_storage_t *storage)
{
    errno = 0;
    bool data_gone = lstat(storage->data_path, &(struct stat){0}) != 0 &&
        errno == ENOENT;
    errno = 0;
    bool meta_gone = lstat(storage->meta_path, &(struct stat){0}) != 0 &&
        errno == ENOENT;
    errno = 0;
    bool directory_gone = lstat(storage->generation_dir,
                                &(struct stat){0}) != 0 && errno == ENOENT;
    errno = 0;
    bool marker_gone = lstat(storage->marker_path,
                             &(struct stat){0}) != 0 && errno == ENOENT;
    return data_gone && meta_gone && directory_gone && marker_gone;
}

static bool active_aliases_match(const char *path,
                                 const myfs_storage_t *active)
{
    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_data_path(data_alias, path);
    build_meta_path(meta_alias, path);
    return same_inode(data_alias, active->data_path) &&
           same_inode(meta_alias, active->meta_path);
}

static int initialize_gc_path(const char *path, myfs_storage_t *active)
{
    myfs_file_lock_t *path_lock = myfs_lock_file(path);
    if (!path_lock)
        return -ENOMEM;
    int ret = create_fixture_generation(path, active);
    if (ret == 0)
        ret = publish_generation(path, active);
    myfs_unlock_file(path_lock);
    return ret;
}

static int prepare_successor(const char *path, myfs_storage_t *successor)
{
    myfs_file_lock_t *path_lock = myfs_lock_file(path);
    if (!path_lock)
        return -ENOMEM;
    int ret = create_fixture_generation(path, successor);
    myfs_unlock_file(path_lock);
    return ret;
}

static int rotate_generation(const char *path, myfs_storage_t *active,
                             myfs_storage_t *successor,
                             struct gc_io_gate *io_gate)
{
    myfs_storage_t victim = *active;
    myfs_file_lock_t *path_lock = myfs_lock_file(path);
    if (!path_lock)
        return -ENOMEM;
    int ret = publish_generation(path, successor);
    if (ret == 0)
        ret = mark_generation_for_gc_locked(&victim, true);
    if (ret == 0)
    {
        measured_gc_gate = io_gate;
        announced_measured_gc_io = false;
        inside_measured_gc = true;
        ret = run_generation_gc_locked(path);
        inside_measured_gc = false;
        measured_gc_gate = NULL;
    }
    myfs_unlock_file(path_lock);
    if (ret != 0)
        return ret;
    myfs_storage_t resolved;
    if (resolve_storage(path, &resolved) != 0 ||
        !storage_generation_equal(&resolved, successor) ||
        !storage_retired(&victim) ||
        !active_aliases_match(path, successor) ||
        generation_open_refs_locked(&victim) != 0 ||
        generation_writer_refs_locked(&victim) != 0)
        return -EIO;
    *active = *successor;
    memset(successor, 0, sizeof(*successor));
    return 0;
}

static void unlink_if_present(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT)
        failf("unlink(%s): %s", path, strerror(errno));
}

static bool cleanup_gc_path(const char *path, myfs_storage_t *active,
                            myfs_storage_t *prepared)
{
    if (prepared->generation_dir[0] != '\0')
        remove_generation_storage(prepared);
    char current[PATH_MAX];
    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_current_path(current, path);
    build_data_path(data_alias, path);
    build_meta_path(meta_alias, path);
    unlink_if_present(current);
    unlink_if_present(data_alias);
    unlink_if_present(meta_alias);
    int ret = active->generation_dir[0] == '\0'
        ? 0 : remove_generation_storage(active);
    if (current[0] != '\0')
        (void)fsync_parent_path(current);
    return ret == 0;
}

struct gc_group;

struct gc_worker
{
    struct gc_group *group;
    size_t index;
    char path[128];
    myfs_storage_t active;
    myfs_storage_t prepared;
    pthread_t thread;
};

struct gc_foreground
{
    struct gc_group *group;
    size_t index;
    myfs_file_handle_t handle;
    pthread_t thread;
};

struct gc_group
{
    const struct options *options;
    io_mode_t io_mode;
    size_t foreground_threads;
    size_t gc_thread_count;
    pthread_barrier_t ready;
    pthread_barrier_t start;
    uint64_t *latencies;
    atomic_uint_fast64_t operations;
    atomic_uint_fast64_t cycles;
    atomic_uint_fast64_t foreground_start_ns;
    struct gc_io_gate io_gate;
    atomic_bool stop;
    atomic_bool failed;
};

static bool register_persistent_handle(struct gc_foreground *foreground,
                                       uint64_t seed)
{
    char path[128];
    snprintf(path, sizeof(path), "/bench-fg-%zu", foreground->index);
    myfs_storage_t storage;
    make_fake_storage(&storage, path, seed, foreground->index + 1000);
    initialize_handle(&foreground->handle, &storage);
    myfs_file_lock_t *path_lock = myfs_lock_file(path);
    if (!path_lock)
        return false;
    bool ok = register_generation_handle_locked(&foreground->handle) == 0;
    myfs_unlock_file(path_lock);
    return ok;
}

static bool persistent_registry_operation(struct gc_foreground *foreground)
{
    myfs_file_handle_t *handle = &foreground->handle;
    uint64_t epoch = 0;
    bool superseded = false;
    check_pthread(pthread_rwlock_rdlock(&handle->cache_lock),
                  "pthread_rwlock_rdlock");
    generation_state_snapshot(handle, &epoch, &superseded);
    check_pthread(pthread_rwlock_unlock(&handle->cache_lock),
                  "pthread_rwlock_unlock");
    myfs_file_lock_t *path_lock = myfs_lock_file(handle->storage.logical_path);
    if (!path_lock)
        return false;
    check_pthread(pthread_rwlock_wrlock(&handle->cache_lock),
                  "pthread_rwlock_wrlock");
    uint64_t bumped = generation_bump_metadata_epoch_locked(handle);
    check_pthread(pthread_rwlock_unlock(&handle->cache_lock),
                  "pthread_rwlock_unlock");
    myfs_unlock_file(path_lock);
    return !superseded && epoch != 0 && bumped == epoch + 1;
}

static bool wait_for_counter(atomic_size_t *counter, size_t target,
                             struct gc_io_gate *gate)
{
    uint64_t deadline = monotonic_ns() + UINT64_C(10000000000);
    while (atomic_load_explicit(counter, memory_order_acquire) < target)
    {
        if (atomic_load(&gate->timed_out))
            return false;
        if (monotonic_ns() >= deadline)
        {
            atomic_store(&gate->timed_out, true);
            atomic_store_explicit(&gate->release_io, true,
                                  memory_order_release);
            return false;
        }
        sched_yield();
    }
    return true;
}

static void *gc_foreground_main(void *argument)
{
    struct gc_foreground *foreground = argument;
    struct gc_group *group = foreground->group;
    for (size_t i = 0; i < group->options->warmup; i++)
    {
        if (!persistent_registry_operation(foreground))
            atomic_store(&group->failed, true);
    }
    wait_at_barrier(&group->ready);
    wait_at_barrier(&group->start);
    if (!wait_for_counter(&group->io_gate.io_entered, 1,
                          &group->io_gate))
    {
        atomic_store(&group->failed, true);
        return NULL;
    }
    atomic_fetch_add_explicit(&group->io_gate.foreground_ready, 1,
                              memory_order_release);
    if (!wait_for_counter(&group->io_gate.foreground_ready,
                          group->foreground_threads, &group->io_gate))
    {
        atomic_store(&group->failed, true);
        return NULL;
    }
    uint_fast64_t expected_start = 0;
    (void)atomic_compare_exchange_strong(&group->foreground_start_ns,
                                         &expected_start, monotonic_ns());
    atomic_store_explicit(&group->io_gate.release_io, true,
                          memory_order_release);
    uint64_t *samples = group->latencies +
        foreground->index * group->options->iterations;
    uint64_t completed = 0;
    for (size_t i = 0; i < group->options->iterations; i++)
    {
        uint64_t start_ns = monotonic_ns();
        bool ok = persistent_registry_operation(foreground);
        samples[i] = monotonic_ns() - start_ns;
        if (!ok)
            atomic_store(&group->failed, true);
        else
            completed++;
    }
    atomic_fetch_add(&group->operations, completed);
    return NULL;
}

static void *gc_worker_main(void *argument)
{
    struct gc_worker *worker = argument;
    struct gc_group *group = worker->group;
    int ret = prepare_successor(worker->path, &worker->prepared);
    if (ret != 0)
        atomic_store(&group->failed, true);
    wait_at_barrier(&group->ready);
    wait_at_barrier(&group->start);
    while (ret == 0)
    {
        ret = rotate_generation(worker->path, &worker->active,
                                &worker->prepared, &group->io_gate);
        if (ret != 0)
            break;
        atomic_fetch_add(&group->cycles, 1);
        if (atomic_load(&group->stop))
            break;
        ret = prepare_successor(worker->path, &worker->prepared);
    }
    if (ret != 0)
        atomic_store(&group->failed, true);
    return NULL;
}

static bool unregister_persistent_handle(struct gc_foreground *foreground)
{
    myfs_file_handle_t *handle = &foreground->handle;
    myfs_file_lock_t *path_lock = myfs_lock_file(handle->storage.logical_path);
    if (!path_lock)
        return false;
    check_pthread(pthread_rwlock_wrlock(&handle->cache_lock),
                  "pthread_rwlock_wrlock");
    unregister_generation_handle_locked(handle);
    check_pthread(pthread_rwlock_unlock(&handle->cache_lock),
                  "pthread_rwlock_unlock");
    myfs_unlock_file(path_lock);
    bool ok = handle->generation_record == NULL &&
        handle->registry_prev == NULL && handle->registry_next == NULL &&
        generation_open_refs_locked(&handle->storage) == 0 &&
        generation_writer_refs_locked(&handle->storage) == 0;
    check_pthread(pthread_rwlock_destroy(&handle->cache_lock),
                  "pthread_rwlock_destroy");
    return ok;
}

static bool directory_empty(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory)
        return false;
    bool empty = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0)
        {
            empty = false;
            break;
        }
    }
    closedir(directory);
    return empty;
}

static void make_measurement_root(const struct options *options,
                                  size_t repetition, unsigned arm,
                                  char root[PATH_MAX])
{
    int written = snprintf(root, PATH_MAX,
                           "%s/myfs-generation-registry-r%zu-a%u-XXXXXX",
                           options->workdir, repetition, arm);
    if (written < 0 || written >= PATH_MAX || !mkdtemp(root))
        failf("mkdtemp under %s: %s", options->workdir, strerror(errno));
}

static struct measurement run_gc_measurement(
    size_t threads, io_mode_t io_mode, const struct options *options,
    size_t repetition, unsigned arm)
{
    destroy_generation_registry();
    destroy_lock_table();
    char root[PATH_MAX];
    make_measurement_root(options, repetition, arm, root);
    struct myfs_config config = {0};
    if (snprintf(config.root, sizeof(config.root), "%s", root) >=
        (int)sizeof(config.root))
        failf("measurement root too long");
    myfs_conf = &config;

    struct gc_group group = {
        .options = options,
        .io_mode = io_mode,
        .foreground_threads = threads,
        .gc_thread_count = options->gc_threads,
    };
    size_t participant_count = threads + options->gc_threads + 1;
    check_pthread(pthread_barrier_init(&group.ready, NULL,
                                      (unsigned)participant_count),
                  "pthread_barrier_init");
    check_pthread(pthread_barrier_init(&group.start, NULL,
                                      (unsigned)participant_count),
                  "pthread_barrier_init");
    size_t sample_count = threads * options->iterations;
    group.latencies = calloc(sample_count, sizeof(*group.latencies));
    struct gc_foreground *foregrounds = calloc(threads,
                                               sizeof(*foregrounds));
    struct gc_worker *gc_workers = calloc(options->gc_threads,
                                          sizeof(*gc_workers));
    if (!group.latencies || !foregrounds || !gc_workers)
        failf("allocation failed");

    for (size_t i = 0; i < options->gc_threads; i++)
    {
        gc_workers[i].group = &group;
        gc_workers[i].index = i;
        snprintf(gc_workers[i].path, sizeof(gc_workers[i].path),
                 "/bench-gc-%zu", i);
        if (initialize_gc_path(gc_workers[i].path,
                               &gc_workers[i].active) != 0)
            failf("failed to initialize GC path %s", gc_workers[i].path);
    }
    for (size_t i = 0; i < threads; i++)
    {
        foregrounds[i].group = &group;
        foregrounds[i].index = i;
        if (!register_persistent_handle(&foregrounds[i], options->seed))
            failf("failed to register foreground handle %zu", i);
    }
    atomic_store(&synthetic_delay_ns,
                 io_mode == IO_MODE_SYNTHETIC_DELAY
                     ? options->synthetic_delay_us * UINT64_C(1000) : 0);
    atomic_store(&synthetic_delay_calls, 0);

    for (size_t i = 0; i < options->gc_threads; i++)
        check_pthread(pthread_create(&gc_workers[i].thread, NULL,
                                     gc_worker_main, &gc_workers[i]),
                      "pthread_create");
    for (size_t i = 0; i < threads; i++)
        check_pthread(pthread_create(&foregrounds[i].thread, NULL,
                                     gc_foreground_main, &foregrounds[i]),
                      "pthread_create");

    wait_at_barrier(&group.ready);
    uint64_t gc_start_ns = monotonic_ns();
    wait_at_barrier(&group.start);
    for (size_t i = 0; i < threads; i++)
        check_pthread(pthread_join(foregrounds[i].thread, NULL),
                      "pthread_join");
    uint64_t foreground_end_ns = monotonic_ns();
    uint64_t foreground_start_ns = atomic_load(&group.foreground_start_ns);
    uint64_t elapsed_ns = foreground_start_ns > 0 &&
        foreground_end_ns >= foreground_start_ns
        ? foreground_end_ns - foreground_start_ns : 0;
    atomic_store(&group.stop, true);
    for (size_t i = 0; i < options->gc_threads; i++)
        check_pthread(pthread_join(gc_workers[i].thread, NULL),
                      "pthread_join");
    uint64_t gc_elapsed_ns = monotonic_ns() - gc_start_ns;
    atomic_store(&synthetic_delay_ns, 0);

    struct measurement measurement = {
        .operations = atomic_load(&group.operations),
        .elapsed_ns = elapsed_ns,
        .gc_cycles = atomic_load(&group.cycles),
    };
    uint64_t expected = (uint64_t)threads * options->iterations;
    bool cleanup_ok = true;
    for (size_t i = 0; i < threads; i++)
        cleanup_ok = unregister_persistent_handle(&foregrounds[i]) &&
                     cleanup_ok;
    for (size_t i = 0; i < options->gc_threads; i++)
        cleanup_ok = cleanup_gc_path(gc_workers[i].path,
                                     &gc_workers[i].active,
                                     &gc_workers[i].prepared) && cleanup_ok;
    cleanup_ok = directory_empty(root) && cleanup_ok;
    if (rmdir(root) != 0)
        cleanup_ok = false;
    measurement.self_check = !atomic_load(&group.failed) &&
        !atomic_load(&group.io_gate.timed_out) && cleanup_ok &&
        measurement.operations == expected && measurement.gc_cycles > 0 &&
        foreground_start_ns > 0 &&
        atomic_load(&group.io_gate.io_entered) > 0 &&
        atomic_load(&group.io_gate.foreground_ready) == threads;
    if (io_mode == IO_MODE_SYNTHETIC_DELAY &&
        atomic_load(&synthetic_delay_calls) == 0)
        measurement.self_check = false;
    if (!measurement.self_check)
        fprintf(stderr,
                "GC self-check detail: failed=%d cleanup=%d operations=%" PRIu64
                "/%" PRIu64 " cycles=%" PRIu64 " io_entered=%zu"
                " foreground_ready=%zu timed_out=%d delayed_calls=%"
                PRIuFAST64 "\n",
                atomic_load(&group.failed), cleanup_ok,
                measurement.operations, expected, measurement.gc_cycles,
                atomic_load(&group.io_gate.io_entered),
                atomic_load(&group.io_gate.foreground_ready),
                atomic_load(&group.io_gate.timed_out),
                atomic_load(&synthetic_delay_calls));
    measurement.operations_per_second = elapsed_ns
        ? (double)measurement.operations * 1e9 / (double)elapsed_ns : 0.0;
    measurement.per_thread_operations_per_second =
        measurement.operations_per_second / (double)threads;
    measurement.gc_cycles_per_second = gc_elapsed_ns
        ? (double)measurement.gc_cycles * 1e9 / (double)gc_elapsed_ns : 0.0;
    calculate_latency(&measurement, group.latencies, sample_count,
                      options->threshold_us * UINT64_C(1000));

    check_pthread(pthread_barrier_destroy(&group.start),
                  "pthread_barrier_destroy");
    check_pthread(pthread_barrier_destroy(&group.ready),
                  "pthread_barrier_destroy");
    free(gc_workers);
    free(foregrounds);
    free(group.latencies);
    myfs_conf = NULL;
    destroy_generation_registry();
    destroy_lock_table();
    if (!measurement.self_check)
        failf("self-check failed for gc-interference/%s/%zu",
              io_mode_name(io_mode), threads);
    return measurement;
}

static struct measurement run_measurement(
    workload_kind_t workload, io_mode_t io_mode, size_t threads,
    const struct options *options, size_t repetition, unsigned arm)
{
    if (workload == WORKLOAD_GC_INTERFERENCE)
        return run_gc_measurement(threads, io_mode, options,
                                  repetition, arm);
    return run_basic_measurement(workload, threads, options,
                                 repetition, arm);
}

static struct summary summarize(
    workload_kind_t workload, io_mode_t io_mode, size_t threads,
    const struct options *options, const struct measurement *primary,
    const struct measurement *repeat)
{
    size_t count = options->repetitions;
    double *ops = calloc(count, sizeof(*ops));
    double *per_thread = calloc(count, sizeof(*per_thread));
    double *delayed = calloc(count, sizeof(*delayed));
    double *gc_rate = calloc(count, sizeof(*gc_rate));
    double *aa_spread = calloc(count, sizeof(*aa_spread));
    uint64_t *p50 = calloc(count, sizeof(*p50));
    uint64_t *p99 = calloc(count, sizeof(*p99));
    uint64_t *p999 = calloc(count, sizeof(*p999));
    if (!ops || !per_thread || !delayed || !gc_rate || !aa_spread ||
        !p50 || !p99 || !p999)
        failf("allocation failed");
    struct summary summary = {
        .workload = workload,
        .io_mode = io_mode,
        .threads = threads,
        .gc_threads = workload == WORKLOAD_GC_INTERFERENCE
            ? options->gc_threads : 0,
        .repetitions = count,
        .operations = primary[0].operations,
        .self_check = true,
        .min_gc_cycles = UINT64_MAX,
    };
    summary.min_ops = primary[0].operations_per_second;
    summary.max_ops = primary[0].operations_per_second;
    summary.max_ns = primary[0].max_ns;
    for (size_t i = 0; i < count; i++)
    {
        ops[i] = primary[i].operations_per_second;
        per_thread[i] = primary[i].per_thread_operations_per_second;
        delayed[i] = primary[i].delayed_fraction;
        gc_rate[i] = primary[i].gc_cycles_per_second;
        p50[i] = primary[i].p50_ns;
        p99[i] = primary[i].p99_ns;
        p999[i] = primary[i].p999_ns;
        if (primary[i].max_ns > summary.max_ns)
            summary.max_ns = primary[i].max_ns;
        if (ops[i] < summary.min_ops)
            summary.min_ops = ops[i];
        if (ops[i] > summary.max_ops)
            summary.max_ops = ops[i];
        if (primary[i].gc_cycles < summary.min_gc_cycles)
            summary.min_gc_cycles = primary[i].gc_cycles;
        summary.self_check = summary.self_check && primary[i].self_check;
        if (repeat)
        {
            double low = ops[i] < repeat[i].operations_per_second
                ? ops[i] : repeat[i].operations_per_second;
            double high = ops[i] > repeat[i].operations_per_second
                ? ops[i] : repeat[i].operations_per_second;
            aa_spread[i] = low > 0.0 ? (high - low) * 100.0 / low : 0.0;
            summary.self_check = summary.self_check && repeat[i].self_check;
        }
    }
    summary.median_ops = median_double(ops, count);
    summary.median_per_thread_ops = median_double(per_thread, count);
    summary.median_delayed_fraction = median_double(delayed, count);
    summary.median_gc_cycles_per_second = median_double(gc_rate, count);
    summary.median_p50_ns = median_u64(p50, count);
    summary.median_p99_ns = median_u64(p99, count);
    summary.median_p999_ns = median_u64(p999, count);
    if (workload != WORKLOAD_GC_INTERFERENCE)
        summary.min_gc_cycles = 0;
    if (repeat)
    {
        summary.aa_median_spread_pct = median_double(aa_spread, count);
        summary.aa_max_spread_pct = aa_spread[0];
        for (size_t i = 1; i < count; i++)
        {
            if (aa_spread[i] > summary.aa_max_spread_pct)
                summary.aa_max_spread_pct = aa_spread[i];
        }
        summary.noise_floor_pct = summary.aa_max_spread_pct;
        summary.win_gate = "aa-reference";
    }
    else if (options->noise_floor_given)
    {
        summary.noise_floor_pct = options->noise_floor_pct;
        summary.win_gate = "exceeds-noise-floor";
    }
    else
    {
        summary.noise_floor_pct = 0.0;
        summary.win_gate = "unavailable";
    }

    free(p999);
    free(p99);
    free(p50);
    free(aa_spread);
    free(gc_rate);
    free(delayed);
    free(per_thread);
    free(ops);
    return summary;
}

static struct summary run_configuration(
    workload_kind_t workload, io_mode_t io_mode, size_t threads,
    const struct options *options)
{
    struct measurement *primary = calloc(options->repetitions,
                                          sizeof(*primary));
    struct measurement *repeat = options->aa
        ? calloc(options->repetitions, sizeof(*repeat)) : NULL;
    if (!primary || (options->aa && !repeat))
        failf("allocation failed");
    for (size_t repetition = 0; repetition < options->repetitions;
         repetition++)
    {
        if (options->aa && repetition % 2 != 0)
        {
            repeat[repetition] = run_measurement(
                workload, io_mode, threads, options, repetition, 2);
            primary[repetition] = run_measurement(
                workload, io_mode, threads, options, repetition, 1);
        }
        else
        {
            primary[repetition] = run_measurement(
                workload, io_mode, threads, options, repetition, 1);
            if (options->aa)
                repeat[repetition] = run_measurement(
                    workload, io_mode, threads, options, repetition, 2);
        }
    }
    struct summary summary = summarize(workload, io_mode, threads,
                                       options, primary, repeat);
    free(repeat);
    free(primary);
    return summary;
}

static void sanitize_field(char *text)
{
    for (; *text; text++)
    {
        if (*text == ',' || *text == '\n' || *text == '\r')
            *text = ';';
    }
}

static void read_cpu_model(char output[256])
{
    output[0] = '\0';
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo)
    {
        strcpy(output, "unknown");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), cpuinfo))
    {
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        if (strstr(line, "model name") == NULL &&
            strstr(line, "Hardware") == NULL)
            continue;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        value[strcspn(value, "\r\n")] = '\0';
        snprintf(output, 256, "%s", value);
        break;
    }
    fclose(cpuinfo);
    if (output[0] == '\0')
        strcpy(output, "unknown");
    sanitize_field(output);
}

static const char *filesystem_name(long type)
{
    switch ((unsigned long)type)
    {
    case UINT64_C(0x01021994):
        return "tmpfs";
    case UINT64_C(0x858458f6):
        return "ramfs";
    case UINT64_C(0x794c7630):
        return "overlay";
    case UINT64_C(0xef53):
        return "ext";
    case UINT64_C(0x9123683e):
        return "btrfs";
    case UINT64_C(0x58465342):
        return "xfs";
    default:
        return "other";
    }
}

static void print_header(const struct options *options,
                         const char *effective_cpus)
{
    struct utsname host;
    if (uname(&host) != 0)
        failf("uname: %s", strerror(errno));
    char cpu_model[256];
    read_cpu_model(cpu_model);
    struct statfs filesystem;
    if (statfs(options->workdir, &filesystem) != 0)
        failf("statfs(%s): %s", options->workdir, strerror(errno));
    const char *fs_name = filesystem_name(filesystem.f_type);
    if (strcmp(fs_name, "tmpfs") == 0 || strcmp(fs_name, "ramfs") == 0)
        fprintf(stderr,
                "WARNING: --workdir is on %s; fsync is nearly free here\n",
                fs_name);
    char thread_list[256] = {0};
    size_t used = 0;
    for (size_t i = 0; i < options->thread_count; i++)
    {
        int written = snprintf(thread_list + used, sizeof(thread_list) - used,
                               "%s%zu", i == 0 ? "" : ";",
                               options->threads[i]);
        if (written < 0 || (size_t)written >= sizeof(thread_list) - used)
            failf("thread list is too long");
        used += (size_t)written;
    }
    printf("GENERATION_REGISTRY_BENCHMARK,version=1\n");
    printf("TREE,hash=%s,kind=real-public-api\n", MYFS_BENCH_TREE_HASH);
    printf("HOST,name=%s,kernel=%s,arch=%s,cpu=%s,filesystem=%s,fs_magic=0x%lx\n",
           host.nodename, host.release, host.machine, cpu_model, fs_name,
           (unsigned long)filesystem.f_type);
    printf("AFFINITY,pinned=%s,cpus=%s\n",
           options->cpu_list_given ? "true" : "false", effective_cpus);
    printf("CONFIG,threads=%s,repetitions=%zu,warmup=%zu,iterations=%zu,"
           "gc_threads=%zu,threshold_us=%" PRIu64
           ",synthetic_delay_us=%" PRIu64 ",seed=%" PRIu64 ",aa=%s\n",
           thread_list, options->repetitions, options->warmup,
           options->iterations, options->gc_threads, options->threshold_us,
           options->synthetic_delay_us, options->seed,
           options->aa ? "true" : "false");
    puts("SYNTHETIC,wrapped=fsync|rename|unlink|rmdir|readlink|lstat|link|symlink,scope=inside-run_generation_gc_locked");
    puts("MIXED,skipped,reason=public-api-no-shard-placement");
    puts("SELF_CHECK,registry_empty=inferred-from-balanced-public-lifecycle,victims=retired,owned_files=removed");
    if (options->repetitions < 7)
        printf("WARNING,repetitions_below_recommended=%zu\n",
               options->repetitions);
}

static void print_csv_row(const struct summary *summary)
{
    printf("%s,%s,%zu,%zu,%zu,%" PRIu64
           ",%.3f,%.3f,%.3f,%.3f,%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 ",%.9f,%.3f,%" PRIu64
           ",%.6f,%.6f,%.6f,%s,%s\n",
           workload_name(summary->workload), io_mode_name(summary->io_mode),
           summary->threads, summary->gc_threads, summary->repetitions,
           summary->operations, summary->median_ops, summary->min_ops,
           summary->max_ops, summary->median_per_thread_ops,
           summary->median_p50_ns, summary->median_p99_ns,
           summary->median_p999_ns, summary->max_ns,
           summary->median_delayed_fraction,
           summary->median_gc_cycles_per_second, summary->min_gc_cycles,
           summary->aa_median_spread_pct, summary->aa_max_spread_pct,
           summary->noise_floor_pct, summary->win_gate,
           summary->self_check ? "pass" : "fail");
}

static void print_human_row(const struct summary *summary)
{
    printf("%-16s %-15s %3zu  %12.0f [%12.0f,%12.0f] "
           "p99=%9" PRIu64 "ns p99.9=%9" PRIu64
           "ns max=%9" PRIu64 "ns delayed=%8.5f gc=%8.1f/s "
           "A/Amax=%7.2f%%\n",
           workload_name(summary->workload), io_mode_name(summary->io_mode),
           summary->threads, summary->median_ops, summary->min_ops,
           summary->max_ops, summary->median_p99_ns,
           summary->median_p999_ns, summary->max_ns,
           summary->median_delayed_fraction,
           summary->median_gc_cycles_per_second,
           summary->aa_max_spread_pct);
}

int main(int argc, char **argv)
{
    struct options options = parse_options(argc, argv);
    struct stat workdir_status;
    if (stat(options.workdir, &workdir_status) != 0 ||
        !S_ISDIR(workdir_status.st_mode))
        failf("--workdir must name an existing directory: %s",
              options.workdir);
    char effective_cpus[1024];
    configure_affinity(&options, effective_cpus);
    print_header(&options, effective_cpus);

    size_t maximum_summaries = options.thread_count * 5;
    struct summary *summaries = calloc(maximum_summaries,
                                       sizeof(*summaries));
    if (!summaries)
        failf("allocation failed");
    size_t summary_count = 0;
    const workload_kind_t basic_workloads[] = {
        WORKLOAD_CONTROL,
        WORKLOAD_SCALABILITY,
        WORKLOAD_HOT_PATH,
    };
    for (size_t workload_index = 0;
         workload_index < sizeof(basic_workloads) / sizeof(basic_workloads[0]);
         workload_index++)
    {
        for (size_t thread_index = 0;
             thread_index < options.thread_count; thread_index++)
        {
            summaries[summary_count++] = run_configuration(
                basic_workloads[workload_index], IO_MODE_NONE,
                options.threads[thread_index], &options);
        }
    }
    if (options.run_real)
    {
        for (size_t i = 0; i < options.thread_count; i++)
            summaries[summary_count++] = run_configuration(
                WORKLOAD_GC_INTERFERENCE, IO_MODE_REAL,
                options.threads[i], &options);
    }
    if (options.run_synthetic)
    {
        for (size_t i = 0; i < options.thread_count; i++)
            summaries[summary_count++] = run_configuration(
                WORKLOAD_GC_INTERFERENCE, IO_MODE_SYNTHETIC_DELAY,
                options.threads[i], &options);
    }

    puts("CSV_BEGIN");
    puts("workload,io_mode,threads,gc_threads,repetitions,operations,median_ops_per_sec,min_ops_per_sec,max_ops_per_sec,median_per_thread_ops_per_sec,p50_ns,p99_ns,p999_ns,max_ns,delayed_fraction,gc_median_cycles_per_sec,gc_min_cycles,aa_median_spread_pct,aa_max_spread_pct,noise_floor_pct,win_gate,self_check");
    for (size_t i = 0; i < summary_count; i++)
        print_csv_row(&summaries[i]);
    puts("CSV_END");
    puts("SUMMARY_BEGIN");
    puts("Medians with throughput spread [min,max]:");
    for (size_t i = 0; i < summary_count; i++)
        print_human_row(&summaries[i]);
    puts("Rule: do not report a difference as a win unless it exceeds the observed A/A spread.");
    puts("SUMMARY_END");
    free(summaries);
    return EXIT_SUCCESS;
}
