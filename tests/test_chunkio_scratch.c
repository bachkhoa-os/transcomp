#include "myfs.h"
#include "chunkio_scratch.h"

#include <assert.h>
#include <stdatomic.h>

enum
{
    THREAD_COUNT = 12,
    THREAD_ITERATIONS = 400,
};

static void assert_snapshot(myfs_chunkio_scratch_role_t role,
                            myfs_chunkio_scratch_snapshot_t *snapshot)
{
    assert(myfs_chunkio_scratch_test_snapshot(role, snapshot) == 0);
}

static void test_same_size_reuses_retained_buffer(void)
{
    myfs_chunkio_scratch_lease_t first = {0};
    myfs_chunkio_scratch_lease_t second = {0};
    myfs_chunkio_scratch_snapshot_t snapshot = {0};

    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_RAW,
                                        16U * 1024U, &first) == 0);
    assert(first.data != NULL);
    void *address = first.data;
    myfs_chunkio_scratch_release(&first);
    assert(first.data == NULL);
    assert(first.capacity == 0);

    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_RAW,
                                        16U * 1024U, &second) == 0);
    assert(second.data == address);
    assert(second.capacity == 16U * 1024U);
    myfs_chunkio_scratch_release(&second);

    assert_snapshot(MYFS_CHUNKIO_SCRATCH_RAW, &snapshot);
    assert(snapshot.retained_address == address);
    assert(snapshot.retained_capacity == 16U * 1024U);
    assert(snapshot.growth_count == 1);
    assert(!snapshot.in_use);
}

static void test_growth_is_high_water_and_never_shrinks(void)
{
    myfs_chunkio_scratch_lease_t lease = {0};
    myfs_chunkio_scratch_snapshot_t before = {0};
    myfs_chunkio_scratch_snapshot_t grown = {0};
    myfs_chunkio_scratch_snapshot_t smaller = {0};

    assert_snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW, &before);
    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_WINDOW,
                                        128U * 1024U, &lease) == 0);
    assert(lease.capacity == 128U * 1024U);
    myfs_chunkio_scratch_release(&lease);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW, &grown);
    assert(grown.retained_capacity == 128U * 1024U);
    assert(grown.growth_count == before.growth_count + 1);
    const void *address = grown.retained_address;

    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_WINDOW,
                                        32U * 1024U, &lease) == 0);
    assert(lease.data == address);
    assert(lease.capacity == 128U * 1024U);
    myfs_chunkio_scratch_release(&lease);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW, &smaller);
    assert(smaller.retained_address == address);
    assert(smaller.retained_capacity == grown.retained_capacity);
    assert(smaller.growth_count == grown.growth_count);
}

static void test_roles_are_distinct_when_held_together(void)
{
    myfs_chunkio_scratch_lease_t leases[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT] = {0};

    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
    {
        assert(myfs_chunkio_scratch_acquire(
                   (myfs_chunkio_scratch_role_t)role, 4096, &leases[role]) == 0);
        assert(leases[role].data != NULL);
        for (int previous = 0; previous < role; previous++)
            assert(leases[role].data != leases[previous].data);
    }
    for (int role = MYFS_CHUNKIO_SCRATCH_ROLE_COUNT - 1; role >= 0; role--)
        myfs_chunkio_scratch_release(&leases[role]);
}

static void test_nested_same_role_uses_non_aliasing_temporary(void)
{
    myfs_chunkio_scratch_lease_t retained = {0};
    myfs_chunkio_scratch_lease_t temporary = {0};
    myfs_chunkio_scratch_snapshot_t before = {0};
    myfs_chunkio_scratch_snapshot_t held = {0};
    myfs_chunkio_scratch_snapshot_t after = {0};

    assert_snapshot(MYFS_CHUNKIO_SCRATCH_COMP, &before);
    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_COMP,
                                        8192, &retained) == 0);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_COMP, &held);
    assert(held.in_use);
    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_COMP,
                                        8192, &temporary) == 0);
    assert(temporary.data != retained.data);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_COMP, &after);
    assert(after.retained_address == retained.data);
    assert(after.retained_capacity == held.retained_capacity);
    assert(after.temporary_acquisition_count ==
           before.temporary_acquisition_count + 1);

    myfs_chunkio_scratch_release(&temporary);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_COMP, &after);
    assert(after.in_use);
    myfs_chunkio_scratch_release(&retained);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_COMP, &after);
    assert(!after.in_use);
}

static void test_oversized_request_is_temporary_without_growth(void)
{
    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
    {
        myfs_chunkio_scratch_role_t typed_role =
            (myfs_chunkio_scratch_role_t)role;
        myfs_chunkio_scratch_snapshot_t before = {0};
        myfs_chunkio_scratch_snapshot_t during = {0};
        myfs_chunkio_scratch_snapshot_t after = {0};
        myfs_chunkio_scratch_lease_t lease = {0};
        size_t cap = myfs_chunkio_scratch_retained_limit(typed_role);

        assert(cap >= MYFS_MAX_WINDOW_SIZE);
        assert_snapshot(typed_role, &before);
        assert(myfs_chunkio_scratch_acquire(typed_role, cap + 1, &lease) == 0);
        assert(lease.data != NULL);
        assert(lease.capacity == cap + 1);
        assert_snapshot(typed_role, &during);
        assert(during.retained_address == before.retained_address);
        assert(during.retained_capacity == before.retained_capacity);
        assert(during.growth_count == before.growth_count);
        assert(!during.in_use);
        assert(during.temporary_acquisition_count ==
               before.temporary_acquisition_count + 1);
        myfs_chunkio_scratch_release(&lease);
        assert_snapshot(typed_role, &after);
        assert(after.retained_address == before.retained_address);
        assert(after.retained_capacity == before.retained_capacity);
    }
}

static void test_forced_tls_unavailable_uses_temporary(void)
{
    myfs_chunkio_scratch_snapshot_t before = {0};
    myfs_chunkio_scratch_snapshot_t after = {0};
    myfs_chunkio_scratch_lease_t lease = {0};

    assert_snapshot(MYFS_CHUNKIO_SCRATCH_CACHED, &before);
    myfs_chunkio_scratch_test_force_tls_unavailable(true);
    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_CACHED,
                                        4096, &lease) == 0);
    myfs_chunkio_scratch_test_force_tls_unavailable(false);
    assert(lease.data != NULL);
    assert(lease.capacity == 4096);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_CACHED, &after);
    assert(after.retained_address == before.retained_address);
    assert(after.retained_capacity == before.retained_capacity);
    assert(after.temporary_acquisition_count ==
           before.temporary_acquisition_count + 1);
    myfs_chunkio_scratch_release(&lease);
}

static void test_failed_growth_preserves_retained_buffer_and_recovers(void)
{
    myfs_chunkio_scratch_lease_t lease = {0};
    myfs_chunkio_scratch_snapshot_t before = {0};
    myfs_chunkio_scratch_snapshot_t failed = {0};
    myfs_chunkio_scratch_snapshot_t recovered = {0};

    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_CACHED,
                                        16U * 1024U, &lease) == 0);
    myfs_chunkio_scratch_release(&lease);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_CACHED, &before);

    myfs_chunkio_scratch_test_fail_next_growth();
    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_CACHED,
                                        64U * 1024U, &lease) == 0);
    assert(lease.data != before.retained_address);
    myfs_chunkio_scratch_release(&lease);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_CACHED, &failed);
    assert(failed.retained_address == before.retained_address);
    assert(failed.retained_capacity == before.retained_capacity);
    assert(failed.growth_count == before.growth_count);
    assert(failed.temporary_acquisition_count ==
           before.temporary_acquisition_count + 1);

    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_CACHED,
                                        64U * 1024U, &lease) == 0);
    myfs_chunkio_scratch_release(&lease);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_CACHED, &recovered);
    assert(recovered.retained_capacity == 64U * 1024U);
    assert(recovered.growth_count == before.growth_count + 1);
}

typedef struct
{
    pthread_barrier_t *ready;
    pthread_barrier_t *release;
    void *address;
    atomic_int *failures;
} worker_state_t;

static void barrier_wait_or_record(pthread_barrier_t *barrier,
                                   atomic_int *failures)
{
    int ret = pthread_barrier_wait(barrier);
    if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD)
        atomic_fetch_add(failures, 1);
}

static void *scratch_worker(void *argument)
{
    worker_state_t *worker = argument;
    myfs_chunkio_scratch_lease_t lease = {0};

    if (myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_RAW,
                                     32U * 1024U, &lease) != 0)
    {
        atomic_fetch_add(worker->failures, 1);
    }
    else
    {
        worker->address = lease.data;
        for (unsigned i = 0; i < THREAD_ITERATIONS; i++)
        {
            myfs_chunkio_scratch_release(&lease);
            if (myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_RAW,
                                             32U * 1024U, &lease) != 0 ||
                lease.data != worker->address)
            {
                atomic_fetch_add(worker->failures, 1);
                break;
            }
        }
    }

    barrier_wait_or_record(worker->ready, worker->failures);
    barrier_wait_or_record(worker->release, worker->failures);
    myfs_chunkio_scratch_release(&lease);
    return NULL;
}

static void test_thread_isolation_and_destructor_cleanup(void)
{
    pthread_barrier_t ready;
    pthread_barrier_t release;
    pthread_t threads[THREAD_COUNT];
    worker_state_t workers[THREAD_COUNT] = {0};
    atomic_int failures = 0;
    size_t destroy_before = myfs_chunkio_scratch_test_destroy_count();

    assert(pthread_barrier_init(&ready, NULL, THREAD_COUNT + 1) == 0);
    assert(pthread_barrier_init(&release, NULL, THREAD_COUNT + 1) == 0);
    for (unsigned i = 0; i < THREAD_COUNT; i++)
    {
        workers[i] = (worker_state_t){
            .ready = &ready,
            .release = &release,
            .failures = &failures,
        };
        assert(pthread_create(&threads[i], NULL, scratch_worker,
                              &workers[i]) == 0);
    }

    barrier_wait_or_record(&ready, &failures);
    for (unsigned i = 0; i < THREAD_COUNT; i++)
    {
        assert(workers[i].address != NULL);
        for (unsigned j = i + 1; j < THREAD_COUNT; j++)
            assert(workers[i].address != workers[j].address);
    }
    barrier_wait_or_record(&release, &failures);
    for (unsigned i = 0; i < THREAD_COUNT; i++)
        assert(pthread_join(threads[i], NULL) == 0);

    assert(atomic_load(&failures) == 0);
    assert(myfs_chunkio_scratch_test_destroy_count() ==
           destroy_before + THREAD_COUNT);
    assert(pthread_barrier_destroy(&release) == 0);
    assert(pthread_barrier_destroy(&ready) == 0);
}

static void test_release_clears_in_use_and_double_release_is_harmless(void)
{
    myfs_chunkio_scratch_lease_t lease = {0};
    myfs_chunkio_scratch_snapshot_t snapshot = {0};

    assert(myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_WINDOW,
                                        4096, &lease) == 0);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW, &snapshot);
    assert(snapshot.in_use);
    myfs_chunkio_scratch_release(&lease);
    myfs_chunkio_scratch_release(&lease);
    assert_snapshot(MYFS_CHUNKIO_SCRATCH_WINDOW, &snapshot);
    assert(!snapshot.in_use);
    assert(lease.data == NULL);
    assert(lease.capacity == 0);
}

static bool run_request_or_compaction_pattern(bool compaction_like)
{
    myfs_chunkio_scratch_lease_t window = {0};
    myfs_chunkio_scratch_lease_t cached = {0};
    myfs_chunkio_scratch_lease_t raw = {0};
    myfs_chunkio_scratch_lease_t comp = {0};
    bool success = false;

    if (myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_WINDOW,
                                     64U * 1024U, &window) != 0)
        goto out;
    if (compaction_like &&
        myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_CACHED,
                                     128U * 1024U, &cached) != 0)
        goto out;
    if (myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_RAW,
                                     32U * 1024U, &raw) != 0)
        goto out;
    myfs_chunkio_scratch_release(&raw);
    if (myfs_chunkio_scratch_acquire(
            MYFS_CHUNKIO_SCRATCH_COMP,
            ZSTD_compressBound(64U * 1024U), &comp) != 0)
        goto out;
    success = true;

out:
    myfs_chunkio_scratch_release(&comp);
    myfs_chunkio_scratch_release(&raw);
    myfs_chunkio_scratch_release(&cached);
    myfs_chunkio_scratch_release(&window);
    return success;
}

static void test_foreground_then_fallback_compaction_reuses_same_thread_slots(void)
{
    myfs_chunkio_scratch_snapshot_t warm[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];

    assert(run_request_or_compaction_pattern(false));
    assert(run_request_or_compaction_pattern(true));
    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        assert_snapshot((myfs_chunkio_scratch_role_t)role, &warm[role]);

    for (unsigned i = 0; i < 500; i++)
    {
        assert(run_request_or_compaction_pattern(false));
        assert(run_request_or_compaction_pattern(true));
    }
    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
    {
        myfs_chunkio_scratch_snapshot_t after = {0};
        assert_snapshot((myfs_chunkio_scratch_role_t)role, &after);
        assert(after.growth_count == warm[role].growth_count);
        assert(after.temporary_acquisition_count ==
               warm[role].temporary_acquisition_count);
        assert(!after.in_use);
    }
}

enum
{
    REQUEST_THREAD_COUNT = 8,
    MIXED_THREAD_COUNT = REQUEST_THREAD_COUNT + 1,
};

typedef struct
{
    pthread_barrier_t *ready;
    pthread_barrier_t *release;
    bool compaction_like;
    void *addresses[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
    atomic_int *failures;
} mixed_worker_state_t;

static void *mixed_worker(void *argument)
{
    mixed_worker_state_t *worker = argument;
    myfs_chunkio_scratch_lease_t held[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT] = {0};

    for (unsigned i = 0; i < THREAD_ITERATIONS; i++)
    {
        if (!run_request_or_compaction_pattern(worker->compaction_like))
        {
            atomic_fetch_add(worker->failures, 1);
            break;
        }
    }
    for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
    {
        if (myfs_chunkio_scratch_acquire(
                (myfs_chunkio_scratch_role_t)role, 4096,
                &held[role]) != 0)
        {
            atomic_fetch_add(worker->failures, 1);
            break;
        }
        worker->addresses[role] = held[role].data;
    }

    barrier_wait_or_record(worker->ready, worker->failures);
    barrier_wait_or_record(worker->release, worker->failures);
    for (int role = MYFS_CHUNKIO_SCRATCH_ROLE_COUNT - 1; role >= 0; role--)
        myfs_chunkio_scratch_release(&held[role]);
    return NULL;
}

static void test_request_threads_and_dedicated_compaction_thread_are_isolated(void)
{
    pthread_barrier_t ready;
    pthread_barrier_t release;
    pthread_t threads[MIXED_THREAD_COUNT];
    mixed_worker_state_t workers[MIXED_THREAD_COUNT] = {0};
    atomic_int failures = 0;
    size_t destroy_before = myfs_chunkio_scratch_test_destroy_count();

    assert(pthread_barrier_init(&ready, NULL, MIXED_THREAD_COUNT + 1) == 0);
    assert(pthread_barrier_init(&release, NULL, MIXED_THREAD_COUNT + 1) == 0);
    for (unsigned i = 0; i < MIXED_THREAD_COUNT; i++)
    {
        workers[i] = (mixed_worker_state_t){
            .ready = &ready,
            .release = &release,
            .compaction_like = i == REQUEST_THREAD_COUNT,
            .failures = &failures,
        };
        assert(pthread_create(&threads[i], NULL, mixed_worker,
                              &workers[i]) == 0);
    }

    barrier_wait_or_record(&ready, &failures);
    for (unsigned i = 0; i < MIXED_THREAD_COUNT; i++)
    {
        for (int role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        {
            assert(workers[i].addresses[role] != NULL);
            for (unsigned j = i + 1; j < MIXED_THREAD_COUNT; j++)
                assert(workers[i].addresses[role] !=
                       workers[j].addresses[role]);
        }
    }
    barrier_wait_or_record(&release, &failures);
    for (unsigned i = 0; i < MIXED_THREAD_COUNT; i++)
        assert(pthread_join(threads[i], NULL) == 0);

    assert(atomic_load(&failures) == 0);
    assert(myfs_chunkio_scratch_test_destroy_count() ==
           destroy_before + MIXED_THREAD_COUNT);
    assert(pthread_barrier_destroy(&release) == 0);
    assert(pthread_barrier_destroy(&ready) == 0);
}

int main(void)
{
    test_same_size_reuses_retained_buffer();
    test_growth_is_high_water_and_never_shrinks();
    test_roles_are_distinct_when_held_together();
    test_nested_same_role_uses_non_aliasing_temporary();
    test_oversized_request_is_temporary_without_growth();
    test_forced_tls_unavailable_uses_temporary();
    test_failed_growth_preserves_retained_buffer_and_recovers();
    test_thread_isolation_and_destructor_cleanup();
    test_release_clears_in_use_and_double_release_is_harmless();
    test_foreground_then_fallback_compaction_reuses_same_thread_slots();
    test_request_threads_and_dedicated_compaction_thread_are_isolated();
    return 0;
}
