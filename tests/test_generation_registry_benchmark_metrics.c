/*
 * Exercise the benchmark's real latency/aggregation functions without running
 * its workers or filesystem workloads. BENCHMARK_SOURCE permits isolated
 * mutation checks; the normal test always includes the checked-in benchmark.
 */
#ifndef BENCHMARK_SOURCE
#define BENCHMARK_SOURCE "../benchmarks/generation_registry_bench.c"
#endif
#define main benchmark_program_main
#include BENCHMARK_SOURCE
#undef main

#ifdef NDEBUG
#error "The pooled-metrics regression fixture requires assertions"
#endif
#include <assert.h>
#include <math.h>

int main(void)
{
    const struct options options = {
        .repetitions = 7,
        .gc_threads = 1,
        .threshold_us = 1000,
    };
    const uint64_t threshold_ns = options.threshold_us * UINT64_C(1000);

    /* Catch > becoming >= independently of the seven-repetition fixture. */
    uint64_t boundary_sample = 1000000;
    struct measurement boundary = {0};
    calculate_latency(&boundary, &boundary_sample, 1, threshold_ns);
    assert(boundary.delayed_operations == 0);
    assert(boundary.delayed_fraction == 0);
    boundary_sample = 1000001;
    calculate_latency(&boundary, &boundary_sample, 1, threshold_ns);
    assert(boundary.delayed_operations == 1);
    assert(boundary.delayed_fraction == 1);

    const size_t operation_counts[] = {2, 3, 4, 5, 6, 7, 8};
    const uint64_t gc_cycles[] = {1, 2, 3, 4, 5, 6, 7};
    const uint64_t delayed_counts[] = {1, 0, 0, 0, 0, 0, 2};
    struct measurement primary[7] = {0};
    struct measurement companion[7] = {0};
    for (size_t i = 0; i < 7; i++)
    {
        uint64_t samples[8];
        for (size_t j = 0; j < operation_counts[i]; j++)
            samples[j] = 100;
        samples[0] = 1000000; /* Equality is not a delayed observation. */
        if (i == 0)
            samples[1] = 1000001;
        if (i == 6)
            samples[1] = samples[2] = 2000000;
        primary[i].operations = operation_counts[i];
        primary[i].operations_per_second = 1000 + i;
        primary[i].per_thread_operations_per_second = 1000 + i;
        primary[i].gc_cycles = gc_cycles[i];
        primary[i].self_check = true;
        calculate_latency(&primary[i], samples, operation_counts[i],
                          threshold_ns);
        assert(primary[i].delayed_operations == delayed_counts[i]);

        /* Different counts and all-delayed samples expose A/A contamination. */
        uint64_t companion_samples[17];
        size_t companion_count = 11 + i;
        for (size_t j = 0; j < companion_count; j++)
            companion_samples[j] = 9000000;
        companion[i].operations = companion_count;
        companion[i].operations_per_second = 2000 + i;
        companion[i].per_thread_operations_per_second = 2000 + i;
        companion[i].gc_cycles = 1000 + i;
        companion[i].self_check = true;
        calculate_latency(&companion[i], companion_samples, companion_count,
                          threshold_ns);
    }

    struct summary summary = summarize(WORKLOAD_GC_INTERFERENCE, IO_MODE_REAL,
                                        1, &options, primary, companion);
    assert(summary.repetitions == 7);
    assert(summary.median_delayed_fraction == 0);
    assert(summary.delayed_total == 3);
    assert(summary.pooled_operations == 35);
    assert(fabs(summary.pooled_delayed_fraction - 3.0 / 35.0) < 1e-12);
    /* 28 is the sum of completed primary cycles, not min_cycles * 7. */
    assert(fabs(summary.delayed_per_gc_cycle - 3.0 / 28.0) < 1e-12);
    assert(summary.max_ns == 2000000);
    assert(summary.operations == 5);
    assert(summary.min_gc_cycles == 1);
    assert(summary.self_check);

    for (size_t i = 0; i < 7; i++)
    {
        primary[i].gc_cycles = 0;
        companion[i].gc_cycles = 0;
    }
    summary = summarize(WORKLOAD_CONTROL, IO_MODE_NONE, 1, &options,
                        primary, companion);
    assert(summary.median_delayed_fraction == 0);
    assert(summary.delayed_total == 3);
    assert(summary.pooled_operations == 35);
    assert(fabs(summary.pooled_delayed_fraction - 3.0 / 35.0) < 1e-12);
    assert(summary.delayed_per_gc_cycle == 0);
    assert(summary.min_gc_cycles == 0);
    assert(summary.gc_threads == 0);
    assert(summary.self_check);

    puts("generation registry pooled metrics: pass (7 primary repetitions, "
         "35 operations, 3 delayed, 28 GC cycles, strict threshold, "
         "A/A exclusion, control zero denominator)");
    return 0;
}
