#include "myfs.h"

#include <poll.h>

#define COMPACT_THRESHOLD 0.25

struct myfs_generation_record
{
    myfs_storage_t storage;
    unsigned open_refs;
    unsigned writer_refs;
    bool gc_pending;
    bool install_aliases;
    struct myfs_generation_record *next;
};

static struct myfs_generation_record *generation_registry;

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

    struct myfs_generation_record *record = get_generation_record(&handle->storage);
    if (!record)
        return -ENOMEM;

    record->open_refs++;
    if ((handle->flags & O_ACCMODE) != O_RDONLY)
        record->writer_refs++;
    handle->generation_record = record;
    return 0;
}

void unregister_generation_handle_locked(myfs_file_handle_t *handle)
{
    struct myfs_generation_record *record = handle->generation_record;
    if (!record)
        return;

    if (record->open_refs > 0)
        record->open_refs--;
    if ((handle->flags & O_ACCMODE) != O_RDONLY && record->writer_refs > 0)
        record->writer_refs--;
    handle->generation_record = NULL;

    if (record->open_refs == 0 && !record->gc_pending)
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
}

unsigned generation_writer_refs_locked(const myfs_storage_t *storage)
{
    struct myfs_generation_record *record = find_generation_record(storage);
    return record ? record->writer_refs : 0;
}

unsigned generation_open_refs_locked(const myfs_storage_t *storage)
{
    struct myfs_generation_record *record = find_generation_record(storage);
    return record ? record->open_refs : 0;
}

int mark_generation_for_gc_locked(const myfs_storage_t *storage,
                                  bool install_aliases)
{
    struct myfs_generation_record *record = get_generation_record(storage);
    if (!record)
        return -ENOMEM;
    record->gc_pending = true;
    record->install_aliases = record->install_aliases || install_aliases;
    return 0;
}

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

/* Run only while myfs_metadata_mutex is held.  A record enters this list only
 * after the pointer rename and its parent-directory fsync have succeeded. */
int run_generation_gc_locked(void)
{
    int first_error = 0;
    struct myfs_generation_record **cursor = &generation_registry;

    while (*cursor)
    {
        struct myfs_generation_record *record = *cursor;
        if (!record->gc_pending)
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

    int gc_ret = run_generation_gc_locked();
    if (gc_ret != 0 && ret == 0)
        ret = gc_ret;
    return ret;
}

void destroy_generation_registry(void)
{
    struct myfs_generation_record *record = generation_registry;
    while (record)
    {
        struct myfs_generation_record *next = record->next;
        free(record);
        record = next;
    }
    generation_registry = NULL;
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

static int compact_data_file_locked(const char *path)
{
    myfs_storage_t old_storage;
    int ret = resolve_storage(path, &old_storage);
    if (ret != 0)
        return ret;

    ret = recover_generations_for_path_locked(path, &old_storage);
    if (ret != 0)
        LOG("[WARN] compact: recovery for %s returned %d\n", path, ret);

    /* A writer's handle is never allowed to become stale.  The final writable
     * release removes its reference before invoking compaction. */
    if (generation_writer_refs_locked(&old_storage) > 0)
    {
        LOG("[DEBUG] compact: deferred; active generation has writers\n");
        return 0;
    }

    myfs_inode_t inode = {0};
    ret = load_chunk_map_from_path(old_storage.meta_path, &inode);
    if (ret != 0)
        return ret;
    if (inode.chunk_map.num_chunks == 0)
    {
        free(inode.chunk_map.chunks);
        return 0;
    }

    struct stat st;
    if (stat(old_storage.data_path, &st) != 0)
    {
        ret = -errno;
        free(inode.chunk_map.chunks);
        return ret;
    }
    off_t data_file_size = st.st_size;

    uint64_t live_bytes = 0;
    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
        live_bytes += inode.chunk_map.chunks[i].raw_size;
    if (live_bytes > (uint64_t)data_file_size)
    {
        free(inode.chunk_map.chunks);
        return -EIO;
    }

    uint64_t wasted_bytes = (uint64_t)data_file_size - live_bytes;
    double wasted = data_file_size > 0
        ? (double)wasted_bytes / (double)data_file_size : 0.0;
    /* File chưa packed luôn được compact bất kể waste — migration một lần
     * sang bất biến cửa sổ 64KB; sau đó trigger phụ này tự im lặng. */
    if ((wasted < COMPACT_THRESHOLD || wasted_bytes < CHUNK_SIZE) &&
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
        return ret;
    }

    myfs_storage_t new_storage;
    ret = create_generation_storage(path, st.st_mode, &new_storage);
    if (ret != 0)
    {
        close(src_fd);
        free(inode.chunk_map.chunks);
        return ret;
    }

    int dst_fd = open(new_storage.data_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (dst_fd < 0)
    {
        ret = -errno;
        close(src_fd);
        remove_generation_storage(&new_storage);
        free(inode.chunk_map.chunks);
        return ret;
    }

    off_t new_phys = 0;
    if (inode.chunk_map.fully_packed)
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
        /* File legacy chưa packed: repack toàn bộ nội dung vào cửa sổ 64KB —
         * migration một lần; sau lần compact này file thoả bất biến packed
         * và các lần đọc sau dùng được lookup theo cửa sổ. */
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
        ret = myfs_repack_windows(src_fd, dst_fd, &new_phys, &inode.chunk_map,
                                  0, inode.chunk_map.num_chunks,
                                  0, (off_t)WINDOW_CEIL(max_end),
                                  NULL, 0, 0, &entries, &entry_count);
        if (ret == 0)
        {
            free(inode.chunk_map.chunks);
            inode.chunk_map.chunks = entries;
            inode.chunk_map.num_chunks = entry_count;
            inode.chunk_map.fully_packed = true;
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
        return ret;
    }

    compact_test_failpoint(path, "prepared_before_pointer", &new_storage);

    ret = publish_generation(path, &new_storage);
    if (ret != 0)
    {
        /* rename may already have occurred if only the final directory fsync
         * failed.  Keeping both generations is always safe; never guess here. */
        free(inode.chunk_map.chunks);
        return ret;
    }

    compact_test_failpoint(path, "pointer_durable_before_gc", &old_storage);

    ret = mark_generation_for_gc_locked(&old_storage, true);
    if (ret == 0)
        ret = run_generation_gc_locked();

    free(inode.chunk_map.chunks);
    LOG("[DEBUG] compact: committed generation=%s new_data_size=%lld (was %lld)\n",
        new_storage.generation_id, (long long)new_phys,
        (long long)data_file_size);
    return ret;
}

int compact_data_file(const char *path)
{
    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    int ret = compact_data_file_locked(path);
    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0)
        return -unlock_ret;
    return ret;
}
