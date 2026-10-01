#include "myfs.h"

#include <poll.h>

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
    bool gc_pending;
    bool install_aliases;
    struct myfs_generation_record *next;
};

static struct myfs_generation_record *generation_registry;

/* Leaf mutex bảo vệ danh sách registry và các field của record. Luôn được
 * lấy SAU file lock (không bao giờ lấy file lock khi đang giữ registry_mu),
 * và không giữ mutex nào khác bên trong — không thể deadlock. */
static pthread_mutex_t registry_mu = PTHREAD_MUTEX_INITIALIZER;

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

static struct myfs_generation_record *find_generation_record(
    const myfs_storage_t *storage)
{
    for (struct myfs_generation_record *record = generation_registry;
         record; record = record->next)
    {
        if (storage_generation_equal(&record->storage, storage))
            return record;
    }
    return NULL;
}

static struct myfs_generation_record *get_generation_record(
    const myfs_storage_t *storage)
{
    struct myfs_generation_record *record = find_generation_record(storage);
    if (record)
        return record;

    record = calloc(1, sizeof(*record));
    if (!record)
        return NULL;
    record->storage = *storage;
    record->metadata_epoch = 1;
    record->next = generation_registry;
    generation_registry = record;
    return record;
}

int register_generation_handle_locked(myfs_file_handle_t *handle)
{
    struct stat st;
    if (fstat(handle->data_fd, &st) == 0)
    {
        handle->storage.data_dev = st.st_dev;
        handle->storage.data_ino = st.st_ino;
    }

    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = get_generation_record(&handle->storage);
    if (!record)
    {
        pthread_mutex_unlock(&registry_mu);
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
    pthread_mutex_unlock(&registry_mu);
    return 0;
}

void unregister_generation_handle_locked(myfs_file_handle_t *handle)
{
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
        return;

    pthread_mutex_lock(&registry_mu);
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

    if (record->open_refs == 0 && !record->gc_pending &&
        record->adaptive_stats.full_windows == 0 &&
        record->adaptive_stats.partial_rmw == 0 &&
        record->adaptive_state == ADAPTIVE_IDLE &&
        !record->last_resize_valid)
    {
        struct myfs_generation_record **cursor = &generation_registry;
        while (*cursor && *cursor != record)
            cursor = &(*cursor)->next;
        if (*cursor == record)
        {
            *cursor = record->next;
            free(record);
        }
    }
    pthread_mutex_unlock(&registry_mu);
}

void generation_state_snapshot(myfs_file_handle_t *handle,
                               uint64_t *metadata_epoch, bool *superseded)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = handle->generation_record;
    if (metadata_epoch)
        *metadata_epoch = record ? record->metadata_epoch : 0;
    if (superseded)
        *superseded = record ? record->superseded : true;
    pthread_mutex_unlock(&registry_mu);
}

uint64_t generation_bump_metadata_epoch_locked(myfs_file_handle_t *handle)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = handle->generation_record;
    uint64_t epoch = 0;
    if (record)
    {
        record->metadata_epoch++;
        if (record->metadata_epoch == 0)
            record->metadata_epoch = 1;
        epoch = record->metadata_epoch;
    }
    pthread_mutex_unlock(&registry_mu);
    return epoch;
}

void generation_bump_storage_epoch_locked(const myfs_storage_t *storage)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = find_generation_record(storage);
    if (record)
    {
        record->metadata_epoch++;
        if (record->metadata_epoch == 0)
            record->metadata_epoch = 1;
    }
    pthread_mutex_unlock(&registry_mu);
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

bool generation_observe_write_locked(myfs_file_handle_t *handle,
                                     uint64_t full_windows,
                                     uint64_t partial_rmw,
                                     myfs_adaptive_ticket_t *ticket)
{
    if (ticket)
        memset(ticket, 0, sizeof(*ticket));
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
    {
        pthread_mutex_unlock(&registry_mu);
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
        pthread_mutex_unlock(&registry_mu);
        return false;
    }
    bool claimed = claim_adaptive_ticket(record, ticket);
    pthread_mutex_unlock(&registry_mu);
    return claimed;
}

bool generation_claim_release_locked(myfs_file_handle_t *handle,
                                     myfs_adaptive_ticket_t *ticket)
{
    if (ticket)
        memset(ticket, 0, sizeof(*ticket));
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = handle->generation_record;
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
    pthread_mutex_unlock(&registry_mu);
    return claimed;
}

void generation_adaptive_schedule_failed(const myfs_adaptive_ticket_t *ticket)
{
    if (!ticket || !ticket->valid)
        return;
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record =
        find_generation_record(&ticket->source_storage);
    if (record && record->adaptive_eval_id == ticket->evaluation_id &&
        record->adaptive_state == ADAPTIVE_QUEUED)
    {
        record->adaptive_stats.full_windows = add_saturating(
            record->adaptive_stats.full_windows,
            ticket->stats_snapshot.full_windows);
        record->adaptive_stats.partial_rmw = add_saturating(
            record->adaptive_stats.partial_rmw,
            ticket->stats_snapshot.partial_rmw);
        record->classified_since_evaluation = add_saturating(
            record->classified_since_evaluation,
            stats_sample(ticket->stats_snapshot));
        record->adaptive_state = ADAPTIVE_IDLE;
    }
    pthread_mutex_unlock(&registry_mu);
}

unsigned generation_writer_refs_locked(const myfs_storage_t *storage)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = find_generation_record(storage);
    unsigned refs = record ? record->writer_refs : 0;
    pthread_mutex_unlock(&registry_mu);
    return refs;
}

unsigned generation_open_refs_locked(const myfs_storage_t *storage)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = find_generation_record(storage);
    unsigned refs = record ? record->open_refs : 0;
    pthread_mutex_unlock(&registry_mu);
    return refs;
}

static bool generation_has_queued_adaptive_locked(
    const myfs_storage_t *storage)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = find_generation_record(storage);
    bool queued = record && record->adaptive_state == ADAPTIVE_QUEUED;
    pthread_mutex_unlock(&registry_mu);
    return queued;
}

int mark_generation_for_gc_locked(const myfs_storage_t *storage,
                                  bool install_aliases)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = get_generation_record(storage);
    if (!record)
    {
        pthread_mutex_unlock(&registry_mu);
        return -ENOMEM;
    }
    record->gc_pending = true;
    record->install_aliases = record->install_aliases || install_aliases;
    pthread_mutex_unlock(&registry_mu);
    return 0;
}

/* Gọi khi ĐANG giữ registry_mu (từ vòng GC). */
static unsigned legacy_refs_for_path(const char *path)
{
    unsigned refs = 0;
    for (struct myfs_generation_record *record = generation_registry;
         record; record = record->next)
    {
        if (record->storage.is_legacy &&
            strcmp(record->storage.logical_path, path) == 0)
            refs += record->open_refs;
    }
    return refs;
}

/* Caller giữ file lock của `path` (hoặc path == NULL lúc destroy). Chỉ xử lý
 * record của path đó — record của path khác thuộc quyền file lock khác.
 * A record enters this list only after the pointer rename and its
 * parent-directory fsync have succeeded. */
int run_generation_gc_locked(const char *path)
{
    int first_error = 0;
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record **cursor = &generation_registry;

    while (*cursor)
    {
        struct myfs_generation_record *record = *cursor;
        if (!record->gc_pending ||
            (path && strcmp(record->storage.logical_path, path) != 0))
        {
            cursor = &record->next;
            continue;
        }

        myfs_storage_t active;
        int resolve_ret = resolve_storage(record->storage.logical_path, &active);
        if (resolve_ret == 0 &&
            storage_generation_equal(&record->storage, &active))
        {
            LOG("[ERROR] GC refused to remove active generation %s for %s\n",
                record->storage.generation_id, record->storage.logical_path);
            if (first_error == 0)
                first_error = -EBUSY;
            cursor = &record->next;
            continue;
        }

        /* Replacing legacy compatibility names would remove the legacy pair's
         * directory entries, so it is deferred until all legacy handles close. */
        if (record->install_aliases &&
            legacy_refs_for_path(record->storage.logical_path) > 0)
        {
            compact_test_failpoint(record->storage.logical_path,
                                   "gc_deferred_old_ref", &record->storage);
            cursor = &record->next;
            continue;
        }

        if (record->install_aliases)
        {
            if (resolve_ret != 0 || active.is_legacy)
            {
                if (first_error == 0)
                    first_error = (resolve_ret != 0) ? resolve_ret : -EIO;
                cursor = &record->next;
                continue;
            }
            int alias_ret = install_generation_aliases(
                record->storage.logical_path, &active);
            if (alias_ret != 0)
            {
                if (first_error == 0)
                    first_error = alias_ret;
                cursor = &record->next;
                continue;
            }
        }

        if (record->open_refs > 0)
        {
            compact_test_failpoint(record->storage.logical_path,
                                   "gc_deferred_old_ref", &record->storage);
            cursor = &record->next;
            continue;
        }

        int remove_ret = 0;
        if (!record->storage.is_legacy)
            remove_ret = remove_generation_storage(&record->storage);
        /* For a legacy generation, install_generation_aliases() atomically
         * replaced both old directory entries, so no separate unlink remains. */

        if (remove_ret != 0)
        {
            if (first_error == 0)
                first_error = remove_ret;
            cursor = &record->next;
            continue;
        }

        LOG("[DEBUG] GC removed generation %s for %s\n",
            record->storage.generation_id, record->storage.logical_path);
        *cursor = record->next;
        free(record);
    }
    pthread_mutex_unlock(&registry_mu);
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
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record = generation_registry;
    while (record)
    {
        struct myfs_generation_record *next = record->next;
        free(record);
        record = next;
    }
    generation_registry = NULL;
    pthread_mutex_unlock(&registry_mu);
}

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
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record =
        find_generation_record(&ticket->source_storage);
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
    pthread_mutex_unlock(&registry_mu);
    return valid;
}

static void adaptive_reject(const myfs_adaptive_ticket_t *ticket)
{
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *record =
        find_generation_record(&ticket->source_storage);
    if (record && record->adaptive_eval_id == ticket->evaluation_id &&
        (record->adaptive_state == ADAPTIVE_QUEUED ||
         record->adaptive_state == ADAPTIVE_INFLIGHT))
    {
        record->adaptive_stats.full_windows = add_saturating(
            record->adaptive_stats.full_windows,
            ticket->stats_snapshot.full_windows);
        record->adaptive_stats.partial_rmw = add_saturating(
            record->adaptive_stats.partial_rmw,
            ticket->stats_snapshot.partial_rmw);
        record->adaptive_state = ADAPTIVE_IDLE;
    }
    pthread_mutex_unlock(&registry_mu);
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
                                  struct prepared_handle **out,
                                  size_t *out_count)
{
    *out = NULL;
    *out_count = 0;
    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *old_record =
        find_generation_record(old_storage);
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
    pthread_mutex_unlock(&registry_mu);

    struct prepared_handle *prepared = count
        ? calloc(count, sizeof(*prepared)) : NULL;
    if (count && !prepared)
        return -ENOMEM;
    for (size_t i = 0; i < count; i++)
    {
        prepared[i].data_fd = -1;
        prepared[i].meta_fd = -1;
    }

    pthread_mutex_lock(&registry_mu);
    old_record = find_generation_record(old_storage);
    size_t index = 0;
    if (old_record)
    {
        for (myfs_file_handle_t *h = old_record->handles;
             h; h = h->registry_next)
        {
            if ((h->flags & O_ACCMODE) != O_RDONLY)
                prepared[index++].handle = h;
        }
    }
    pthread_mutex_unlock(&registry_mu);
    if (index != count)
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
    struct prepared_handle *prepared = NULL;
    size_t count = 0;
    int ret = prepare_writer_handoff(old_storage, new_storage, new_inode,
                                     &prepared, &count);
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

    ret = publish_generation(path, new_storage);
    if (ret == 0)
        *publish_durable = true;
    if (ret != 0)
    {
        myfs_storage_t active;
        int resolve_ret = resolve_storage(path, &active);
        if (resolve_ret != 0 || !storage_generation_equal(&active, new_storage))
        {
            if (resolve_ret != 0)
            {
                pthread_mutex_lock(&registry_mu);
                struct myfs_generation_record *old_record =
                    find_generation_record(old_storage);
                if (old_record)
                    old_record->superseded = true;
                pthread_mutex_unlock(&registry_mu);
            }
            cleanup_prepared_handles(prepared, count, locked);
            return ret;
        }
        /* The rename is visible, so live writers must move atomically to the
         * new bundle.  The old generation is nevertheless not GC-safe until
         * a later successful parent-directory fsync confirms durability. */
        ret = 0;
    }

    pthread_mutex_lock(&registry_mu);
    struct myfs_generation_record *old_record =
        find_generation_record(old_storage);
    struct myfs_generation_record *new_record =
        get_generation_record(new_storage);
    if (!new_record)
        ret = -ENOMEM;
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
        }
    }
    pthread_mutex_unlock(&registry_mu);

    if (ret != 0)
    {
        /* Publication already happened.  Keep old resources pinned and make
         * writers stale rather than exposing a mixed bundle. */
        pthread_mutex_lock(&registry_mu);
        if (old_record)
            old_record->superseded = true;
        pthread_mutex_unlock(&registry_mu);
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

    /* A writer's handle is never allowed to become stale.  The final writable
     * release removes its reference before invoking compaction. */
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
    /* File chưa packed luôn được compact bất kể waste — migration một lần
     * sang bất biến cửa sổ 64KB; sau đó trigger phụ này tự im lặng. */
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
 * release() chỉ enqueue path; worker thread lấy file lock của path và chạy
 * compact — FUSE op không còn trả tiền compact/GC đồng bộ. Queue dedupe
 * theo path; destroy drain hết queue trước khi thoát.
 * ========================================================================= */

static pthread_mutex_t compact_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t compact_queue_cv = PTHREAD_COND_INITIALIZER;
static struct compact_request *compact_queue_head;
static struct compact_request *compact_queue_tail;
static pthread_t compact_worker_thread;
static bool compact_worker_running;
static bool compact_worker_stop;

static int schedule_request(const char *path, bool ordinary,
                            const myfs_adaptive_ticket_t *ticket)
{
    pthread_mutex_lock(&compact_queue_mu);
    if (!compact_worker_running)
    {
        /* Worker chưa chạy (init fail hoặc đang shutdown): fallback đồng bộ
         * như hành vi cũ để không bỏ sót việc thu hồi. */
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
    pthread_cond_broadcast(&compact_queue_cv);
    pthread_mutex_unlock(&compact_queue_mu);

    pthread_join(compact_worker_thread, NULL);

    pthread_mutex_lock(&compact_queue_mu);
    compact_worker_running = false;
    compact_worker_stop = false;
    pthread_mutex_unlock(&compact_queue_mu);
}
