#include "myfs.h"

#include <poll.h>

#ifdef MYFS_TEST_FAILPOINTS
#include "core/compact_test.h"
#endif

#define COMPACT_THRESHOLD 0.25
#define ADAPTIVE_MIN_SAMPLE 128ULL
#define ADAPTIVE_COOLDOWN_SECONDS 3600

enum adaptive_state
{
    ADAPTIVE_IDLE = 0,
    ADAPTIVE_QUEUED,
    ADAPTIVE_INFLIGHT,
    ADAPTIVE_COOLDOWN_DEFERRED,
};

enum generation_gc_state
{
    GENERATION_GC_NONE = 0,
    GENERATION_GC_PENDING,
    GENERATION_GC_CLAIMED,
};

enum
{
    MYFS_REGISTRY_SHARD_COUNT = 64,
    MYFS_REGISTRY_BUCKET_COUNT = 64,
};

_Static_assert((MYFS_REGISTRY_SHARD_COUNT &
                (MYFS_REGISTRY_SHARD_COUNT - 1)) == 0,
               "registry shard count must be a power of two");
_Static_assert((MYFS_REGISTRY_BUCKET_COUNT &
                (MYFS_REGISTRY_BUCKET_COUNT - 1)) == 0,
               "registry bucket count must be a power of two");

int myfs_choose_resize_target(uint32_t current_window, uint64_t live_bytes,
                              myfs_window_stats_t stats, bool in_cooldown,
                              uint32_t *target_window)
{
    if (!target_window || !myfs_window_size_valid(current_window))
        return -EINVAL;
    *target_window = current_window;
    uint64_t sample = stats.full_windows;
    if (UINT64_MAX - sample < stats.partial_rmw)
        sample = UINT64_MAX;
    else
        sample += stats.partial_rmw;
    if (in_cooldown || sample < ADAPTIVE_MIN_SAMPLE)
        return 0;

    uint32_t candidate = current_window;
    uint64_t signal_events = 0;
    if ((__uint128_t)stats.partial_rmw * 100 >=
        (__uint128_t)sample * 60)
    {
        if (current_window <= MYFS_MIN_WINDOW_SIZE)
            return 0;
        candidate = current_window / 2;
        signal_events = stats.partial_rmw;
    }
    else if ((__uint128_t)stats.full_windows * 100 >=
             (__uint128_t)sample * 90)
    {
        if (current_window >= MYFS_MAX_WINDOW_SIZE)
            return 0;
        candidate = current_window * 2;
        signal_events = stats.full_windows;
    }
    else
        return 0;

    uint64_t step = candidate > current_window
        ? candidate - current_window : current_window - candidate;
    uint64_t expected_benefit = signal_events > UINT64_MAX / step
        ? UINT64_MAX : signal_events * step;
    if (expected_benefit < live_bytes)
        return 0;
    *target_window = candidate;
    return 1;
}

struct myfs_generation_record
{
    myfs_storage_t storage;
    unsigned open_refs;
    unsigned writer_refs;
    uint64_t metadata_epoch;
    bool superseded;
    myfs_file_handle_t *handles;
    myfs_window_stats_t adaptive_stats;
    uint64_t classified_since_evaluation;
    uint64_t adaptive_eval_id;
    enum adaptive_state adaptive_state;
    bool release_requested;
    bool last_resize_valid;
    struct timespec last_resize_mono;
    struct timespec cooldown_until_mono;
    enum generation_gc_state gc_state;
    uint64_t gc_claim_id;
    bool install_aliases;
    uint64_t path_hash;
    size_t shard_index;
    size_t bucket_index;
    struct myfs_generation_record *previous;
    struct myfs_generation_record *next;
};

struct generation_registry_shard
{
    pthread_mutex_t mu;
    struct myfs_generation_record *buckets[MYFS_REGISTRY_BUCKET_COUNT];
};

struct generation_registry_location
{
    uint64_t path_hash;
    size_t shard_index;
    size_t bucket_index;
    struct generation_registry_shard *shard;
};

static struct generation_registry_shard
    generation_registry_shards[MYFS_REGISTRY_SHARD_COUNT];
static pthread_once_t generation_registry_once = PTHREAD_ONCE_INIT;
static int generation_registry_init_status;

/* Each shard mutex protects its buckets, links, and record fields. Registry-
 * only operations may take a shard without a path lock. When locks nest, take
 * the path lock and cache_lock first; never wait for either while holding a
 * registry shard. Filesystem I/O and compaction-queue operations also run
 * without a registry shard held. */

static uint64_t generation_registry_hash_path(const char *path, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++)
    {
        hash ^= (unsigned char)path[i];
        hash *= UINT64_C(1099511628211);
    }

    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    hash *= UINT64_C(0xc4ceb9fe1a85ec53);
    hash ^= hash >> 33;
    return hash;
}

static void initialize_generation_registry(void)
{
    size_t initialized = 0;
    for (; initialized < MYFS_REGISTRY_SHARD_COUNT; initialized++)
    {
        int status = pthread_mutex_init(
            &generation_registry_shards[initialized].mu, NULL);
        if (status != 0)
        {
            generation_registry_init_status = status;
            while (initialized > 0)
            {
                initialized--;
                pthread_mutex_destroy(
                    &generation_registry_shards[initialized].mu);
            }
            return;
        }
    }
}

static int ensure_generation_registry_initialized(void)
{
    int status = pthread_once(&generation_registry_once,
                              initialize_generation_registry);
    return status != 0 ? status : generation_registry_init_status;
}

static int generation_registry_location_for_path(
    const char *path, struct generation_registry_location *location)
{
    if (!path || !location)
        return EINVAL;
    int status = ensure_generation_registry_initialized();
    if (status != 0)
        return status;
    size_t length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX)
        return ENAMETOOLONG;
    uint64_t hash = generation_registry_hash_path(path, length);
    location->path_hash = hash;
    location->shard_index =
        (size_t)hash & (MYFS_REGISTRY_SHARD_COUNT - 1);
    location->bucket_index =
        ((size_t)(hash >> 6)) & (MYFS_REGISTRY_BUCKET_COUNT - 1);
    location->shard =
        &generation_registry_shards[location->shard_index];
    return 0;
}

struct compact_request
{
    char *path;
    bool ordinary;
    bool adaptive;
    myfs_adaptive_ticket_t adaptive_ticket;
    struct compact_request *next;
};

#ifdef MYFS_TEST_FAILPOINTS
static void compact_test_failpoint(const char *path, const char *name,
                                   const myfs_storage_t *storage)
{
    const char *selected = getenv("MYFS_TEST_COMPACT_FAILPOINT");
    const char *selected_path = getenv("MYFS_TEST_FAILPOINT_PATH");
    if (!selected || strcmp(selected, name) != 0 ||
        (selected_path && strcmp(selected_path, path) != 0))
        return;

    LOG("[TEST] compact failpoint=%s path=%s generation=%s pid=%ld\n",
        name, path, storage ? storage->generation_id : "none", (long)getpid());
    fflush(stderr);
    for (;;)
        poll(NULL, 0, -1);
}
#else
static void compact_test_failpoint(const char *path, const char *name,
                                   const myfs_storage_t *storage)
{
    (void)path;
    (void)name;
    (void)storage;
}
#endif

static struct myfs_generation_record *find_generation_record_locked(
    struct generation_registry_shard *shard, size_t bucket_index,
    const myfs_storage_t *storage)
{
    for (struct myfs_generation_record *record = shard->buckets[bucket_index];
         record; record = record->next)
    {
        if (storage_generation_equal(&record->storage, storage))
            return record;
    }
    return NULL;
}

static struct myfs_generation_record *allocate_generation_record(
    const struct generation_registry_location *location,
    const myfs_storage_t *storage)
{
    struct myfs_generation_record *record = calloc(1, sizeof(*record));
    if (!record)
        return NULL;
    record->storage = *storage;
    record->metadata_epoch = 1;
    record->path_hash = location->path_hash;
    record->shard_index = location->shard_index;
    record->bucket_index = location->bucket_index;
    return record;
}

static void insert_generation_record_locked(
    const struct generation_registry_location *location,
    struct myfs_generation_record *record)
{
    record->next = location->shard->buckets[location->bucket_index];
    if (record->next)
        record->next->previous = record;
    location->shard->buckets[location->bucket_index] = record;
}

static struct myfs_generation_record *get_generation_record_locked(
    const struct generation_registry_location *location,
    const myfs_storage_t *storage)
{
    struct myfs_generation_record *record = find_generation_record_locked(
        location->shard, location->bucket_index, storage);
    if (record)
        return record;

    record = allocate_generation_record(location, storage);
    if (!record)
        return NULL;
    insert_generation_record_locked(location, record);
    return record;
}

static void unlink_generation_record_locked(
    struct generation_registry_shard *shard,
    struct myfs_generation_record *record)
{
    if (record->previous)
        record->previous->next = record->next;
    else
        shard->buckets[record->bucket_index] = record->next;
    if (record->next)
        record->next->previous = record->previous;
    record->previous = NULL;
    record->next = NULL;
}

int register_generation_handle_locked(myfs_file_handle_t *handle)
{
    struct stat st;
    if (fstat(handle->data_fd, &st) == 0)
    {
        handle->storage.data_dev = st.st_dev;
        handle->storage.data_ino = st.st_ino;
    }

    struct generation_registry_location location;
    int status = generation_registry_location_for_path(
        handle->storage.logical_path, &location);
    if (status != 0)
        return -status;
    status = pthread_mutex_lock(&location.shard->mu);
    if (status != 0)
        return -status;
    struct myfs_generation_record *record = get_generation_record_locked(
        &location, &handle->storage);
    if (!record)
    {
        pthread_mutex_unlock(&location.shard->mu);
        return -ENOMEM;
    }

    record->open_refs++;
    if ((handle->flags & O_ACCMODE) != O_RDONLY)
        record->writer_refs++;
    handle->generation_record = record;
    handle->registry_prev = NULL;
    handle->registry_next = record->handles;
    if (record->handles)
        record->handles->registry_prev = handle;
    record->handles = handle;
    handle->seen_metadata_epoch = record->metadata_epoch;
    pthread_mutex_unlock(&location.shard->mu);
    return 0;
}

void unregister_generation_handle_locked(myfs_file_handle_t *handle)
{
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
        return;

    struct generation_registry_shard *shard =
        &generation_registry_shards[record->shard_index];
    if (pthread_mutex_lock(&shard->mu) != 0)
        return;
    if (record->open_refs > 0)
        record->open_refs--;
    if ((handle->flags & O_ACCMODE) != O_RDONLY && record->writer_refs > 0)
        record->writer_refs--;
    if (handle->registry_prev)
        handle->registry_prev->registry_next = handle->registry_next;
    else if (record->handles == handle)
        record->handles = handle->registry_next;
    if (handle->registry_next)
        handle->registry_next->registry_prev = handle->registry_prev;
    handle->registry_prev = NULL;
    handle->registry_next = NULL;
    handle->generation_record = NULL;

    bool reclaim = false;
    if (record->open_refs == 0 &&
        record->gc_state == GENERATION_GC_NONE &&
        record->adaptive_stats.full_windows == 0 &&
        record->adaptive_stats.partial_rmw == 0 &&
        record->adaptive_state == ADAPTIVE_IDLE &&
        !record->last_resize_valid)
    {
        unlink_generation_record_locked(shard, record);
        reclaim = true;
    }
    pthread_mutex_unlock(&shard->mu);
    if (reclaim)
        free(record);
}

void generation_state_snapshot(myfs_file_handle_t *handle,
                               uint64_t *metadata_epoch, bool *superseded)
{
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
    {
        if (metadata_epoch)
            *metadata_epoch = 0;
        if (superseded)
            *superseded = true;
        return;
    }
    struct generation_registry_shard *shard =
        &generation_registry_shards[record->shard_index];
    if (pthread_mutex_lock(&shard->mu) != 0)
    {
        if (metadata_epoch)
            *metadata_epoch = 0;
        if (superseded)
            *superseded = true;
        return;
    }
    record = handle->generation_record;
    if (metadata_epoch)
        *metadata_epoch = record ? record->metadata_epoch : 0;
    if (superseded)
        *superseded = record ? record->superseded : true;
    pthread_mutex_unlock(&shard->mu);
}

uint64_t generation_bump_metadata_epoch_locked(myfs_file_handle_t *handle)
{
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
        return 0;
    struct generation_registry_shard *shard =
        &generation_registry_shards[record->shard_index];
    if (pthread_mutex_lock(&shard->mu) != 0)
        return 0;
    record = handle->generation_record;
    uint64_t epoch = 0;
    if (record)
    {
        record->metadata_epoch++;
        if (record->metadata_epoch == 0)
            record->metadata_epoch = 1;
        epoch = record->metadata_epoch;
    }
    pthread_mutex_unlock(&shard->mu);
    return epoch;
}

void generation_bump_storage_epoch_locked(const myfs_storage_t *storage)
{
    struct generation_registry_location location;
    if (generation_registry_location_for_path(storage->logical_path,
                                              &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, storage);
    if (record)
    {
        record->metadata_epoch++;
        if (record->metadata_epoch == 0)
            record->metadata_epoch = 1;
    }
    pthread_mutex_unlock(&location.shard->mu);
}

static uint64_t add_saturating(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static int timespec_compare(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec)
        return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static uint64_t stats_sample(myfs_window_stats_t stats)
{
    return add_saturating(stats.full_windows, stats.partial_rmw);
}

static void process_expired_cooldown(struct myfs_generation_record *record,
                                     const struct timespec *now)
{
    if (!record->last_resize_valid ||
        timespec_compare(now, &record->cooldown_until_mono) < 0)
        return;
    if (record->adaptive_state == ADAPTIVE_COOLDOWN_DEFERRED)
    {
        record->adaptive_stats.full_windows /= 2;
        record->adaptive_stats.partial_rmw /= 2;
        record->classified_since_evaluation =
            stats_sample(record->adaptive_stats);
        record->adaptive_state = ADAPTIVE_IDLE;
    }
    record->last_resize_valid = false;
}

static bool claim_adaptive_ticket(struct myfs_generation_record *record,
                                  myfs_adaptive_ticket_t *ticket)
{
    if (!ticket || record->adaptive_state != ADAPTIVE_IDLE ||
        stats_sample(record->adaptive_stats) < ADAPTIVE_MIN_SAMPLE ||
        record->classified_since_evaluation < ADAPTIVE_MIN_SAMPLE)
        return false;
    memset(ticket, 0, sizeof(*ticket));
    ticket->valid = true;
    ticket->source_storage = record->storage;
    ticket->stats_snapshot = record->adaptive_stats;
    ticket->evaluation_id = ++record->adaptive_eval_id;
    record->adaptive_stats = (myfs_window_stats_t){0};
    record->classified_since_evaluation = 0;
    record->adaptive_state = ADAPTIVE_QUEUED;
    return true;
}

static void restore_adaptive_ticket_locked(
    struct myfs_generation_record *record,
    const myfs_adaptive_ticket_t *ticket, bool restore_evidence)
{
    record->adaptive_stats.full_windows = add_saturating(
        record->adaptive_stats.full_windows,
        ticket->stats_snapshot.full_windows);
    record->adaptive_stats.partial_rmw = add_saturating(
        record->adaptive_stats.partial_rmw,
        ticket->stats_snapshot.partial_rmw);
    if (restore_evidence)
        record->classified_since_evaluation = add_saturating(
            record->classified_since_evaluation,
            stats_sample(ticket->stats_snapshot));
    record->adaptive_state = ADAPTIVE_IDLE;
}

bool generation_observe_write_locked(myfs_file_handle_t *handle,
                                     uint64_t full_windows,
                                     uint64_t partial_rmw,
                                     myfs_adaptive_ticket_t *ticket)
{
    if (ticket)
        memset(ticket, 0, sizeof(*ticket));
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
        return false;
    struct generation_registry_shard *shard =
        &generation_registry_shards[record->shard_index];
    if (pthread_mutex_lock(&shard->mu) != 0)
        return false;
    record = handle->generation_record;
    if (!record)
    {
        pthread_mutex_unlock(&shard->mu);
        return false;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    process_expired_cooldown(record, &now);
    record->adaptive_stats.full_windows = add_saturating(
        record->adaptive_stats.full_windows, full_windows);
    record->adaptive_stats.partial_rmw = add_saturating(
        record->adaptive_stats.partial_rmw, partial_rmw);
    record->classified_since_evaluation = add_saturating(
        record->classified_since_evaluation,
        add_saturating(full_windows, partial_rmw));

    if (record->last_resize_valid &&
        timespec_compare(&now, &record->cooldown_until_mono) < 0)
    {
        if (record->adaptive_state == ADAPTIVE_IDLE)
            record->adaptive_state = ADAPTIVE_COOLDOWN_DEFERRED;
        pthread_mutex_unlock(&shard->mu);
        return false;
    }
    bool claimed = claim_adaptive_ticket(record, ticket);
    pthread_mutex_unlock(&shard->mu);
    return claimed;
}

bool generation_claim_release_locked(myfs_file_handle_t *handle,
                                     myfs_adaptive_ticket_t *ticket)
{
    if (ticket)
        memset(ticket, 0, sizeof(*ticket));
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
        return false;
    struct generation_registry_shard *shard =
        &generation_registry_shards[record->shard_index];
    if (pthread_mutex_lock(&shard->mu) != 0)
        return false;
    record = handle->generation_record;
    bool claimed = false;
    if (record && (handle->flags & O_ACCMODE) != O_RDONLY &&
        record->writer_refs == 1)
    {
        record->release_requested = true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        process_expired_cooldown(record, &now);
        if (!record->last_resize_valid ||
            timespec_compare(&now, &record->cooldown_until_mono) >= 0)
            claimed = claim_adaptive_ticket(record, ticket);
    }
    pthread_mutex_unlock(&shard->mu);
    return claimed;
}

void generation_adaptive_schedule_failed(const myfs_adaptive_ticket_t *ticket)
{
    if (!ticket || !ticket->valid)
        return;
    struct generation_registry_location location;
    if (generation_registry_location_for_path(
            ticket->source_storage.logical_path, &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, &ticket->source_storage);
    if (record && record->adaptive_eval_id == ticket->evaluation_id &&
        record->adaptive_state == ADAPTIVE_QUEUED)
        restore_adaptive_ticket_locked(record, ticket, true);
    /* A CLAIMED record cannot be freed before GC finalization.  If restoration
     * wins the shard, a failed/deferred finalize preserves it; successful
     * retirement makes it moot.  If finalize wins first, this lookup either
     * finds the retained PENDING record or safely misses a retired record. */
    pthread_mutex_unlock(&location.shard->mu);
}

unsigned generation_writer_refs_locked(const myfs_storage_t *storage)
{
    struct generation_registry_location location;
    if (generation_registry_location_for_path(storage->logical_path,
                                              &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return 0;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, storage);
    unsigned refs = record ? record->writer_refs : 0;
    pthread_mutex_unlock(&location.shard->mu);
    return refs;
}

unsigned generation_open_refs_locked(const myfs_storage_t *storage)
{
    struct generation_registry_location location;
    if (generation_registry_location_for_path(storage->logical_path,
                                              &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return 0;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, storage);
    unsigned refs = record ? record->open_refs : 0;
    pthread_mutex_unlock(&location.shard->mu);
    return refs;
}

static bool generation_has_queued_adaptive_locked(
    const myfs_storage_t *storage)
{
    struct generation_registry_location location;
    if (generation_registry_location_for_path(storage->logical_path,
                                              &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return false;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, storage);
    bool queued = record && record->adaptive_state == ADAPTIVE_QUEUED;
    pthread_mutex_unlock(&location.shard->mu);
    return queued;
}

int mark_generation_for_gc_locked(const myfs_storage_t *storage,
                                  bool install_aliases)
{
    struct generation_registry_location location;
    int status = generation_registry_location_for_path(storage->logical_path,
                                                       &location);
    if (status != 0)
        return -status;
    status = pthread_mutex_lock(&location.shard->mu);
    if (status != 0)
        return -status;
    struct myfs_generation_record *record = get_generation_record_locked(
        &location, storage);
    if (!record)
    {
        pthread_mutex_unlock(&location.shard->mu);
        return -ENOMEM;
    }
    if (record->gc_state == GENERATION_GC_NONE)
        record->gc_state = GENERATION_GC_PENDING;
    record->install_aliases = record->install_aliases || install_aliases;
    pthread_mutex_unlock(&location.shard->mu);
    return 0;
}

enum generation_gc_hook_stage
{
    GENERATION_GC_HOOK_BEFORE_RESOLVE = 0,
    GENERATION_GC_HOOK_BEFORE_ALIAS,
    GENERATION_GC_HOOK_BEFORE_REMOVE,
    GENERATION_GC_HOOK_BEFORE_FINALIZE,
};

enum generation_handoff_hook_stage
{
    GENERATION_HANDOFF_HOOK_BEFORE_PUBLISH = 0,
    GENERATION_HANDOFF_HOOK_AFTER_VISIBLE_PUBLISH,
    GENERATION_HANDOFF_HOOK_BEFORE_RECORD_CREATE,
    GENERATION_HANDOFF_HOOK_AFTER_FIRST_WRITER_RELINK,
    GENERATION_HANDOFF_HOOK_AFTER_REGISTRY_TRANSACTION,
};

#ifdef MYFS_TEST_FAILPOINTS
static myfs_generation_gc_test_hook_fn generation_gc_test_hook;
static void *generation_gc_test_hook_context;
static myfs_generation_handoff_test_hook_fn generation_handoff_test_hook;
static void *generation_handoff_test_hook_context;

void myfs_generation_registry_test_set_gc_hook(
    myfs_generation_gc_test_hook_fn hook, void *context)
{
    generation_gc_test_hook = hook;
    generation_gc_test_hook_context = context;
}

void myfs_generation_registry_test_set_handoff_hook(
    myfs_generation_handoff_test_hook_fn hook, void *context)
{
    generation_handoff_test_hook = hook;
    generation_handoff_test_hook_context = context;
}

static int run_generation_gc_test_hook(enum generation_gc_hook_stage stage,
                                       const myfs_storage_t *storage)
{
    if (!generation_gc_test_hook)
        return 0;
    return generation_gc_test_hook(
        (enum myfs_generation_gc_test_stage)stage, storage,
        generation_gc_test_hook_context);
}

static int run_generation_handoff_test_hook(
    enum generation_handoff_hook_stage stage,
    const myfs_storage_t *old_storage,
    const myfs_storage_t *new_storage)
{
    if (!generation_handoff_test_hook)
        return 0;
    return generation_handoff_test_hook(
        (enum myfs_generation_handoff_test_stage)stage,
        old_storage, new_storage,
        generation_handoff_test_hook_context);
}
#else
static int run_generation_gc_test_hook(enum generation_gc_hook_stage stage,
                                       const myfs_storage_t *storage)
{
    (void)stage;
    (void)storage;
    return 0;
}

static int run_generation_handoff_test_hook(
    enum generation_handoff_hook_stage stage,
    const myfs_storage_t *old_storage,
    const myfs_storage_t *new_storage)
{
    (void)stage;
    (void)old_storage;
    (void)new_storage;
    return 0;
}
#endif

struct generation_gc_work
{
    struct myfs_generation_record *record;
    size_t shard_index;
    size_t bucket_index;
    uint64_t claim_id;
    myfs_storage_t victim;
    unsigned open_refs_at_claim;
    bool install_aliases;
    bool has_legacy_refs;
};

enum generation_gc_completion
{
    GENERATION_GC_RETRY = 0,
    GENERATION_GC_DEFERRED,
    GENERATION_GC_ALIAS_ONLY,
    GENERATION_GC_RETIRED,
};

struct generation_gc_result
{
    enum generation_gc_completion completion;
    int error;
};

static bool generation_gc_record_matches(
    const struct myfs_generation_record *record, const char *path)
{
    return record->gc_state == GENERATION_GC_PENDING &&
           (!path || strcmp(record->storage.logical_path, path) == 0);
}

static size_t generation_gc_pending_count_locked(
    struct generation_registry_shard *shard, size_t first_bucket,
    size_t bucket_count, const char *path)
{
    size_t count = 0;
    for (size_t i = 0; i < bucket_count; i++)
    {
        size_t bucket_index = first_bucket + i;
        for (struct myfs_generation_record *record =
                 shard->buckets[bucket_index];
             record; record = record->next)
        {
            if (generation_gc_record_matches(record, path))
                count++;
        }
    }
    return count;
}

static bool generation_has_legacy_refs_for_path_locked(
    struct generation_registry_shard *shard, size_t bucket_index,
    const char *path)
{
    for (struct myfs_generation_record *record = shard->buckets[bucket_index];
         record; record = record->next)
    {
        if (record->storage.is_legacy && record->open_refs > 0 &&
            strcmp(record->storage.logical_path, path) == 0)
            return true;
    }
    return false;
}

static int claim_generation_gc_batch(
    size_t shard_index, size_t first_bucket, size_t bucket_count,
    const char *path, struct generation_gc_work **out_work,
    size_t *out_count)
{
    struct generation_registry_shard *shard =
        &generation_registry_shards[shard_index];
    struct generation_gc_work *work = NULL;
    size_t capacity = 0;

    *out_work = NULL;
    *out_count = 0;
    for (;;)
    {
        int status = pthread_mutex_lock(&shard->mu);
        if (status != 0)
        {
            free(work);
            return -status;
        }
        size_t needed = generation_gc_pending_count_locked(
            shard, first_bucket, bucket_count, path);
        if (needed > capacity)
        {
            pthread_mutex_unlock(&shard->mu);
            if (needed > SIZE_MAX / sizeof(*work))
            {
                free(work);
                return -EOVERFLOW;
            }
            struct generation_gc_work *replacement = realloc(
                work, needed * sizeof(*work));
            if (!replacement)
            {
                free(work);
                return -ENOMEM;
            }
            work = replacement;
            capacity = needed;
            continue;
        }

        size_t used = 0;
        int first_error = 0;
        for (size_t i = 0; i < bucket_count; i++)
        {
            size_t bucket_index = first_bucket + i;
            for (struct myfs_generation_record *record =
                     shard->buckets[bucket_index];
                 record; record = record->next)
            {
                if (!generation_gc_record_matches(record, path))
                    continue;
                if (record->gc_claim_id == UINT64_MAX)
                {
                    if (first_error == 0)
                        first_error = -EOVERFLOW;
                    continue;
                }

                record->gc_claim_id++;
                record->gc_state = GENERATION_GC_CLAIMED;
                work[used++] = (struct generation_gc_work){
                    .record = record,
                    .shard_index = shard_index,
                    .bucket_index = bucket_index,
                    .claim_id = record->gc_claim_id,
                    .victim = record->storage,
                    .open_refs_at_claim = record->open_refs,
                    .install_aliases = record->install_aliases,
                    .has_legacy_refs =
                        generation_has_legacy_refs_for_path_locked(
                            shard, bucket_index,
                            record->storage.logical_path),
                };
            }
        }
        pthread_mutex_unlock(&shard->mu);
        *out_work = work;
        *out_count = used;
        return first_error;
    }
}

static struct generation_gc_result perform_generation_gc_io(
    const struct generation_gc_work *work)
{
    myfs_storage_t active;
    int ret = run_generation_gc_test_hook(
        GENERATION_GC_HOOK_BEFORE_RESOLVE, &work->victim);
    if (ret == 0)
        ret = resolve_storage(work->victim.logical_path, &active);
    if (ret != 0)
        return (struct generation_gc_result){GENERATION_GC_RETRY, ret};

    if (storage_generation_equal(&work->victim, &active))
    {
        LOG("[ERROR] GC refused to remove active generation %s for %s\n",
            work->victim.generation_id, work->victim.logical_path);
        return (struct generation_gc_result){GENERATION_GC_RETRY, -EBUSY};
    }

    if (work->install_aliases && work->has_legacy_refs)
    {
        compact_test_failpoint(work->victim.logical_path,
                               "gc_deferred_old_ref", &work->victim);
        return (struct generation_gc_result){GENERATION_GC_DEFERRED, 0};
    }

    if (work->install_aliases)
    {
        if (active.is_legacy)
            return (struct generation_gc_result){GENERATION_GC_RETRY, -EIO};
        ret = run_generation_gc_test_hook(
            GENERATION_GC_HOOK_BEFORE_ALIAS, &work->victim);
        if (ret == 0)
            ret = install_generation_aliases(work->victim.logical_path,
                                             &active);
        if (ret != 0)
            return (struct generation_gc_result){GENERATION_GC_RETRY, ret};
    }

    if (work->victim.is_legacy && !work->install_aliases)
        return (struct generation_gc_result){GENERATION_GC_RETRY, -EIO};

    if (work->open_refs_at_claim > 0)
    {
        compact_test_failpoint(work->victim.logical_path,
                               "gc_deferred_old_ref", &work->victim);
        return (struct generation_gc_result){
            work->install_aliases ? GENERATION_GC_ALIAS_ONLY
                                  : GENERATION_GC_DEFERRED,
            0,
        };
    }

    if (work->victim.is_legacy)
        return (struct generation_gc_result){GENERATION_GC_RETIRED, 0};

    ret = run_generation_gc_test_hook(
        GENERATION_GC_HOOK_BEFORE_REMOVE, &work->victim);
    if (ret == 0)
        ret = remove_generation_storage(&work->victim);
    return (struct generation_gc_result){
        ret == 0 ? GENERATION_GC_RETIRED : GENERATION_GC_RETRY,
        ret,
    };
}

static int finalize_generation_gc_work(
    const struct generation_gc_work *work,
    const struct generation_gc_result *result)
{
    struct generation_registry_shard *shard =
        &generation_registry_shards[work->shard_index];
    int status = pthread_mutex_lock(&shard->mu);
    if (status != 0)
        return -status;
    struct myfs_generation_record *record = find_generation_record_locked(
        shard, work->bucket_index, &work->victim);
    if (record != work->record || !record ||
        record->gc_state != GENERATION_GC_CLAIMED ||
        record->gc_claim_id != work->claim_id)
    {
        pthread_mutex_unlock(&shard->mu);
        return -EIO;
    }

    bool retired = false;
    int ret = 0;
    if (result->completion == GENERATION_GC_RETIRED)
    {
        if (record->open_refs != 0)
        {
            record->gc_state = GENERATION_GC_PENDING;
            ret = -EBUSY;
        }
        else if (record->install_aliases != work->install_aliases)
        {
            record->gc_state = GENERATION_GC_PENDING;
            ret = -EAGAIN;
        }
        else
        {
            unlink_generation_record_locked(shard, record);
            retired = true;
        }
    }
    else
        record->gc_state = GENERATION_GC_PENDING;
    pthread_mutex_unlock(&shard->mu);

    if (retired)
    {
        LOG("[DEBUG] GC removed generation %s for %s\n",
            work->victim.generation_id, work->victim.logical_path);
        free(record);
    }
    return ret;
}

static int run_generation_gc_for_shard(size_t shard_index,
                                       size_t first_bucket,
                                       size_t bucket_count,
                                       const char *path)
{
    struct generation_gc_work *work = NULL;
    size_t work_count = 0;
    int first_error = claim_generation_gc_batch(
        shard_index, first_bucket, bucket_count, path, &work, &work_count);

    for (size_t i = 0; i < work_count; i++)
    {
        struct generation_gc_result result =
            perform_generation_gc_io(&work[i]);
        (void)run_generation_gc_test_hook(
            GENERATION_GC_HOOK_BEFORE_FINALIZE, &work[i].victim);
        int finalize_ret = finalize_generation_gc_work(&work[i], &result);
        if (first_error == 0 && result.error != 0)
            first_error = result.error;
        if (first_error == 0 && finalize_ret != 0)
            first_error = finalize_ret;
    }
    free(work);
    return first_error;
}

/* Caller holds the file lock for path, or passes NULL only after all registry
 * users have quiesced. Claim and finalization mutate records under one shard;
 * resolution, alias installation, removal, logging, and failpoints run after
 * releasing it. Normal compaction marks an old generation only after durable
 * publication; recovery may also mark inactive owned generations. */
int run_generation_gc_locked(const char *path)
{
    int status = ensure_generation_registry_initialized();
    if (status != 0)
        return -status;
    if (path)
    {
        struct generation_registry_location location;
        status = generation_registry_location_for_path(path, &location);
        if (status != 0)
            return -status;
        return run_generation_gc_for_shard(
            location.shard_index, location.bucket_index, 1, path);
    }

    int first_error = 0;
    for (size_t shard_index = 0;
         shard_index < MYFS_REGISTRY_SHARD_COUNT; shard_index++)
    {
        int ret = run_generation_gc_for_shard(
            shard_index, 0, MYFS_REGISTRY_BUCKET_COUNT, NULL);
        if (first_error == 0 && ret != 0)
            first_error = ret;
    }
    return first_error;
}

static bool same_backing_inode(const char *a, const char *b)
{
    struct stat a_st;
    struct stat b_st;
    return stat(a, &a_st) == 0 && stat(b, &b_st) == 0 &&
           a_st.st_dev == b_st.st_dev && a_st.st_ino == b_st.st_ino;
}

static int make_legacy_candidate(const char *path, myfs_storage_t *storage)
{
    memset(storage, 0, sizeof(*storage));
    if (snprintf(storage->logical_path, PATH_MAX, "%s", path) >= PATH_MAX)
        return -ENAMETOOLONG;
    strcpy(storage->generation_id, "legacy");
    storage->is_legacy = true;
    build_data_path(storage->data_path, path);
    build_meta_path(storage->meta_path, path);
    struct stat st;
    if (stat(storage->data_path, &st) == 0)
    {
        storage->data_dev = st.st_dev;
        storage->data_ino = st.st_ino;
    }
    return 0;
}

/* Lazily recover unpublished/superseded generations on the first access after
 * a daemon restart.  After restart no old kernel FUSE handles survive, while
 * live handles in this process are represented by the registry and are kept. */
int recover_generations_for_path_locked(const char *path,
                                        const myfs_storage_t *active)
{
    char base_path[PATH_MAX];
    build_path(base_path, path);
    if (base_path[0] == '\0')
        return -ENAMETOOLONG;

    /* A pointer that survived a crash is not a GC authority until its parent
     * directory has been synced successfully in this process. */
    if (!active->is_legacy)
    {
        char current_path[PATH_MAX];
        build_current_path(current_path, path);
        int sync_ret = fsync_parent_path(current_path);
        if (sync_ret != 0)
            return sync_ret;
    }

    char parent[PATH_MAX];
    char base_name_buf[NAME_MAX + 1];
    if (snprintf(parent, PATH_MAX, "%s", base_path) >= PATH_MAX)
        return -ENAMETOOLONG;
    char *slash = strrchr(parent, '/');
    const char *base_name = slash ? slash + 1 : base_path;
    if (snprintf(base_name_buf, sizeof(base_name_buf), "%s", base_name)
        >= (int)sizeof(base_name_buf))
        return -ENAMETOOLONG;
    if (slash == parent)
        slash[1] = '\0';
    else if (slash)
        *slash = '\0';
    else
        strcpy(parent, ".");

    char generation_prefix[NAME_MAX + 1];
    if (snprintf(generation_prefix, sizeof(generation_prefix), "%s.g.", base_name_buf)
            >= (int)sizeof(generation_prefix))
        return -ENAMETOOLONG;

    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_data_path(data_alias, path);
    build_meta_path(meta_alias, path);
    bool aliases_current = active->is_legacy ||
        (same_backing_inode(active->data_path, data_alias) &&
         same_backing_inode(active->meta_path, meta_alias));

    if (!active->is_legacy && !aliases_current)
    {
        myfs_storage_t legacy;
        int ret = make_legacy_candidate(path, &legacy);
        if (ret != 0)
            return ret;
        ret = mark_generation_for_gc_locked(&legacy, true);
        if (ret != 0)
            return ret;
    }

    DIR *dir = opendir(parent);
    if (!dir)
        return -errno;

    int ret = 0;
    struct dirent *entry;
    size_t generation_prefix_len = strlen(generation_prefix);
    while ((entry = readdir(dir)) != NULL)
    {
        const char *name = entry->d_name;
        if (strncmp(name, generation_prefix, generation_prefix_len) != 0 ||
            strlen(name) != generation_prefix_len + MYFS_GENERATION_HEX_LEN)
            continue;

        const char *id = name + generation_prefix_len;
        myfs_storage_t candidate;
        if (build_generation_storage(path, id, &candidate) != 0)
            continue;

        struct stat st;
        if (lstat(candidate.generation_dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
            validate_generation_marker(&candidate) != 0)
            continue;
        if (!storage_generation_equal(active, &candidate))
        {
            int mark_ret = mark_generation_for_gc_locked(&candidate,
                                                         !active->is_legacy);
            if (mark_ret != 0 && ret == 0)
                ret = mark_ret;
        }
    }
    closedir(dir);

    int gc_ret = run_generation_gc_locked(path);
    if (gc_ret != 0 && ret == 0)
        ret = gc_ret;
    return ret;
}

void destroy_generation_registry(void)
{
    if (ensure_generation_registry_initialized() != 0)
        return;
    for (size_t shard_index = 0;
         shard_index < MYFS_REGISTRY_SHARD_COUNT; shard_index++)
    {
        struct generation_registry_shard *shard =
            &generation_registry_shards[shard_index];
        if (pthread_mutex_lock(&shard->mu) != 0)
            continue;
        struct myfs_generation_record *detached = NULL;
        for (size_t bucket_index = 0;
             bucket_index < MYFS_REGISTRY_BUCKET_COUNT; bucket_index++)
        {
            struct myfs_generation_record *record =
                shard->buckets[bucket_index];
            shard->buckets[bucket_index] = NULL;
            while (record)
            {
                struct myfs_generation_record *next = record->next;
                record->previous = NULL;
                record->next = detached;
                detached = record;
                record = next;
            }
        }
        pthread_mutex_unlock(&shard->mu);
        while (detached)
        {
            struct myfs_generation_record *next = detached->next;
            free(detached);
            detached = next;
        }
    }
}

#ifdef MYFS_TEST_FAILPOINTS
size_t myfs_generation_registry_test_shard_index(const char *path)
{
    struct generation_registry_location location;
    return generation_registry_location_for_path(path, &location) == 0
        ? location.shard_index : SIZE_MAX;
}

size_t myfs_generation_registry_test_bucket_index(const char *path)
{
    struct generation_registry_location location;
    return generation_registry_location_for_path(path, &location) == 0
        ? location.bucket_index : SIZE_MAX;
}

size_t myfs_generation_registry_test_record_count(void)
{
    if (ensure_generation_registry_initialized() != 0)
        return 0;
    size_t count = 0;
    for (size_t shard_index = 0;
         shard_index < MYFS_REGISTRY_SHARD_COUNT; shard_index++)
    {
        struct generation_registry_shard *shard =
            &generation_registry_shards[shard_index];
        if (pthread_mutex_lock(&shard->mu) != 0)
            continue;
        for (size_t bucket_index = 0;
             bucket_index < MYFS_REGISTRY_BUCKET_COUNT; bucket_index++)
        {
            for (struct myfs_generation_record *record =
                     shard->buckets[bucket_index];
                 record; record = record->next)
                count++;
        }
        pthread_mutex_unlock(&shard->mu);
    }
    return count;
}

int myfs_generation_registry_test_try_path_shard(const char *path)
{
    struct generation_registry_location location;
    int status = generation_registry_location_for_path(path, &location);
    if (status != 0)
        return -status;
    status = pthread_mutex_trylock(&location.shard->mu);
    if (status != 0)
        return -status;
    pthread_mutex_unlock(&location.shard->mu);
    return 0;
}

int myfs_generation_registry_test_snapshot(
    const myfs_storage_t *storage,
    struct myfs_generation_registry_test_snapshot *snapshot)
{
    if (!storage)
        return -EINVAL;
    struct generation_registry_location location;
    int status = generation_registry_location_for_path(storage->logical_path,
                                                       &location);
    if (status != 0)
        return -status;
    status = pthread_mutex_lock(&location.shard->mu);
    if (status != 0)
        return -status;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, storage);
    if (!record)
    {
        pthread_mutex_unlock(&location.shard->mu);
        return -ENOENT;
    }
    if (snapshot)
    {
        *snapshot = (struct myfs_generation_registry_test_snapshot){
            .open_refs = record->open_refs,
            .writer_refs = record->writer_refs,
            .metadata_epoch = record->metadata_epoch,
            .gc_claim_id = record->gc_claim_id,
            .gc_state = record->gc_state,
            .install_aliases = record->install_aliases,
            .adaptive_full_windows = record->adaptive_stats.full_windows,
            .adaptive_partial_rmw = record->adaptive_stats.partial_rmw,
            .classified_since_evaluation =
                record->classified_since_evaluation,
            .adaptive_eval_id = record->adaptive_eval_id,
            .adaptive_state = record->adaptive_state,
            .superseded = record->superseded,
        };
    }
    pthread_mutex_unlock(&location.shard->mu);
    return 0;
}
#endif

static int pread_full_at(int fd, void *buf, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pread(fd, (char *)buf + done, size - done,
                          offset + (off_t)done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        done += (size_t)n;
    }
    return 0;
}

static int pwrite_full_at(int fd, const void *buf, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pwrite(fd, (const char *)buf + done, size - done,
                           offset + (off_t)done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        done += (size_t)n;
    }
    return 0;
}

static bool adaptive_begin(const myfs_adaptive_ticket_t *ticket,
                           bool *in_cooldown)
{
    *in_cooldown = false;
    struct generation_registry_location location;
    if (generation_registry_location_for_path(
            ticket->source_storage.logical_path, &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return false;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, &ticket->source_storage);
    bool valid = record && record->adaptive_eval_id == ticket->evaluation_id &&
                 record->adaptive_state == ADAPTIVE_QUEUED;
    if (valid)
    {
        record->adaptive_state = ADAPTIVE_INFLIGHT;
        if (record->last_resize_valid)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            *in_cooldown =
                timespec_compare(&now, &record->cooldown_until_mono) < 0;
        }
    }
    pthread_mutex_unlock(&location.shard->mu);
    return valid;
}

static void adaptive_reject(const myfs_adaptive_ticket_t *ticket)
{
    struct generation_registry_location location;
    if (generation_registry_location_for_path(
            ticket->source_storage.logical_path, &location) != 0 ||
        pthread_mutex_lock(&location.shard->mu) != 0)
        return;
    struct myfs_generation_record *record = find_generation_record_locked(
        location.shard, location.bucket_index, &ticket->source_storage);
    if (record && record->adaptive_eval_id == ticket->evaluation_id &&
        (record->adaptive_state == ADAPTIVE_QUEUED ||
         record->adaptive_state == ADAPTIVE_INFLIGHT))
        /* The evaluation ran and was rejected, so its ratio history is kept
         * but it takes a fresh evidence window before another evaluation. */
        restore_adaptive_ticket_locked(record, ticket, false);
    pthread_mutex_unlock(&location.shard->mu);
}

static int clone_inode(const myfs_inode_t *source, myfs_inode_t *dest)
{
    *dest = *source;
    size_t bytes = (size_t)source->chunk_map.num_chunks *
                   sizeof(myfs_chunk_t);
    dest->chunk_map.chunks = bytes ? malloc(bytes) : NULL;
    if (bytes && !dest->chunk_map.chunks)
        return -ENOMEM;
    if (bytes)
        memcpy(dest->chunk_map.chunks, source->chunk_map.chunks, bytes);
    return 0;
}

struct prepared_handle
{
    myfs_file_handle_t *handle;
    int data_fd;
    int meta_fd;
    myfs_inode_t inode;
};

static int compare_prepared_handles(const void *a, const void *b)
{
    const struct prepared_handle *pa = a;
    const struct prepared_handle *pb = b;
    uintptr_t av = (uintptr_t)pa->handle;
    uintptr_t bv = (uintptr_t)pb->handle;
    return av < bv ? -1 : av > bv;
}

static void cleanup_prepared_handles(struct prepared_handle *prepared,
                                     size_t count, size_t locked)
{
    while (locked > 0)
    {
        locked--;
        pthread_rwlock_unlock(&prepared[locked].handle->cache_lock);
    }
    for (size_t i = 0; i < count; i++)
    {
        if (prepared[i].data_fd >= 0)
            close(prepared[i].data_fd);
        if (prepared[i].meta_fd >= 0)
            close(prepared[i].meta_fd);
        free(prepared[i].inode.chunk_map.chunks);
    }
    free(prepared);
}

static int prepare_writer_handoff(const myfs_storage_t *old_storage,
                                  const myfs_storage_t *new_storage,
                                  const myfs_inode_t *new_inode,
                                  const struct generation_registry_location *location,
                                  struct prepared_handle **out,
                                  size_t *out_count)
{
    *out = NULL;
    *out_count = 0;
    int status = pthread_mutex_lock(&location->shard->mu);
    if (status != 0)
        return -status;
    struct myfs_generation_record *old_record = find_generation_record_locked(
        location->shard, location->bucket_index, old_storage);
    size_t count = 0;
    if (old_record)
    {
        for (myfs_file_handle_t *h = old_record->handles;
             h; h = h->registry_next)
        {
            if ((h->flags & O_ACCMODE) != O_RDONLY)
                count++;
        }
    }
    pthread_mutex_unlock(&location->shard->mu);

    struct prepared_handle *prepared = count
        ? calloc(count, sizeof(*prepared)) : NULL;
    if (count && !prepared)
        return -ENOMEM;
    for (size_t i = 0; i < count; i++)
    {
        prepared[i].data_fd = -1;
        prepared[i].meta_fd = -1;
    }

    status = pthread_mutex_lock(&location->shard->mu);
    if (status != 0)
    {
        cleanup_prepared_handles(prepared, count, 0);
        return -status;
    }
    old_record = find_generation_record_locked(
        location->shard, location->bucket_index, old_storage);
    size_t index = 0;
    bool overflow = false;
    if (old_record)
    {
        for (myfs_file_handle_t *h = old_record->handles;
             h; h = h->registry_next)
        {
            if ((h->flags & O_ACCMODE) != O_RDONLY)
            {
                if (index == count)
                {
                    overflow = true;
                    break;
                }
                prepared[index++].handle = h;
            }
        }
    }
    pthread_mutex_unlock(&location->shard->mu);
    if (overflow || index != count)
    {
        cleanup_prepared_handles(prepared, count, 0);
        return -EAGAIN;
    }

    if (count > 1)
        qsort(prepared, count, sizeof(*prepared), compare_prepared_handles);
    for (size_t i = 0; i < count; i++)
    {
        prepared[i].data_fd = open(new_storage->data_path,
                                   O_RDWR | O_CLOEXEC);
        if (prepared[i].data_fd < 0)
        {
            int ret = -errno;
            cleanup_prepared_handles(prepared, count, 0);
            return ret;
        }
        prepared[i].meta_fd = open(new_storage->meta_path,
                                   O_RDWR | O_CLOEXEC);
        if (prepared[i].meta_fd < 0)
        {
            int ret = -errno;
            cleanup_prepared_handles(prepared, count, 0);
            return ret;
        }
        int ret = clone_inode(new_inode, &prepared[i].inode);
        if (ret != 0)
        {
            cleanup_prepared_handles(prepared, count, 0);
            return ret;
        }
    }
    *out = prepared;
    *out_count = count;
    return 0;
}

static void detach_handle_from_record(struct myfs_generation_record *record,
                                      myfs_file_handle_t *handle)
{
    if (handle->registry_prev)
        handle->registry_prev->registry_next = handle->registry_next;
    else if (record->handles == handle)
        record->handles = handle->registry_next;
    if (handle->registry_next)
        handle->registry_next->registry_prev = handle->registry_prev;
    if (record->open_refs > 0)
        record->open_refs--;
    if (record->writer_refs > 0)
        record->writer_refs--;
}

static void attach_handle_to_record(struct myfs_generation_record *record,
                                    myfs_file_handle_t *handle)
{
    handle->registry_prev = NULL;
    handle->registry_next = record->handles;
    if (record->handles)
        record->handles->registry_prev = handle;
    record->handles = handle;
    record->open_refs++;
    record->writer_refs++;
    handle->generation_record = record;
}

static int publish_and_handoff(const char *path,
                               const myfs_storage_t *old_storage,
                               const myfs_storage_t *new_storage,
                               const myfs_inode_t *new_inode,
                               bool resized, bool *publish_durable)
{
    *publish_durable = false;
    if (strcmp(old_storage->logical_path, new_storage->logical_path) != 0)
        return -EXDEV;
    struct generation_registry_location location;
    int status = generation_registry_location_for_path(
        old_storage->logical_path, &location);
    if (status != 0)
        return -status;

    struct prepared_handle *prepared = NULL;
    size_t count = 0;
    int ret = prepare_writer_handoff(old_storage, new_storage, new_inode,
                                     &location, &prepared, &count);
    if (ret != 0)
        return ret;

    size_t locked = 0;
    for (; locked < count; locked++)
    {
        int lock_ret = pthread_rwlock_wrlock(
            &prepared[locked].handle->cache_lock);
        if (lock_ret != 0)
        {
            cleanup_prepared_handles(prepared, count, locked);
            return -lock_ret;
        }
    }

    ret = run_generation_handoff_test_hook(
        GENERATION_HANDOFF_HOOK_BEFORE_PUBLISH,
        old_storage, new_storage);
    if (ret == 0)
        ret = publish_generation(path, new_storage);
    if (ret == 0)
    {
        ret = run_generation_handoff_test_hook(
            GENERATION_HANDOFF_HOOK_AFTER_VISIBLE_PUBLISH,
            old_storage, new_storage);
        if (ret == 0)
            *publish_durable = true;
    }
    if (ret != 0)
    {
        myfs_storage_t active;
        int resolve_ret = resolve_storage(path, &active);
        bool active_is_old = resolve_ret == 0 &&
            storage_generation_equal(&active, old_storage);
        bool active_is_new = resolve_ret == 0 &&
            storage_generation_equal(&active, new_storage);
        if (!active_is_new)
        {
            if (!active_is_old)
            {
                if (pthread_mutex_lock(&location.shard->mu) == 0)
                {
                    struct myfs_generation_record *old_record =
                        find_generation_record_locked(
                            location.shard, location.bucket_index,
                            old_storage);
                    if (old_record)
                        old_record->superseded = true;
                    pthread_mutex_unlock(&location.shard->mu);
                }
            }
            cleanup_prepared_handles(prepared, count, locked);
            return ret;
        }
        /* The rename is visible, so live writers must move atomically to the
         * new bundle.  The old generation is nevertheless not GC-safe until
         * a later successful parent-directory fsync confirms durability. */
        ret = 0;
    }

    int record_create_ret = run_generation_handoff_test_hook(
        GENERATION_HANDOFF_HOOK_BEFORE_RECORD_CREATE,
        old_storage, new_storage);
    struct myfs_generation_record *new_record_candidate =
        record_create_ret == 0
            ? allocate_generation_record(&location, new_storage) : NULL;
    if (record_create_ret == 0 && !new_record_candidate)
        record_create_ret = -ENOMEM;
    status = pthread_mutex_lock(&location.shard->mu);
    if (status != 0)
    {
        free(new_record_candidate);
        cleanup_prepared_handles(prepared, count, locked);
        return -status;
    }
    struct myfs_generation_record *old_record = find_generation_record_locked(
        location.shard, location.bucket_index, old_storage);
    struct myfs_generation_record *new_record = find_generation_record_locked(
        location.shard, location.bucket_index, new_storage);
    if (!new_record && new_record_candidate)
    {
        insert_generation_record_locked(&location, new_record_candidate);
        new_record = new_record_candidate;
        new_record_candidate = NULL;
    }
    if (record_create_ret != 0)
        ret = record_create_ret;
    else if (!new_record)
        ret = -ENOMEM;
    if (ret != 0 && old_record)
        old_record->superseded = true;
    if (ret == 0)
    {
        if (old_record)
        {
            old_record->superseded = true;
            new_record->adaptive_stats.full_windows = add_saturating(
                new_record->adaptive_stats.full_windows,
                old_record->adaptive_stats.full_windows);
            new_record->adaptive_stats.partial_rmw = add_saturating(
                new_record->adaptive_stats.partial_rmw,
                old_record->adaptive_stats.partial_rmw);
            new_record->classified_since_evaluation = add_saturating(
                new_record->classified_since_evaluation,
                old_record->classified_since_evaluation);
            old_record->adaptive_stats = (myfs_window_stats_t){0};
            old_record->classified_since_evaluation = 0;
            old_record->adaptive_state = ADAPTIVE_IDLE;
        }
        if (resized)
        {
            clock_gettime(CLOCK_MONOTONIC, &new_record->last_resize_mono);
            new_record->cooldown_until_mono = new_record->last_resize_mono;
            new_record->cooldown_until_mono.tv_sec +=
                ADAPTIVE_COOLDOWN_SECONDS;
            new_record->last_resize_valid = true;
            if (stats_sample(new_record->adaptive_stats) > 0)
                new_record->adaptive_state = ADAPTIVE_COOLDOWN_DEFERRED;
        }
        else if (old_record && old_record->last_resize_valid)
        {
            new_record->last_resize_valid = true;
            new_record->last_resize_mono = old_record->last_resize_mono;
            new_record->cooldown_until_mono = old_record->cooldown_until_mono;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (stats_sample(new_record->adaptive_stats) > 0 &&
                timespec_compare(&now, &new_record->cooldown_until_mono) < 0)
                new_record->adaptive_state = ADAPTIVE_COOLDOWN_DEFERRED;
        }

        for (size_t i = 0; i < count; i++)
        {
            myfs_file_handle_t *handle = prepared[i].handle;
            if (old_record)
                detach_handle_from_record(old_record, handle);
            attach_handle_to_record(new_record, handle);
            handle->storage = *new_storage;
            handle->seen_metadata_epoch = new_record->metadata_epoch;
            if (i == 0)
                (void)run_generation_handoff_test_hook(
                    GENERATION_HANDOFF_HOOK_AFTER_FIRST_WRITER_RELINK,
                    old_storage, new_storage);
        }
    }
    pthread_mutex_unlock(&location.shard->mu);
    free(new_record_candidate);

    if (ret == 0)
        (void)run_generation_handoff_test_hook(
            GENERATION_HANDOFF_HOOK_AFTER_REGISTRY_TRANSACTION,
            old_storage, new_storage);

    if (ret != 0)
    {
        /* Publication already happened.  Keep old resources pinned and make
         * writers stale rather than exposing a mixed bundle. */
        cleanup_prepared_handles(prepared, count, locked);
        return ret;
    }

    for (size_t i = 0; i < count; i++)
    {
        myfs_file_handle_t *handle = prepared[i].handle;
        int old_data_fd = handle->data_fd;
        int old_meta_fd = handle->meta_fd;
        myfs_inode_t old_inode = handle->cached_inode;
        handle->data_fd = prepared[i].data_fd;
        handle->meta_fd = prepared[i].meta_fd;
        handle->cached_inode = prepared[i].inode;
        handle->cache_valid = true;
        prepared[i].data_fd = -1;
        prepared[i].meta_fd = -1;
        prepared[i].inode.chunk_map.chunks = NULL;
        close(old_data_fd);
        close(old_meta_fd);
        free(old_inode.chunk_map.chunks);
    }
    cleanup_prepared_handles(prepared, count, locked);
    return 0;
}

static int compact_data_file_locked(const char *path,
                                    const struct compact_request *request)
{
    myfs_storage_t old_storage;
    int ret = resolve_storage(path, &old_storage);
    if (ret != 0)
    {
        if (request && request->adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }

    bool ordinary = !request || request->ordinary;
    bool adaptive = false;
    bool in_cooldown = false;
    if (request && request->adaptive)
    {
        if (storage_generation_equal(
                &old_storage, &request->adaptive_ticket.source_storage))
            adaptive = adaptive_begin(&request->adaptive_ticket,
                                      &in_cooldown);
        if (!adaptive)
            adaptive_reject(&request->adaptive_ticket);
    }

    /* A release may have claimed an adaptive ticket while an older ordinary
     * request was already waiting for this file lock.  Let the queued
     * combined request consume that ticket before any ordinary publish can
     * supersede its source generation. */
    if (!adaptive && generation_has_queued_adaptive_locked(&old_storage))
        return 0;

    ret = recover_generations_for_path_locked(path, &old_storage);
    if (ret != 0)
        LOG("[WARN] compact: recovery for %s returned %d\n", path, ret);

    /* Ordinary compaction waits until the active generation has no writers.
     * Adaptive compaction hands live writers to the new generation after a
     * successful publication.  Final writable release removes its reference
     * before scheduling compaction. */
    if (!adaptive && generation_writer_refs_locked(&old_storage) > 0)
    {
        LOG("[DEBUG] compact: deferred; active generation has writers\n");
        return 0;
    }

    myfs_inode_t inode = {0};
    ret = load_chunk_map_from_path(old_storage.meta_path, &inode);
    if (ret != 0)
    {
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }
    struct stat st;
    if (stat(old_storage.data_path, &st) != 0)
    {
        ret = -errno;
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }
    off_t data_file_size = st.st_size;

    if (inode.chunk_map.num_chunks == 0 && data_file_size == 0)
    {
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        free(inode.chunk_map.chunks);
        return 0;
    }

    uint64_t live_bytes = 0;
    uint64_t live_logical_bytes = 0;
    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
    {
        live_bytes = add_saturating(live_bytes,
                                    inode.chunk_map.chunks[i].raw_size);
        live_logical_bytes = add_saturating(
            live_logical_bytes, inode.chunk_map.chunks[i].stored_size);
    }
    if (live_bytes > (uint64_t)data_file_size)
    {
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return -EIO;
    }

    uint32_t target_window = inode.window_size;
    bool resize = false;
    if (adaptive)
    {
        int decision = myfs_choose_resize_target(
            inode.window_size, live_logical_bytes,
            request->adaptive_ticket.stats_snapshot,
            in_cooldown, &target_window);
        if (decision < 0)
        {
            adaptive_reject(&request->adaptive_ticket);
            free(inode.chunk_map.chunks);
            return decision;
        }
        resize = decision == 1;
        if (!resize)
        {
            adaptive_reject(&request->adaptive_ticket);
            adaptive = false;
            if (!ordinary)
            {
                free(inode.chunk_map.chunks);
                return 0;
            }
        }
    }

    uint64_t wasted_bytes = (uint64_t)data_file_size - live_bytes;
    double wasted = data_file_size > 0
        ? (double)wasted_bytes / (double)data_file_size : 0.0;
    /* File chưa packed luôn được repack bất kể waste theo window_size của
     * generation; adaptive resize có thể chọn window_size đích mới. Sau khi
     * packed, trigger migration này tự im lặng. */
    if (!resize && inode.chunk_map.num_chunks != 0 &&
        (wasted < COMPACT_THRESHOLD || wasted_bytes < inode.window_size) &&
        inode.chunk_map.fully_packed)
    {
        LOG("[DEBUG] compact: skip (wasted=%.1f%% wasted_bytes=%llu)\n",
            wasted * 100, (unsigned long long)wasted_bytes);
        free(inode.chunk_map.chunks);
        return 0;
    }

    LOG("[DEBUG] compact: start generation=%s data_size=%lld live=%llu wasted=%.1f%%\n",
        old_storage.generation_id, (long long)data_file_size,
        (unsigned long long)live_bytes, wasted * 100);

    int src_fd = open(old_storage.data_path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0)
    {
        ret = -errno;
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }

    myfs_storage_t new_storage;
    ret = create_generation_storage(path, st.st_mode, &new_storage);
    if (ret != 0)
    {
        close(src_fd);
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }

    int dst_fd = open(new_storage.data_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (dst_fd < 0)
    {
        ret = -errno;
        close(src_fd);
        remove_generation_storage(&new_storage);
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }

    off_t new_phys = 0;
    if (inode.chunk_map.fully_packed && !resize)
    {
        /* File đã packed: copy verbatim từng blob — không tốn recompress. */
        for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
        {
            myfs_chunk_t *chunk = &inode.chunk_map.chunks[i];
            char *buf = malloc(chunk->raw_size);
            if (!buf)
            {
                ret = -ENOMEM;
                break;
            }
            ret = pread_full_at(src_fd, buf, chunk->raw_size,
                                chunk->physical_offset);
            if (ret == 0 && chunk->checksum != 0 &&
                chunk_crc32(buf, chunk->raw_size) != chunk->checksum)
                ret = -EIO;
            if (ret == 0)
                ret = pwrite_full_at(dst_fd, buf, chunk->raw_size, new_phys);
            free(buf);
            if (ret != 0)
                break;

            chunk->physical_offset = new_phys;
            new_phys += chunk->raw_size;
        }
    }
    else
    {
        /* Repack legacy layouts or convert the complete generation to the
         * selected adaptive window size. */
        off_t max_end = 0;
        for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
        {
            myfs_chunk_t *c = &inode.chunk_map.chunks[i];
            off_t c_end = (off_t)c->logical_offset + (off_t)c->stored_size;
            if (c_end > max_end)
                max_end = c_end;
        }
        myfs_chunk_t *entries = NULL;
        uint32_t entry_count = 0;
        uint64_t repack_hi_u64;
        ret = myfs_window_ceil((uint64_t)max_end, target_window,
                               &repack_hi_u64);
        if (ret == 0 && repack_hi_u64 > INT64_MAX)
            ret = -EFBIG;
        if (ret == 0)
            ret = myfs_repack_windows(src_fd, dst_fd, &new_phys,
                                  &inode.chunk_map, target_window,
                                  0, inode.chunk_map.num_chunks,
                                  0, (off_t)repack_hi_u64,
                                  NULL, 0, 0, &entries, &entry_count);
        if (ret == 0)
        {
            free(inode.chunk_map.chunks);
            inode.chunk_map.chunks = entries;
            inode.chunk_map.num_chunks = entry_count;
            inode.chunk_map.fully_packed = true;
            inode.window_size = target_window;
            LOG("[DEBUG] compact: repacked legacy file into %u windows\n",
                entry_count);
        }
    }

    if (close(src_fd) != 0 && ret == 0)
        ret = -errno;
    if (ret == 0 && fsync(dst_fd) != 0)
        ret = -errno;
    if (close(dst_fd) != 0 && ret == 0)
        ret = -errno;

    if (ret == 0)
        ret = save_chunk_map_to_path(new_storage.meta_path, &inode);
    if (ret == 0)
        ret = fsync_parent_path(new_storage.generation_dir);
    if (ret != 0)
    {
        remove_generation_storage(&new_storage);
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }

    compact_test_failpoint(path, "prepared_before_pointer", &new_storage);

    bool publish_durable = false;
    ret = publish_and_handoff(path, &old_storage, &new_storage, &inode, resize,
                              &publish_durable);
    if (ret != 0)
    {
        /* rename may already have occurred if only the final directory fsync
         * failed.  Keeping both generations is always safe; never guess here. */
        free(inode.chunk_map.chunks);
        if (adaptive)
            adaptive_reject(&request->adaptive_ticket);
        return ret;
    }

    compact_test_failpoint(path, "pointer_durable_before_gc", &old_storage);

    if (publish_durable)
    {
        ret = mark_generation_for_gc_locked(&old_storage, true);
        if (ret == 0)
            ret = run_generation_gc_locked(path);
    }
    else
        LOG("[WARN] compact: pointer visible but not durability-confirmed; "
            "retaining old generation %s\n", old_storage.generation_id);

    free(inode.chunk_map.chunks);
    LOG("[DEBUG] compact: committed generation=%s new_data_size=%lld (was %lld)\n",
        new_storage.generation_id, (long long)new_phys,
        (long long)data_file_size);
    return ret;
}

int compact_data_file(const char *path)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;

    int ret = compact_data_file_locked(path, NULL);
    myfs_unlock_file(lk);
    return ret;
}

/* =========================================================================
 * Background compaction worker.
 * release() performs eligible generation GC synchronously, then submits only
 * compaction work here.  The worker takes the logical path lock before
 * compacting; the stopped-worker fallback follows the same locked core path.
 * Queue deduplication is by path, and shutdown drains the queue before exit.
 * ========================================================================= */

static pthread_mutex_t compact_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t compact_queue_cv = PTHREAD_COND_INITIALIZER;
static struct compact_request *compact_queue_head;
static struct compact_request *compact_queue_tail;
static pthread_t compact_worker_thread;
static bool compact_worker_running;
static bool compact_worker_stop;

/* Worker start/stop is serialized by init/destroy, and request producers are
 * quiescent before destroy.  Once stop is set under compact_queue_mu, new work
 * uses synchronous fallback; the worker exits only after the queue is empty. */

#ifdef MYFS_TEST_FAILPOINTS
static myfs_compaction_stop_test_hook_fn compact_stop_post_join_test_hook;
static void *compact_stop_post_join_test_hook_context;

void myfs_compaction_test_set_stop_post_join_hook(
    myfs_compaction_stop_test_hook_fn hook, void *context)
{
    if (pthread_mutex_lock(&compact_queue_mu) != 0)
        return;
    compact_stop_post_join_test_hook = hook;
    compact_stop_post_join_test_hook_context = context;
    pthread_mutex_unlock(&compact_queue_mu);
}

int myfs_compaction_test_queue_snapshot(
    struct myfs_compaction_queue_test_snapshot *snapshot)
{
    if (!snapshot)
        return -EINVAL;
    int status = pthread_mutex_lock(&compact_queue_mu);
    if (status != 0)
        return -status;
    size_t queued_requests = 0;
    for (struct compact_request *request = compact_queue_head;
         request; request = request->next)
        queued_requests++;
    *snapshot = (struct myfs_compaction_queue_test_snapshot){
        .queued_requests = queued_requests,
        .head_is_null = compact_queue_head == NULL,
        .tail_is_null = compact_queue_tail == NULL,
        .worker_running = compact_worker_running,
        .stop_requested = compact_worker_stop,
    };
    pthread_mutex_unlock(&compact_queue_mu);
    return 0;
}
#endif

static int schedule_request(const char *path, bool ordinary,
                            const myfs_adaptive_ticket_t *ticket)
{
    pthread_mutex_lock(&compact_queue_mu);
    if (!compact_worker_running || compact_worker_stop)
    {
        /* Worker chưa chạy hoặc đang dừng: thực hiện compaction đồng bộ dưới
         * path lock để không bỏ sót request. */
        pthread_mutex_unlock(&compact_queue_mu);
        if (ticket && ticket->valid)
        {
            struct compact_request immediate = {
                .path = (char *)path,
                .ordinary = ordinary,
                .adaptive = true,
                .adaptive_ticket = *ticket,
            };
            myfs_file_lock_t *lk = myfs_lock_file(path);
            if (!lk)
                return -ENOMEM;
            int ret = compact_data_file_locked(path, &immediate);
            myfs_unlock_file(lk);
            return ret;
        }
        return compact_data_file(path);
    }
    for (struct compact_request *r = compact_queue_head; r; r = r->next)
    {
        if (strcmp(r->path, path) == 0)
        {
            r->ordinary = r->ordinary || ordinary;
            if (ticket && ticket->valid)
            {
                if (r->adaptive &&
                    (r->adaptive_ticket.evaluation_id != ticket->evaluation_id ||
                     !storage_generation_equal(
                         &r->adaptive_ticket.source_storage,
                         &ticket->source_storage)))
                {
                    pthread_mutex_unlock(&compact_queue_mu);
                    return -EBUSY;
                }
                r->adaptive = true;
                r->adaptive_ticket = *ticket;
            }
            pthread_mutex_unlock(&compact_queue_mu);
            return 0; /* đã có trong hàng đợi */
        }
    }
    struct compact_request *req = malloc(sizeof(*req));
    char *copy = req ? strdup(path) : NULL;
    if (!req || !copy)
    {
        free(req);
        free(copy);
        pthread_mutex_unlock(&compact_queue_mu);
        return -ENOMEM;
    }
    req->path = copy;
    req->ordinary = ordinary;
    req->adaptive = ticket && ticket->valid;
    if (req->adaptive)
        req->adaptive_ticket = *ticket;
    req->next = NULL;
    if (compact_queue_tail)
        compact_queue_tail->next = req;
    else
        compact_queue_head = req;
    compact_queue_tail = req;
    pthread_cond_signal(&compact_queue_cv);
    pthread_mutex_unlock(&compact_queue_mu);
    return 0;
}

int schedule_compaction(const char *path)
{
    return schedule_request(path, true, NULL);
}

int schedule_adaptive_compaction(const char *path,
                                 const myfs_adaptive_ticket_t *ticket)
{
    if (!ticket || !ticket->valid)
        return -EINVAL;
    return schedule_request(path, false, ticket);
}

int schedule_release_compaction(const char *path,
                                const myfs_adaptive_ticket_t *ticket)
{
    if (!ticket || !ticket->valid)
        return -EINVAL;
    return schedule_request(path, true, ticket);
}

static void *compact_worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&compact_queue_mu);
    for (;;)
    {
        while (!compact_queue_head && !compact_worker_stop)
            pthread_cond_wait(&compact_queue_cv, &compact_queue_mu);
        if (!compact_queue_head)
            break; /* stop được bật và queue đã drain sạch */

        struct compact_request *req = compact_queue_head;
        compact_queue_head = req->next;
        if (!compact_queue_head)
            compact_queue_tail = NULL;
        pthread_mutex_unlock(&compact_queue_mu);

        myfs_file_lock_t *lk = myfs_lock_file(req->path);
        int ret = lk ? compact_data_file_locked(req->path, req) : -ENOMEM;
        if (lk)
            myfs_unlock_file(lk);
        else if (req->adaptive)
            generation_adaptive_schedule_failed(&req->adaptive_ticket);
        if (ret != 0)
            LOG("[WARN] background compact %s returned %d\n", req->path, ret);
        free(req->path);
        free(req);

        pthread_mutex_lock(&compact_queue_mu);
    }
    pthread_mutex_unlock(&compact_queue_mu);
    return NULL;
}

int start_compaction_worker(void)
{
    pthread_mutex_lock(&compact_queue_mu);
    if (compact_worker_running)
    {
        pthread_mutex_unlock(&compact_queue_mu);
        return 0;
    }
    compact_worker_stop = false;
    int ret = pthread_create(&compact_worker_thread, NULL, compact_worker, NULL);
    if (ret == 0)
        compact_worker_running = true;
    pthread_mutex_unlock(&compact_queue_mu);
    if (ret != 0)
        LOG("[WARN] compaction worker start failed (%d), compact chạy đồng bộ\n",
            ret);
    return -ret;
}

void stop_compaction_worker(void)
{
    pthread_mutex_lock(&compact_queue_mu);
    if (!compact_worker_running)
    {
        pthread_mutex_unlock(&compact_queue_mu);
        return;
    }
    compact_worker_stop = true;
#ifdef MYFS_TEST_FAILPOINTS
    myfs_compaction_stop_test_hook_fn post_join_hook =
        compact_stop_post_join_test_hook;
    void *post_join_hook_context = compact_stop_post_join_test_hook_context;
#endif
    pthread_cond_broadcast(&compact_queue_cv);
    pthread_mutex_unlock(&compact_queue_mu);

    pthread_join(compact_worker_thread, NULL);

#ifdef MYFS_TEST_FAILPOINTS
    if (post_join_hook)
        post_join_hook(post_join_hook_context);
#endif

    pthread_mutex_lock(&compact_queue_mu);
    compact_worker_running = false;
    compact_worker_stop = false;
    pthread_mutex_unlock(&compact_queue_mu);
}
