#include "myfs.h"

#ifdef MYFS_TEST_FAILPOINTS
#include "core/lock_test.h"
#endif

/*
 * lock.c — per-path locking.
 *
 * Each logical path has one mutex, allocated on demand and reclaimed as soon
 * as its last owner, waiter, or unlocker drops its reference. Entries live in
 * a fixed hash table with independently protected shards. A shard mutex
 * protects only that shard's buckets, links, and reference counts. It must
 * always be released before waiting for a path mutex.
 *
 * System-wide nested lock ordering (never reverse):
 *   path lock -> cache_lock -> generation-registry shard
 * A path lock may also take a registry shard directly, and fast handle reads
 * take cache_lock -> registry shard without a path lock. compact_queue_mu is
 * released before waiting for a path lock; it is not part of this nesting.
 *
 * Future operations that need more than one path lock (for example rename)
 * must deduplicate identical paths, then acquire distinct paths in ascending
 * (shard index, strcmp(path)) order and release them in reverse order. Paths
 * in the same shard are therefore ordered solely by strcmp(path). Hash-table
 * bucket indices do not participate in this ordering. A shard mutex is never
 * held while waiting for any path mutex, including the same-shard case.
 *
 * destroy_lock_table() is only valid after all users have quiesced. It never
 * holds more than one shard mutex at a time. The shard mutexes themselves
 * remain initialized so the table can be reused after destruction.
 */

enum
{
    MYFS_LOCK_SHARD_COUNT = 64,
    MYFS_LOCK_BUCKET_COUNT = 64,
};

_Static_assert((MYFS_LOCK_SHARD_COUNT & (MYFS_LOCK_SHARD_COUNT - 1)) == 0,
               "lock shard count must be a power of two");
_Static_assert((MYFS_LOCK_BUCKET_COUNT & (MYFS_LOCK_BUCKET_COUNT - 1)) == 0,
               "lock bucket count must be a power of two");

struct myfs_file_lock
{
    char path[PATH_MAX];
    uint64_t path_hash;
    size_t shard_index;
    size_t bucket_index;
    pthread_mutex_t mu;
    unsigned refs;
    struct myfs_file_lock *previous;
    struct myfs_file_lock *next;
};

struct lock_shard
{
    pthread_mutex_t mu;
    struct myfs_file_lock *buckets[MYFS_LOCK_BUCKET_COUNT];
};

static struct lock_shard lock_shards[MYFS_LOCK_SHARD_COUNT];
static pthread_once_t lock_table_once = PTHREAD_ONCE_INIT;
static int lock_table_init_status;

#ifdef MYFS_TEST_FAILPOINTS
static pthread_mutex_t lock_test_hook_mu = PTHREAD_MUTEX_INITIALIZER;
static myfs_lock_test_acquire_fail_hook_fn lock_test_acquire_fail_hook;
static void *lock_test_acquire_fail_hook_context;

void myfs_lock_test_set_acquire_fail_hook(
    myfs_lock_test_acquire_fail_hook_fn hook, void *context)
{
    if (pthread_mutex_lock(&lock_test_hook_mu) != 0)
        return;
    lock_test_acquire_fail_hook = hook;
    lock_test_acquire_fail_hook_context = context;
    pthread_mutex_unlock(&lock_test_hook_mu);
}

static bool lock_test_should_fail_acquire(const char *path)
{
    if (pthread_mutex_lock(&lock_test_hook_mu) != 0)
        return false;
    bool fail = lock_test_acquire_fail_hook &&
        lock_test_acquire_fail_hook(
            path, lock_test_acquire_fail_hook_context);
    if (fail)
    {
        lock_test_acquire_fail_hook = NULL;
        lock_test_acquire_fail_hook_context = NULL;
    }
    pthread_mutex_unlock(&lock_test_hook_mu);
    return fail;
}
#endif

static uint64_t hash_path(const char *path, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++)
    {
        hash ^= (unsigned char)path[i];
        hash *= UINT64_C(1099511628211);
    }

    /* MurmurHash3's 64-bit finalizer improves the low-bit distribution. */
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash;
}

static size_t shard_index_for_hash(uint64_t hash)
{
    return (size_t)hash & (MYFS_LOCK_SHARD_COUNT - 1);
}

static size_t bucket_index_for_hash(uint64_t hash)
{
    return ((size_t)(hash >> 6)) & (MYFS_LOCK_BUCKET_COUNT - 1);
}

static void initialize_lock_table(void)
{
    size_t initialized = 0;
    for (; initialized < MYFS_LOCK_SHARD_COUNT; initialized++)
    {
        int status = pthread_mutex_init(&lock_shards[initialized].mu, NULL);
        if (status != 0)
        {
            lock_table_init_status = status;
            while (initialized > 0)
            {
                initialized--;
                pthread_mutex_destroy(&lock_shards[initialized].mu);
            }
            return;
        }
    }
}

static int ensure_lock_table_initialized(void)
{
    int status = pthread_once(&lock_table_once, initialize_lock_table);
    return status != 0 ? status : lock_table_init_status;
}

static struct myfs_file_lock *find_entry(struct lock_shard *shard,
                                         size_t bucket_index,
                                         uint64_t hash,
                                         const char *path)
{
    struct myfs_file_lock *entry = shard->buckets[bucket_index];
    while (entry)
    {
        if (entry->path_hash == hash && strcmp(entry->path, path) == 0)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static void unlink_entry(struct lock_shard *shard,
                         struct myfs_file_lock *entry)
{
    if (entry->previous)
        entry->previous->next = entry->next;
    else
        shard->buckets[entry->bucket_index] = entry->next;
    if (entry->next)
        entry->next->previous = entry->previous;
}

static void release_entry_reference(struct myfs_file_lock *entry)
{
    struct lock_shard *shard = &lock_shards[entry->shard_index];
    bool reclaim = false;

    if (pthread_mutex_lock(&shard->mu) != 0)
        return;
    if (entry->refs > 0)
    {
        entry->refs--;
        if (entry->refs == 0)
        {
            unlink_entry(shard, entry);
            reclaim = true;
        }
    }
    pthread_mutex_unlock(&shard->mu);

    if (reclaim)
    {
        pthread_mutex_destroy(&entry->mu);
        free(entry);
    }
}

myfs_file_lock_t *myfs_lock_file(const char *path)
{
    if (!path || ensure_lock_table_initialized() != 0)
        return NULL;

    size_t length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX)
        return NULL;

#ifdef MYFS_TEST_FAILPOINTS
    /* Run a blocking test hook before taking a lock-table shard.  A true
     * result is consumed atomically so exactly one matching acquisition can
     * be forced to fail. */
    if (lock_test_should_fail_acquire(path))
        return NULL;
#endif

    uint64_t hash = hash_path(path, length);
    size_t shard_index = shard_index_for_hash(hash);
    size_t bucket_index = bucket_index_for_hash(hash);
    struct lock_shard *shard = &lock_shards[shard_index];

    if (pthread_mutex_lock(&shard->mu) != 0)
        return NULL;
    struct myfs_file_lock *entry =
        find_entry(shard, bucket_index, hash, path);
    if (entry)
    {
        if (entry->refs == UINT_MAX)
        {
            pthread_mutex_unlock(&shard->mu);
            return NULL;
        }
        entry->refs++;
    }
    else
    {
        entry = calloc(1, sizeof(*entry));
        if (!entry)
        {
            pthread_mutex_unlock(&shard->mu);
            return NULL;
        }
        if (pthread_mutex_init(&entry->mu, NULL) != 0)
        {
            pthread_mutex_unlock(&shard->mu);
            free(entry);
            return NULL;
        }
        memcpy(entry->path, path, length + 1);
        entry->path_hash = hash;
        entry->shard_index = shard_index;
        entry->bucket_index = bucket_index;
        entry->refs = 1;
        entry->next = shard->buckets[bucket_index];
        if (entry->next)
            entry->next->previous = entry;
        shard->buckets[bucket_index] = entry;
    }
    pthread_mutex_unlock(&shard->mu);

    if (pthread_mutex_lock(&entry->mu) != 0)
    {
        release_entry_reference(entry);
        return NULL;
    }
    return entry;
}

void myfs_unlock_file(myfs_file_lock_t *entry)
{
    if (!entry)
        return;

    pthread_mutex_unlock(&entry->mu);
    release_entry_reference(entry);
}

void destroy_lock_table(void)
{
    if (ensure_lock_table_initialized() != 0)
        return;

    for (size_t shard_index = 0;
         shard_index < MYFS_LOCK_SHARD_COUNT;
         shard_index++)
    {
        struct lock_shard *shard = &lock_shards[shard_index];
        struct myfs_file_lock *detached[MYFS_LOCK_BUCKET_COUNT];

        if (pthread_mutex_lock(&shard->mu) != 0)
            continue;
        for (size_t bucket_index = 0;
             bucket_index < MYFS_LOCK_BUCKET_COUNT;
             bucket_index++)
        {
            detached[bucket_index] = shard->buckets[bucket_index];
            shard->buckets[bucket_index] = NULL;
        }
        pthread_mutex_unlock(&shard->mu);

        for (size_t bucket_index = 0;
             bucket_index < MYFS_LOCK_BUCKET_COUNT;
             bucket_index++)
        {
            struct myfs_file_lock *entry = detached[bucket_index];
            while (entry)
            {
                struct myfs_file_lock *next = entry->next;
                pthread_mutex_destroy(&entry->mu);
                free(entry);
                entry = next;
            }
        }
    }
}

#ifdef MYFS_TEST_FAILPOINTS
size_t myfs_lock_test_live_count(void)
{
    if (ensure_lock_table_initialized() != 0)
        return 0;

    size_t count = 0;
    for (size_t shard_index = 0;
         shard_index < MYFS_LOCK_SHARD_COUNT;
         shard_index++)
    {
        struct lock_shard *shard = &lock_shards[shard_index];
        if (pthread_mutex_lock(&shard->mu) != 0)
            continue;
        for (size_t bucket_index = 0;
             bucket_index < MYFS_LOCK_BUCKET_COUNT;
             bucket_index++)
        {
            for (struct myfs_file_lock *entry = shard->buckets[bucket_index];
                 entry;
                 entry = entry->next)
                count++;
        }
        pthread_mutex_unlock(&shard->mu);
    }
    return count;
}

unsigned myfs_lock_test_refs(const char *path)
{
    if (!path || ensure_lock_table_initialized() != 0)
        return 0;
    size_t length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX)
        return 0;

    uint64_t hash = hash_path(path, length);
    size_t shard_index = shard_index_for_hash(hash);
    size_t bucket_index = bucket_index_for_hash(hash);
    struct lock_shard *shard = &lock_shards[shard_index];
    if (pthread_mutex_lock(&shard->mu) != 0)
        return 0;
    struct myfs_file_lock *entry =
        find_entry(shard, bucket_index, hash, path);
    unsigned refs = entry ? entry->refs : 0;
    pthread_mutex_unlock(&shard->mu);
    return refs;
}

size_t myfs_lock_test_shard_index(const char *path)
{
    if (!path)
        return SIZE_MAX;
    size_t length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX)
        return SIZE_MAX;
    return shard_index_for_hash(hash_path(path, length));
}

size_t myfs_lock_test_bucket_index(const char *path)
{
    if (!path)
        return SIZE_MAX;
    size_t length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX)
        return SIZE_MAX;
    return bucket_index_for_hash(hash_path(path, length));
}
#endif
