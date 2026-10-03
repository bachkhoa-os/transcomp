#include "myfs.h"

#include <assert.h>
#include <stdatomic.h>

/* Test-only accessors emitted by lock.c under MYFS_TEST_FAILPOINTS. */
extern size_t myfs_lock_test_live_count(void);
extern unsigned myfs_lock_test_refs(const char *path);
extern size_t myfs_lock_test_shard_index(const char *path);
extern size_t myfs_lock_test_bucket_index(const char *path);

static struct timespec deadline_after_ms(long milliseconds)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += milliseconds / 1000;
    deadline.tv_nsec += (milliseconds % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

static void wait_for_refs(const char *path, unsigned refs)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
    deadline.tv_sec += 2;
    for (;;)
    {
        if (myfs_lock_test_refs(path) == refs)
            return;
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec < deadline.tv_sec ||
               (now.tv_sec == deadline.tv_sec &&
                now.tv_nsec < deadline.tv_nsec));
        sched_yield();
    }
}

static void test_path_length_boundary_and_reclamation(void)
{
    char accepted[PATH_MAX];
    memset(accepted, 'a', sizeof(accepted));
    accepted[0] = '/';
    accepted[PATH_MAX - 1] = '\0';

    myfs_file_lock_t *lk = myfs_lock_file(accepted);
    assert(lk != NULL);
    assert(myfs_lock_test_live_count() == 1);
    assert(myfs_lock_test_refs(accepted) == 1);
    myfs_unlock_file(lk);
    assert(myfs_lock_test_live_count() == 0);

    char rejected[PATH_MAX + 1];
    memset(rejected, 'b', sizeof(rejected));
    rejected[0] = '/';
    rejected[PATH_MAX] = '\0';
    assert(myfs_lock_file(rejected) == NULL);
    myfs_unlock_file(NULL);
    assert(myfs_lock_test_live_count() == 0);
}

struct waiter_context
{
    const char *path;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool acquired;
    bool release;
    myfs_file_lock_t *token;
};

static void *waiter_main(void *argument)
{
    struct waiter_context *context = argument;
    myfs_file_lock_t *lk = myfs_lock_file(context->path);
    assert(lk != NULL);
    assert(pthread_mutex_lock(&context->mu) == 0);
    context->token = lk;
    context->acquired = true;
    assert(pthread_cond_signal(&context->cv) == 0);
    while (!context->release)
        assert(pthread_cond_wait(&context->cv, &context->mu) == 0);
    assert(pthread_mutex_unlock(&context->mu) == 0);
    myfs_unlock_file(lk);
    return NULL;
}

static void test_waiter_pins_entry_until_final_release(void)
{
    char first[] = "/same-content";
    char second[] = "/same-content";
    myfs_file_lock_t *owner = myfs_lock_file(first);
    assert(owner != NULL);

    struct waiter_context context = {
        .path = second,
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    pthread_t waiter;
    assert(pthread_create(&waiter, NULL, waiter_main, &context) == 0);
    wait_for_refs(first, 2);
    myfs_unlock_file(owner);

    assert(pthread_mutex_lock(&context.mu) == 0);
    struct timespec deadline = deadline_after_ms(2000);
    while (!context.acquired)
        assert(pthread_cond_timedwait(&context.cv, &context.mu, &deadline) == 0);
    assert(context.token == owner);
    assert(myfs_lock_test_live_count() == 1);
    assert(myfs_lock_test_refs(first) == 1);
    context.release = true;
    assert(pthread_cond_signal(&context.cv) == 0);
    assert(pthread_mutex_unlock(&context.mu) == 0);

    assert(pthread_join(waiter, NULL) == 0);
    assert(myfs_lock_test_live_count() == 0);
    assert(pthread_cond_destroy(&context.cv) == 0);
    assert(pthread_mutex_destroy(&context.mu) == 0);
}

struct progress_context
{
    const char *path;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
};

static void *progress_main(void *argument)
{
    struct progress_context *context = argument;
    myfs_file_lock_t *lk = myfs_lock_file(context->path);
    assert(lk != NULL);
    myfs_unlock_file(lk);
    assert(pthread_mutex_lock(&context->mu) == 0);
    context->done = true;
    assert(pthread_cond_signal(&context->cv) == 0);
    assert(pthread_mutex_unlock(&context->mu) == 0);
    return NULL;
}

static void verify_independent_progress(const char *held_path,
                                        const char *other_path)
{
    myfs_file_lock_t *held = myfs_lock_file(held_path);
    assert(held != NULL);
    struct progress_context context = {
        .path = other_path,
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    pthread_t worker;
    assert(pthread_create(&worker, NULL, progress_main, &context) == 0);
    assert(pthread_mutex_lock(&context.mu) == 0);
    struct timespec deadline = deadline_after_ms(2000);
    while (!context.done)
        assert(pthread_cond_timedwait(&context.cv, &context.mu, &deadline) == 0);
    assert(pthread_mutex_unlock(&context.mu) == 0);
    myfs_unlock_file(held);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_cond_destroy(&context.cv) == 0);
    assert(pthread_mutex_destroy(&context.mu) == 0);
    assert(myfs_lock_test_live_count() == 0);
}

static void test_distinct_paths_progress_independently(void)
{
    verify_independent_progress("/held", "/other");
}

static void find_bucket_collision(char first[64], char second[64])
{
    char seen[64 * 64][64] = {{0}};
    for (unsigned i = 0; i < 100000; i++)
    {
        char candidate[64];
        assert(snprintf(candidate, sizeof(candidate), "/collision-%u", i) > 0);
        size_t slot = myfs_lock_test_shard_index(candidate) * 64 +
                      myfs_lock_test_bucket_index(candidate);
        assert(slot < 64 * 64);
        if (seen[slot][0] != '\0')
        {
            strcpy(first, seen[slot]);
            strcpy(second, candidate);
            assert(strcmp(first, second) != 0);
            return;
        }
        strcpy(seen[slot], candidate);
    }
    assert(!"failed to find a deterministic shard/bucket collision");
}

static void test_same_bucket_paths_keep_distinct_mutexes(void)
{
    char first[64];
    char second[64];
    find_bucket_collision(first, second);
    assert(myfs_lock_test_shard_index(first) ==
           myfs_lock_test_shard_index(second));
    assert(myfs_lock_test_bucket_index(first) ==
           myfs_lock_test_bucket_index(second));
    verify_independent_progress(first, second);
}

enum
{
    SERIAL_THREADS = 8,
    SERIAL_ITERATIONS = 5000,
};

struct serial_context
{
    atomic_int occupancy;
    atomic_int failed;
    unsigned counter;
};

static void *serial_worker(void *argument)
{
    struct serial_context *context = argument;
    for (unsigned i = 0; i < SERIAL_ITERATIONS; i++)
    {
        myfs_file_lock_t *lk = myfs_lock_file("/serialized");
        if (!lk)
        {
            atomic_store(&context->failed, 1);
            return NULL;
        }
        if (atomic_fetch_add(&context->occupancy, 1) != 0)
            atomic_store(&context->failed, 1);
        context->counter++;
        if (atomic_fetch_sub(&context->occupancy, 1) != 1)
            atomic_store(&context->failed, 1);
        myfs_unlock_file(lk);
    }
    return NULL;
}

static void test_same_path_serializes_concurrent_workers(void)
{
    struct serial_context context = {0};
    pthread_t workers[SERIAL_THREADS];
    for (size_t i = 0; i < SERIAL_THREADS; i++)
        assert(pthread_create(&workers[i], NULL, serial_worker, &context) == 0);
    for (size_t i = 0; i < SERIAL_THREADS; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    assert(atomic_load(&context.failed) == 0);
    assert(context.counter == SERIAL_THREADS * SERIAL_ITERATIONS);
    assert(myfs_lock_test_live_count() == 0);
}

enum
{
    LIVE_HOLDERS = 64,
};

struct holders_context
{
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned ready;
    bool release;
};

struct holder_context
{
    struct holders_context *shared;
    char path[64];
};

static void *holder_main(void *argument)
{
    struct holder_context *context = argument;
    myfs_file_lock_t *lk = myfs_lock_file(context->path);
    assert(lk != NULL);
    assert(pthread_mutex_lock(&context->shared->mu) == 0);
    context->shared->ready++;
    assert(pthread_cond_broadcast(&context->shared->cv) == 0);
    while (!context->shared->release)
        assert(pthread_cond_wait(&context->shared->cv,
                                 &context->shared->mu) == 0);
    assert(pthread_mutex_unlock(&context->shared->mu) == 0);
    myfs_unlock_file(lk);
    return NULL;
}

static void test_many_live_entries_reclaim_after_release(void)
{
    struct holders_context shared = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    struct holder_context holders[LIVE_HOLDERS];
    pthread_t threads[LIVE_HOLDERS];
    for (size_t i = 0; i < LIVE_HOLDERS; i++)
    {
        holders[i].shared = &shared;
        assert(snprintf(holders[i].path, sizeof(holders[i].path),
                        "/live-%zu", i) > 0);
        assert(pthread_create(&threads[i], NULL, holder_main, &holders[i]) == 0);
    }
    assert(pthread_mutex_lock(&shared.mu) == 0);
    struct timespec deadline = deadline_after_ms(5000);
    while (shared.ready != LIVE_HOLDERS)
        assert(pthread_cond_timedwait(&shared.cv, &shared.mu, &deadline) == 0);
    assert(myfs_lock_test_live_count() == LIVE_HOLDERS);
    shared.release = true;
    assert(pthread_cond_broadcast(&shared.cv) == 0);
    assert(pthread_mutex_unlock(&shared.mu) == 0);
    for (size_t i = 0; i < LIVE_HOLDERS; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(myfs_lock_test_live_count() == 0);
    assert(pthread_cond_destroy(&shared.cv) == 0);
    assert(pthread_mutex_destroy(&shared.mu) == 0);
}

enum
{
    CHURN_PATHS = 257,
    CHURN_THREADS = 16,
    CHURN_ITERATIONS = 4000,
};

struct churn_context
{
    char (*paths)[64];
    atomic_int *occupancy;
    atomic_int failed;
};

struct churn_worker_context
{
    struct churn_context *shared;
    uint32_t state;
};

static void *churn_worker(void *argument)
{
    struct churn_worker_context *worker = argument;
    for (unsigned i = 0; i < CHURN_ITERATIONS; i++)
    {
        worker->state = worker->state * UINT32_C(1664525) + UINT32_C(1013904223);
        size_t index = worker->state % CHURN_PATHS;
        myfs_file_lock_t *lk = myfs_lock_file(worker->shared->paths[index]);
        if (!lk)
        {
            atomic_store(&worker->shared->failed, 1);
            return NULL;
        }
        if (atomic_fetch_add(&worker->shared->occupancy[index], 1) != 0)
            atomic_store(&worker->shared->failed, 1);
        if (atomic_fetch_sub(&worker->shared->occupancy[index], 1) != 1)
            atomic_store(&worker->shared->failed, 1);
        myfs_unlock_file(lk);
    }
    return NULL;
}

static void test_concurrent_churn_preserves_exclusion_and_reclaims(void)
{
    char paths[CHURN_PATHS][64];
    atomic_int occupancy[CHURN_PATHS];
    for (size_t i = 0; i < CHURN_PATHS; i++)
    {
        assert(snprintf(paths[i], sizeof(paths[i]), "/churn-%zu", i) > 0);
        atomic_init(&occupancy[i], 0);
    }
    struct churn_context context = {
        .paths = paths,
        .occupancy = occupancy,
    };
    struct churn_worker_context worker_contexts[CHURN_THREADS];
    pthread_t workers[CHURN_THREADS];
    for (size_t i = 0; i < CHURN_THREADS; i++)
    {
        worker_contexts[i] = (struct churn_worker_context){
            .shared = &context,
            .state = (uint32_t)(i + 1),
        };
        assert(pthread_create(&workers[i], NULL, churn_worker,
                              &worker_contexts[i]) == 0);
    }
    for (size_t i = 0; i < CHURN_THREADS; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    assert(atomic_load(&context.failed) == 0);
    assert(myfs_lock_test_live_count() == 0);
}

static void test_quiescent_destroy_is_idempotent_and_reusable(void)
{
    destroy_lock_table();
    destroy_lock_table();
    myfs_file_lock_t *lk = myfs_lock_file("/after-destroy");
    assert(lk != NULL);
    myfs_unlock_file(lk);
    destroy_lock_table();
    assert(myfs_lock_test_live_count() == 0);
}

int main(void)
{
    test_path_length_boundary_and_reclamation();
    test_waiter_pins_entry_until_final_release();
    test_distinct_paths_progress_independently();
    test_same_bucket_paths_keep_distinct_mutexes();
    test_same_path_serializes_concurrent_workers();
    test_many_live_entries_reclaim_after_release();
    test_concurrent_churn_preserves_exclusion_and_reclaims();
    test_quiescent_destroy_is_idempotent_and_reusable();
    return 0;
}
