#include "myfs.h"
#include "guards.h"
#define min(a, b) ((a) < (b) ? (a) : (b))

#ifdef MYFS_TEST_FAILPOINTS
#define MYFS_META_CHECKPOINT_RECORDS 8U
#else
#define MYFS_META_CHECKPOINT_RECORDS 1024U
#endif

static bool metadata_checkpoint_needed(const myfs_inode_t *inode)
{
    uint64_t checkpoint_bytes = 64ULL +
        (uint64_t)inode->chunk_map.num_chunks * 32ULL;
    uint64_t byte_limit = checkpoint_bytes > 512ULL * 1024ULL
        ? checkpoint_bytes * 2ULL : 1024ULL * 1024ULL;
    return inode->metadata_delta_count >= MYFS_META_CHECKPOINT_RECORDS ||
           inode->metadata_journal_bytes > byte_limit;
}

/* Caller holds the file lock and this handle's cache write lock.  A failed
 * reopen cannot leave the cache paired with the unlinked pre-checkpoint fd. */
static void checkpoint_handle_metadata_locked(myfs_file_handle_t *handle)
{
    myfs_inode_t *inode = &handle->cached_inode;
    if (!metadata_checkpoint_needed(inode))
        return;

    int checkpoint_ret = save_chunk_map_for_storage(&handle->storage, inode);
    uint64_t checkpoint_epoch =
        generation_bump_metadata_epoch_locked(handle);
    if (checkpoint_ret == 0)
    {
        int new_meta_fd = open(handle->storage.meta_path,
                               O_RDWR | O_CLOEXEC);
        if (new_meta_fd >= 0)
        {
            close(handle->meta_fd);
            handle->meta_fd = new_meta_fd;
            handle->seen_metadata_epoch = checkpoint_epoch;
            return;
        }
        LOG("[WARN] metadata checkpoint reopen failed: %s\n",
            strerror(errno));
    }
    else
        LOG("[WARN] metadata checkpoint failed: %d\n", checkpoint_ret);
    handle->cache_valid = false;
}

static myfs_file_handle_t *get_file_handle(struct fuse_file_info *fi)
{
    if (!fi || fi->fh == 0)
        return NULL;
    return (myfs_file_handle_t *)(uintptr_t)fi->fh;
}

/* Caller holds the logical path lock so compaction cannot supersede the
 * resolved generation between opening its descriptors and registering the
 * reference.  attach_file_handle_locked() initializes the cache_lock used by
 * subsequent handle operations and live handoff. */
static int attach_file_handle_locked(const myfs_storage_t *storage, int data_fd,
                                     int flags, struct fuse_file_info *fi)
{
    int meta_open_flags = ((flags & O_ACCMODE) == O_RDONLY) ? O_RDONLY : O_RDWR;
    int meta_fd = open(storage->meta_path, meta_open_flags | O_CLOEXEC);
    if (meta_fd < 0)
        return -errno;

    myfs_file_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle)
    {
        close(meta_fd);
        return -ENOMEM;
    }
    handle->data_fd = data_fd;
    handle->meta_fd = meta_fd;
    handle->flags = flags;
    handle->storage = *storage;

    pthread_rwlockattr_t attr;
    int lock_ret = pthread_rwlockattr_init(&attr);
    bool attr_initialized = lock_ret == 0;
#if defined(__GLIBC__)
    if (lock_ret == 0)
        lock_ret = pthread_rwlockattr_setkind_np(
            &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#else
    if (lock_ret == 0)
        lock_ret = ENOTSUP;
#endif
    if (lock_ret == 0)
        lock_ret = pthread_rwlock_init(&handle->cache_lock, &attr);
    if (attr_initialized)
        (void)pthread_rwlockattr_destroy(&attr);
    if (lock_ret != 0)
    {
        close(meta_fd);
        free(handle);
        return -lock_ret;
    }

    int ret = load_chunk_map_from_fd(meta_fd, &handle->cached_inode);
    if (ret != 0)
    {
        pthread_rwlock_destroy(&handle->cache_lock);
        close(meta_fd);
        free(handle);
        return ret;
    }
    handle->cache_valid = true;

    ret = register_generation_handle_locked(handle);
    if (ret != 0)
    {
        free(handle->cached_inode.chunk_map.chunks);
        pthread_rwlock_destroy(&handle->cache_lock);
        close(meta_fd);
        free(handle);
        return ret;
    }
    fi->fh = (uint64_t)(uintptr_t)handle;
    return 0;
}

/*
 * Tạo một file thường mới trong filesystem.
 * File dữ liệu thực tế được tạo dưới dạng .data, sau đó metadata .meta được
 * khởi tạo rỗng để bảo đảm đối tượng mới có trạng thái nhất quán ngay từ đầu.
 */
static int myfs_create_locked(const char *path, mode_t mode,
                              struct fuse_file_info *fi)
{
    LOG("[DEBUG] create: %s\n", path);

    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    /* Mở fd .data ban đầu ở chế độ đọc/ghi để gắn vào handle; live handoff có
     * thể thay fd này bằng fd của generation mới. */
    int fd = open(data_path, O_CREAT | O_RDWR | O_TRUNC, mode);
    if (fd == -1)
        return -errno;

    /* Khởi tạo metadata rỗng tương ứng với file mới tạo. */
    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
    {
        close(fd);
        return ret;
    }
    myfs_inode_t inode = {0};
    ret = save_chunk_map_for_storage(&storage, &inode);
    if (ret != 0)
    {
        close(fd);
        return ret;
    }

    ret = attach_file_handle_locked(&storage, fd, fi->flags, fi);
    if (ret != 0)
    {
        close(fd);
        return ret;
    }

    return 0;
}

int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;

    int ret = myfs_create_locked(path, mode, fi);
    myfs_unlock_file(lk);
    return ret;
}

/*
 * Mở storage hiện hành và lưu con trỏ handle sở hữu data/meta fd vào fi->fh.
 * Nếu kernel yêu cầu mở với O_TRUNC, metadata cũng phải được reset tương ứng
 * để tránh ghép dữ liệu mới lên chunk map cũ.
 */
static int myfs_open_locked(const char *path, struct fuse_file_info *fi)
{
    LOG("[DEBUG] open: %s flags=0x%x\n", path, fi->flags);

    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
        return ret;

    int recovery_ret = recover_generations_for_path_locked(path, &storage);
    if (recovery_ret != 0)
        LOG("[WARN] open: generation recovery returned %d\n", recovery_ret);

    /* O_DIRECT là chỉ thị cho kernel FUSE (bypass page cache phía user),
     * KHÔNG được chuyển xuống backing store: pread nội bộ dùng buffer thường,
     * không thoả ràng buộc alignment của O_DIRECT trên ext4 → EINVAL. */
    int data_open_flags = ((fi->flags & O_ACCMODE) == O_RDONLY)
        ? O_RDONLY : O_RDWR;
    int fd = open(storage.data_path, data_open_flags | O_CLOEXEC);
    if (fd == -1)
    {
        perror("[ERROR] open .data");
        return -errno;
    }
    /*
     * Khi shell redirect như `echo > file`, kernel thường đi qua open(O_TRUNC)
     * thay vì gọi truncate() tách biệt. Vì vậy cần xoá sạch chunk map tại đây
     * để các lần ghi tiếp theo bắt đầu từ một trạng thái rỗng.
     */
    if (fi->flags & O_TRUNC)
    {
        LOG("[DEBUG] open: O_TRUNC detected, clearing chunk map\n");
        myfs_inode_t inode = {0};
        ret = load_chunk_map_from_path(storage.meta_path, &inode);
        if (ret != 0)
        {
            close(fd);
            return ret;
        }
        /* Publish an empty map but leave old blobs orphaned.  A read already
         * using the old cache bundle can then finish safely; compaction will
         * reclaim the physical bytes later. */
        free(inode.chunk_map.chunks);
        inode.chunk_map.chunks = NULL;
        inode.chunk_map.num_chunks = 0;
        inode.chunk_map.logical_size = 0;
        inode.chunk_map.fully_packed = true;
        ret = save_chunk_map_for_storage(&storage, &inode);
        /* rename may have committed even when the durability/alias step
         * reports an error, so existing handles must refresh either way. */
        generation_bump_storage_epoch_locked(&storage);
        if (ret != 0)
        {
            close(fd);
            return ret;
        }
    }

    ret = attach_file_handle_locked(&storage, fd, fi->flags, fi);
    if (ret != 0)
    {
        close(fd);
        return ret;
    }
    return ret;
}

int myfs_open(const char *path, struct fuse_file_info *fi)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;

    int ret = myfs_open_locked(path, fi);
    myfs_unlock_file(lk);
    return ret;
}

/*
 * Khi truncate cắt giữa một chunk NÉN, ghi lại prefix còn giữ thành blob mới để
 * frame và stored_size cùng mô tả một payload: đọc + verify CRC, giải nén, cắt,
 * nén lại (raw fallback như write path), append vào cuối .data, fdatasync rồi
 * cập nhật entry. Decoder prefix vẫn giữ tương thích với metadata cũ từng chỉ
 * shrink stored_size; blob cũ thành orphan chờ compact.
 */
static int rewrite_truncated_chunk_fd(int fd, myfs_chunk_t *chunk,
                                      uint32_t new_stored)
{
    char *plain_buf = malloc(chunk->stored_size);
    if (!plain_buf)
        return -ENOMEM;

    int ret = myfs_chunk_payload_load(fd, chunk, plain_buf);
    if (ret == 0)
    {
        off_t eof = lseek(fd, 0, SEEK_END);
        if (eof < 0)
            ret = -errno;
        else
        {
            /* Blob mới phải bền trên disk trước khi meta trỏ tới nó. */
            myfs_chunk_t out;
            ret = myfs_blob_append(fd, &eof, plain_buf, new_stored,
                                   chunk->logical_offset, &out);
            if (ret == 0 && fdatasync(fd) != 0)
                ret = -errno;
            if (ret == 0)
                *chunk = out;
        }
    }
    if (ret != 0)
        LOG("[ERROR] truncate: chunk rewrite failed (%d)\n", ret);
    free(plain_buf);
    return ret;
}

static int rewrite_truncated_chunk(const myfs_storage_t *storage,
                                   myfs_chunk_t *chunk, uint32_t new_stored)
{
    int fd = open(storage->data_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int ret = rewrite_truncated_chunk_fd(fd, chunk, new_stored);
    close(fd);
    return ret;
}

static int truncate_open_handle_locked(myfs_file_handle_t *handle, off_t size)
{
    if (size < 0)
        return -EINVAL;
    if ((handle->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    int ret = pthread_rwlock_wrlock(&handle->cache_lock);
    if (ret != 0)
        return -ret;

    uint64_t epoch;
    bool superseded;
    generation_state_snapshot(handle, &epoch, &superseded);
    if (superseded)
    {
        pthread_rwlock_unlock(&handle->cache_lock);
        return -ESTALE;
    }
    if (!handle->cache_valid || handle->seen_metadata_epoch != epoch)
    {
        int new_meta_fd = open(handle->storage.meta_path, O_RDWR | O_CLOEXEC);
        myfs_inode_t fresh = {0};
        if (new_meta_fd < 0)
            ret = -errno;
        else
            ret = load_chunk_map_from_fd(new_meta_fd, &fresh);
        if (ret != 0)
        {
            if (new_meta_fd >= 0)
                close(new_meta_fd);
            pthread_rwlock_unlock(&handle->cache_lock);
            return ret;
        }
        close(handle->meta_fd);
        free(handle->cached_inode.chunk_map.chunks);
        handle->meta_fd = new_meta_fd;
        handle->cached_inode = fresh;
        handle->cache_valid = true;
        handle->seen_metadata_epoch = epoch;
    }

    myfs_inode_t *current = &handle->cached_inode;
    if (current->metadata_version == MYFS_META_VERSION_LEGACY)
    {
        ret = save_chunk_map_for_storage(&handle->storage, current);
        uint64_t converted_epoch =
            generation_bump_metadata_epoch_locked(handle);
        if (ret == 0)
        {
            int new_meta_fd = open(handle->storage.meta_path,
                                   O_RDWR | O_CLOEXEC);
            if (new_meta_fd < 0)
            {
                ret = -errno;
                handle->cache_valid = false;
            }
            else
            {
                close(handle->meta_fd);
                handle->meta_fd = new_meta_fd;
                handle->seen_metadata_epoch = converted_epoch;
            }
        }
        else
            handle->cache_valid = false;
        if (ret != 0)
        {
            pthread_rwlock_unlock(&handle->cache_lock);
            return ret;
        }
    }

    myfs_inode_t next = *current;
    size_t chunks_bytes = (size_t)current->chunk_map.num_chunks *
                          sizeof(myfs_chunk_t);
    next.chunk_map.chunks = chunks_bytes ? malloc(chunks_bytes) : NULL;
    if (chunks_bytes && !next.chunk_map.chunks)
    {
        pthread_rwlock_unlock(&handle->cache_lock);
        return -ENOMEM;
    }
    if (chunks_bytes)
        memcpy(next.chunk_map.chunks, current->chunk_map.chunks, chunks_bytes);
    uint32_t old_count = current->chunk_map.num_chunks;

    if ((uint64_t)size < INODE_LSIZE(next))
    {
        uint32_t keep = 0;
        for (uint32_t i = 0; i < next.chunk_map.num_chunks; i++)
        {
            myfs_chunk_t *chunk = &next.chunk_map.chunks[i];
            if ((off_t)chunk->logical_offset >= size)
                break;
            keep++;
            off_t chunk_end = (off_t)chunk->logical_offset +
                              (off_t)chunk->stored_size;
            if (chunk_end > size)
            {
                uint32_t new_stored =
                    (uint32_t)(size - (off_t)chunk->logical_offset);
                if (chunk->codec_type == 1)
                    ret = rewrite_truncated_chunk_fd(handle->data_fd, chunk,
                                                     new_stored);
                else
                    chunk->stored_size = new_stored;
                if (ret != 0)
                    break;
            }
        }
        next.chunk_map.num_chunks = keep;
    }
    INODE_LSIZE(next) = (uint64_t)size;
    next.chunk_map.fully_packed =
        chunk_map_is_packed(&next.chunk_map, next.window_size);
    if (ret == 0 && fdatasync(handle->data_fd) != 0)
        ret = -errno;
    if (ret == 0)
        ret = append_chunk_map_delta_to_fd(handle->meta_fd, &next,
                                           0, old_count,
                                           next.chunk_map.num_chunks);
    if (ret == 0)
    {
        myfs_chunk_t *old_chunks = current->chunk_map.chunks;
        *current = next;
        free(old_chunks);
        handle->seen_metadata_epoch =
            generation_bump_metadata_epoch_locked(handle);
        checkpoint_handle_metadata_locked(handle);
    }
    else
        free(next.chunk_map.chunks);
    pthread_rwlock_unlock(&handle->cache_lock);
    return ret;
}

/*
 * Thay đổi kích thước logic của file và đồng bộ lại metadata tương ứng.
 * Hàm xử lý ba trường hợp chính: thu nhỏ về 0, cắt ngắn file, hoặc kéo dài
 * kích thước logic mà chưa tạo thêm chunk mới.
 */
static int myfs_truncate_locked(const char *path, off_t size,
                                struct fuse_file_info *fi)
{
    LOG("[DEBUG] truncate: %s to %ld bytes\n", path, size);

    myfs_file_handle_t *handle = get_file_handle(fi);
    if (handle)
        return truncate_open_handle_locked(handle, size);
    if (size < 0)
        return -EINVAL;

    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
        return ret;

    myfs_inode_t inode = {0};
    ret = load_chunk_map_from_path(storage.meta_path, &inode);
    if (ret != 0)
        return ret;

    /*
     * Trường hợp 1: truncate về 0.
     * Đây là tình huống thường gặp khi redirect ghi đè file. Công bố chunk map
     * rỗng và logical size 0; blob vật lý cũ thành orphan chờ compact.
     */
    if (size == 0)
    {
        free(inode.chunk_map.chunks);
        inode.chunk_map.chunks = NULL;
        inode.chunk_map.num_chunks = 0;
        INODE_LSIZE(inode) = 0;
        inode.chunk_map.fully_packed = true;
        ret = save_chunk_map_for_storage(&storage, &inode);
        generation_bump_storage_epoch_locked(&storage);
        return ret;
    }

    /*
     * Trường hợp 2: cắt ngắn file.
     * Các chunk nằm hoàn toàn ngoài miền kích thước mới sẽ bị loại bỏ, còn
     * chunk cuối cùng có thể phải giảm stored_size để phản ánh phần còn lại.
     */
    if (size < (off_t)INODE_LSIZE(inode))
    {
        uint32_t keep = 0;
        for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
        {
            myfs_chunk_t *c = &inode.chunk_map.chunks[i];
            if ((off_t)c->logical_offset >= size)
                break; // chunk này và các chunk sau đều nằm ngoài size mới

            keep++;

            // Chunk cuối có thể bị cắt giữa chừng
            off_t chunk_end = (off_t)c->logical_offset + (off_t)c->stored_size;
            if (chunk_end > size)
            {
                uint32_t new_stored = (uint32_t)(size - (off_t)c->logical_offset);
                if (c->codec_type == 1)
                {
                    /* Ghi lại prefix nén để frame mới và stored_size cùng mô tả
                     * payload còn giữ. */
                    ret = rewrite_truncated_chunk(&storage, c, new_stored);
                    if (ret != 0)
                    {
                        free(inode.chunk_map.chunks);
                        return ret;
                    }
                }
                else
                {
                    /* Blob raw: đọc chỉ copy stored_size byte đầu nên shrink
                     * metadata là đủ; blob trên disk giữ nguyên, CRC vẫn khớp. */
                    c->stored_size = new_stored;
                }
            }
        }
        inode.chunk_map.num_chunks = keep;
        INODE_LSIZE(inode) = size;
        ret = save_chunk_map_for_storage(&storage, &inode);
        generation_bump_storage_epoch_locked(&storage);
        free(inode.chunk_map.chunks);
        return ret;
    }

    /*
     * Trường hợp 3: kéo dài file.
     * Ở giai đoạn này chỉ cập nhật logical size; vùng mới được xem là khoảng
     * trống và sẽ đọc ra byte 0 cho tới khi có dữ liệu được ghi vào.
     */
    INODE_LSIZE(inode) = size;
    ret = save_chunk_map_for_storage(&storage, &inode);
    generation_bump_storage_epoch_locked(&storage);
    free(inode.chunk_map.chunks);
    return ret;
}

int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;

    int ret = myfs_truncate_locked(path, size, fi);
    myfs_unlock_file(lk);
    return ret;
}

int myfs_refresh_handle_cache_locked(myfs_file_handle_t *handle)
{
    int ret = pthread_rwlock_wrlock(&handle->cache_lock);
    if (ret != 0)
        return -ret;

    uint64_t epoch;
    bool superseded;
    generation_state_snapshot(handle, &epoch, &superseded);
    if (handle->seen_metadata_epoch != epoch)
    {
        if (superseded)
        {
            myfs_inode_t fresh = {0};
            int new_meta_fd = open(handle->storage.meta_path,
                                   O_RDONLY | O_CLOEXEC);
            if (new_meta_fd >= 0)
                ret = load_chunk_map_from_fd(new_meta_fd, &fresh);
            else
                ret = -errno;
            if (ret == 0)
            {
                myfs_inode_t old_inode = handle->cached_inode;
                close(handle->meta_fd);
                handle->meta_fd = new_meta_fd;
                handle->cached_inode = fresh;
                handle->cache_valid = true;
                handle->seen_metadata_epoch = epoch;
                free(old_inode.chunk_map.chunks);
            }
            else if (new_meta_fd >= 0)
                close(new_meta_fd);
        }
        else
        {
            int flags = ((handle->flags & O_ACCMODE) == O_RDONLY)
                ? O_RDONLY : O_RDWR;
            int new_meta_fd = open(handle->storage.meta_path,
                                   flags | O_CLOEXEC);
            if (new_meta_fd < 0)
                ret = -errno;
            else
            {
                myfs_inode_t fresh = {0};
                ret = load_chunk_map_from_fd(new_meta_fd, &fresh);
                if (ret == 0)
                {
                    int old_meta_fd = handle->meta_fd;
                    myfs_inode_t old_inode = handle->cached_inode;
                    handle->meta_fd = new_meta_fd;
                    handle->cached_inode = fresh;
                    handle->cache_valid = true;
                    handle->seen_metadata_epoch = epoch;
                    close(old_meta_fd);
                    free(old_inode.chunk_map.chunks);
                }
                else
                    close(new_meta_fd);
            }
        }
    }
    pthread_rwlock_unlock(&handle->cache_lock);
    return ret;
}

static int refresh_handle_cache(const char *path, myfs_file_handle_t *handle)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;
    int ret = myfs_refresh_handle_cache_locked(handle);
    myfs_unlock_file(lk);
    return ret;
}

static int read_from_inode(int fd, const myfs_inode_t *inode, char *buf,
                           size_t size, off_t offset)
{
    if (offset < 0)
        return -EINVAL;
    if (offset >= (off_t)INODE_LSIZE(*inode))
        return 0;

    size_t bytes_read = 0;
    off_t cur_offset = offset;
    size_t remaining = size;

    while (remaining > 0 && cur_offset < (off_t)INODE_LSIZE(*inode))
    {
        int32_t chunk_idx = -1;
        if (inode->chunk_map.fully_packed)
        {
            /* Packed: chunk chứa cur_offset chỉ có thể bắt đầu tại window base
             * của file — binary search thay vì linear. */
            uint64_t want = myfs_window_base((uint64_t)cur_offset,
                                             inode->window_size);
            uint32_t lo = 0;
            uint32_t hi = inode->chunk_map.num_chunks;
            while (lo < hi)
            {
                uint32_t mid = lo + (hi - lo) / 2;
                if (inode->chunk_map.chunks[mid].logical_offset < want)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < inode->chunk_map.num_chunks &&
                inode->chunk_map.chunks[lo].logical_offset == want &&
                cur_offset < (off_t)want +
                             (off_t)inode->chunk_map.chunks[lo].stored_size)
                chunk_idx = (int32_t)lo;
        }
        else
        {
            /* Chunk map chưa packed (gồm layout legacy): linear scan chịu được
             * chunk kích thước bất kỳ — tầng fallback vĩnh viễn. */
            for (uint32_t i = 0; i < inode->chunk_map.num_chunks; i++)
            {
                const myfs_chunk_t *c = &inode->chunk_map.chunks[i];
                off_t c_end = (off_t)c->logical_offset + (off_t)c->stored_size;
                if ((off_t)c->logical_offset <= cur_offset && cur_offset < c_end)
                {
                    chunk_idx = (int32_t)i;
                    break;
                }
            }
        }
        if (chunk_idx < 0)
        {
            /* cur_offset nằm trong hole. Lấp zero tới chunk kế tiếp (mảng đã
             * sắp nên là chunk đầu tiên có logical_offset > cur_offset) hoặc
             * tới EOF logic — break sớm sẽ trả thiếu byte và kernel hiểu là
             * EOF, làm hole đọc ra rỗng thay vì byte 0. */
            off_t next_start = (off_t)INODE_LSIZE(*inode);
            for (uint32_t i = 0; i < inode->chunk_map.num_chunks; i++)
            {
                off_t c_start = (off_t)inode->chunk_map.chunks[i].logical_offset;
                if (c_start > cur_offset)
                {
                    next_start = c_start;
                    break;
                }
            }
            size_t hole_remaining =
                (size_t)(INODE_LSIZE(*inode) - (size_t)cur_offset);
            size_t gap = (size_t)(next_start - cur_offset);
            size_t zero_len = min(remaining, min(gap, hole_remaining));
            if (zero_len == 0)
                break;
            memset(buf + bytes_read, 0, zero_len);
            bytes_read += zero_len;
            cur_offset += zero_len;
            remaining -= zero_len;
            continue;
        }

        const myfs_chunk_t *chunk =
            &inode->chunk_map.chunks[(uint32_t)chunk_idx];
        off_t chunk_logical_start = chunk->logical_offset;

        if (guard_chunk_logical_offset(cur_offset, chunk_logical_start) < 0)
            return -EIO;

        if (guard_chunk_metadata(chunk, chunk_idx) < 0)
            return -EIO;

        off_t chunk_offset = cur_offset - chunk_logical_start;
        size_t chunk_size = chunk->stored_size;

        if (guard_chunk_bounds((size_t)chunk_offset, chunk_size) < 0)
            return -EIO;

        size_t logical_remaining =
            (size_t)(INODE_LSIZE(*inode) - (size_t)cur_offset);
        size_t bytes_in_chunk = min(remaining, min(chunk_size - chunk_offset, logical_remaining));

        /* pread + verify CRC32 + decompress-or-copy — dùng chung engine với
         * write/truncate/compact; chịu được frame dài hơn stored_size (meta
         * cũ bị truncate shrink trước khi có cơ chế rewrite). */
        char *payload = malloc(chunk->stored_size);
        if (guard_malloc(payload) < 0)
            return -ENOMEM;

        if (myfs_chunk_payload_load(fd, chunk, payload) != 0)
        {
            free(payload);
            return -EIO;
        }

        memcpy(buf + bytes_read, payload + chunk_offset, bytes_in_chunk);
        free(payload);

        bytes_read += bytes_in_chunk;
        cur_offset += bytes_in_chunk;
        remaining -= bytes_in_chunk;
    }

    return (int)bytes_read;
}

/* Handle reads hold cache_lock for the complete lookup/pread/decompress
 * operation.  Uncontended pthread rwlocks stay in userspace, so this preserves
 * the zero-backing-metadata-syscall fast path while making handoff atomic. */
int myfs_read(const char *path, char *buf, size_t size,
              off_t offset, struct fuse_file_info *fi)
{
    LOG("[DEBUG] myfs_read: %s offset=%ld size=%zu\n", path, offset, size);
    myfs_file_handle_t *handle = get_file_handle(fi);
    if (handle)
    {
        for (;;)
        {
            int lock_ret = pthread_rwlock_rdlock(&handle->cache_lock);
            if (lock_ret != 0)
                return -lock_ret;
            uint64_t epoch;
            generation_state_snapshot(handle, &epoch, NULL);
            if (handle->cache_valid && handle->seen_metadata_epoch == epoch)
            {
#ifdef MYFS_TEST_FAILPOINTS
                const char *hold_ms_text = getenv("MYFS_TEST_READ_HOLD_MS");
                if (hold_ms_text)
                {
                    long hold_ms = strtol(hold_ms_text, NULL, 10);
                    if (hold_ms > 0 && hold_ms <= 1000)
                    {
                        struct timespec hold = {
                            .tv_sec = hold_ms / 1000,
                            .tv_nsec = (hold_ms % 1000) * 1000000L,
                        };
                        nanosleep(&hold, NULL);
                    }
                }
#endif
                int ret = read_from_inode(handle->data_fd,
                                          &handle->cached_inode,
                                          buf, size, offset);
                pthread_rwlock_unlock(&handle->cache_lock);
                return ret;
            }
            pthread_rwlock_unlock(&handle->cache_lock);
            int ret = refresh_handle_cache(path, handle);
            if (ret != 0)
                return ret;
        }
    }

    myfs_inode_t inode = {0};
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;
    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret == 0)
        ret = load_chunk_map_from_path(storage.meta_path, &inode);
    int fd = -1;
    if (ret == 0)
    {
        fd = open(storage.data_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            ret = -errno;
    }
    myfs_unlock_file(lk);
    if (ret == 0)
        ret = read_from_inode(fd, &inode, buf, size, offset);
    if (fd >= 0)
        close(fd);
    free(inode.chunk_map.chunks);
    return ret;
}

/*
 * Ghi dữ liệu vào file logic theo window_size cố định của generation: vùng ghi
 * được chia theo các cửa sổ chứa nó, mỗi cửa sổ bị chạm được repack (merge dữ
 * liệu cũ + patch mới) thành đúng một chunk head-aligned. Append thuần,
 * overwrite một phần và write vào hole đều đi chung một đường — không còn
 * nhánh RMW riêng.
 */
static int myfs_write_locked(const char *path, const char *buf, size_t size,
                             off_t offset, struct fuse_file_info *fi,
                             myfs_adaptive_ticket_t *adaptive_ticket)
{
    LOG("[DEBUG] write: %s offset=%ld size=%zu\n", path, offset, size);

    if (size == 0)
        return 0;

    myfs_file_handle_t *handle = get_file_handle(fi);
    if (!handle)
        return -EIO;
    if ((handle->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (offset < 0 || size > (size_t)(INT64_MAX - offset))
        return -EFBIG;

    int ret = pthread_rwlock_wrlock(&handle->cache_lock);
    if (ret != 0)
        return -ret;
    uint64_t epoch;
    bool superseded;
    generation_state_snapshot(handle, &epoch, &superseded);
    if (superseded)
    {
        pthread_rwlock_unlock(&handle->cache_lock);
        return -ESTALE;
    }
    if (!handle->cache_valid || handle->seen_metadata_epoch != epoch)
    {
        int new_meta_fd = open(handle->storage.meta_path, O_RDWR | O_CLOEXEC);
        myfs_inode_t fresh = {0};
        if (new_meta_fd < 0)
            ret = -errno;
        else
            ret = load_chunk_map_from_fd(new_meta_fd, &fresh);
        if (ret != 0)
        {
            if (new_meta_fd >= 0)
                close(new_meta_fd);
            pthread_rwlock_unlock(&handle->cache_lock);
            return ret;
        }
        close(handle->meta_fd);
        free(handle->cached_inode.chunk_map.chunks);
        handle->meta_fd = new_meta_fd;
        handle->cached_inode = fresh;
        handle->cache_valid = true;
        handle->seen_metadata_epoch = epoch;
    }

    myfs_inode_t *inode = &handle->cached_inode;
    if (inode->metadata_version == MYFS_META_VERSION_LEGACY)
    {
        /* One-time v0 conversion.  Normal writes then append bounded delta
         * records; periodic checkpoints may replace the metadata inode and
         * reopen this handle's metadata descriptor. */
        ret = save_chunk_map_for_storage(&handle->storage, inode);
        uint64_t converted_epoch =
            generation_bump_metadata_epoch_locked(handle);
        if (ret == 0)
        {
            int new_meta_fd = open(handle->storage.meta_path,
                                   O_RDWR | O_CLOEXEC);
            if (new_meta_fd < 0)
            {
                ret = -errno;
                handle->cache_valid = false;
            }
            else
            {
                close(handle->meta_fd);
                handle->meta_fd = new_meta_fd;
                handle->seen_metadata_epoch = converted_epoch;
            }
        }
        else
            handle->cache_valid = false;
        if (ret != 0)
        {
            pthread_rwlock_unlock(&handle->cache_lock);
            return ret;
        }
    }

    uint32_t window_size = inode->window_size;
    off_t write_end = offset + (off_t)size;
    uint64_t full_windows = 0;
    uint64_t partial_rmw = 0;
    uint64_t first_touched = myfs_window_base((uint64_t)offset, window_size);
    uint64_t touched_hi;
    ret = myfs_window_ceil((uint64_t)write_end, window_size, &touched_hi);
    if (ret != 0)
    {
        pthread_rwlock_unlock(&handle->cache_lock);
        return ret;
    }
    for (uint64_t win = first_touched; win < touched_hi; win += window_size)
    {
        uint64_t win_end = win + window_size;
        if ((uint64_t)offset <= win && (uint64_t)write_end >= win_end)
        {
            full_windows++;
            continue;
        }
        for (uint32_t i = 0; i < inode->chunk_map.num_chunks; i++)
        {
            const myfs_chunk_t *chunk = &inode->chunk_map.chunks[i];
            uint64_t chunk_end = chunk->logical_offset + chunk->stored_size;
            if (chunk_end <= win)
                continue;
            if (chunk->logical_offset >= win_end)
                break;
            partial_rmw++;
            break;
        }
    }

    /*
     * Xác định dải chunk bị tiêu thụ và miền cửa sổ cần repack.
     * Miền khởi đầu là các cửa sổ theo window_size của generation hiện tại phủ
     * vùng ghi; chunk legacy có thể vắt qua ranh giới cửa sổ nên miền mở rộng
     * theo fixpoint cho tới khi không kéo thêm chunk nào nữa (file đã packed:
     * hội tụ ngay vòng đầu).
     */
    off_t region_lo = (off_t)myfs_window_base((uint64_t)offset, window_size);
    uint64_t region_hi_u64;
    ret = myfs_window_ceil((uint64_t)write_end, window_size, &region_hi_u64);
    if (ret != 0 || region_hi_u64 > INT64_MAX)
    {
        pthread_rwlock_unlock(&handle->cache_lock);
        return ret != 0 ? ret : -EFBIG;
    }
    off_t region_hi = (off_t)region_hi_u64;
    uint32_t first_idx = 0;
    uint32_t end_idx = 0;
    bool changed = true;
    uint32_t guard_iter = 0;
    while (changed)
    {
        if (guard_iter++ > inode->chunk_map.num_chunks + 1)
        {
            /* Miền đơn điệu tăng nên không thể lặp mãi — chặn phòng hờ. */
            pthread_rwlock_unlock(&handle->cache_lock);
            return -EIO;
        }
        changed = false;

        first_idx = 0;
        while (first_idx < inode->chunk_map.num_chunks)
        {
            myfs_chunk_t *c = &inode->chunk_map.chunks[first_idx];
            if ((off_t)c->logical_offset + (off_t)c->stored_size > region_lo)
                break;
            first_idx++;
        }

        off_t min_start = region_lo;
        off_t max_end = region_hi;
        end_idx = first_idx;
        while (end_idx < inode->chunk_map.num_chunks)
        {
            myfs_chunk_t *c = &inode->chunk_map.chunks[end_idx];
            if ((off_t)c->logical_offset >= region_hi)
                break;
            off_t c_end = (off_t)c->logical_offset + (off_t)c->stored_size;
            if ((off_t)c->logical_offset < min_start)
                min_start = (off_t)c->logical_offset;
            if (c_end > max_end)
                max_end = c_end;
            end_idx++;
        }

        off_t new_lo = (off_t)myfs_window_base((uint64_t)min_start,
                                               window_size);
        uint64_t new_hi_u64;
        ret = myfs_window_ceil((uint64_t)max_end, window_size, &new_hi_u64);
        if (ret != 0 || new_hi_u64 > INT64_MAX)
        {
            pthread_rwlock_unlock(&handle->cache_lock);
            return ret != 0 ? ret : -EFBIG;
        }
        off_t new_hi = (off_t)new_hi_u64;
        if (new_lo < region_lo || new_hi > region_hi)
        {
            region_lo = new_lo;
            region_hi = new_hi;
            changed = true;
        }
    }
    uint32_t consumed = end_idx - first_idx;

    /*
     * Một fd O_RDWR duy nhất: engine đọc chunk cũ và append blob mới trên cùng
     * .data; một fdatasync cho cả batch TRƯỚC khi publish metadata — crash
     * không thể để .meta tham chiếu blob chưa persist.
     */
    int fd = handle->data_fd;
    off_t eof = lseek(fd, 0, SEEK_END);
    if (eof < 0)
    {
        pthread_rwlock_unlock(&handle->cache_lock);
        return -errno;
    }

    myfs_chunk_t *entries = NULL;
    uint32_t entry_count = 0;
    ret = myfs_repack_windows(fd, fd, &eof, &inode->chunk_map,
                              window_size,
                              first_idx, consumed, region_lo, region_hi,
                              buf, offset, size, &entries, &entry_count);
    if (ret == 0 && fdatasync(fd) != 0)
        ret = -errno;
    if (ret != 0)
    {
        free(entries);
        pthread_rwlock_unlock(&handle->cache_lock);
        return ret;
    }

    uint32_t old_count = inode->chunk_map.num_chunks;
    uint32_t new_count = old_count - consumed + entry_count;
    myfs_chunk_t *new_chunks = new_count
        ? malloc((size_t)new_count * sizeof(*new_chunks)) : NULL;
    if (new_count && !new_chunks)
    {
        free(entries);
        pthread_rwlock_unlock(&handle->cache_lock);
        return -ENOMEM;
    }
    if (first_idx)
        memcpy(new_chunks, inode->chunk_map.chunks,
               (size_t)first_idx * sizeof(*new_chunks));
    if (entry_count > 0)
        memcpy(&new_chunks[first_idx], entries,
               entry_count * sizeof(myfs_chunk_t));
    uint32_t tail = old_count - first_idx - consumed;
    if (tail)
        memcpy(&new_chunks[first_idx + entry_count],
               &inode->chunk_map.chunks[first_idx + consumed],
               (size_t)tail * sizeof(*new_chunks));
    free(entries);

    myfs_inode_t next = *inode;
    next.chunk_map.chunks = new_chunks;
    next.chunk_map.num_chunks = new_count;
    next.chunk_map.fully_packed =
        chunk_map_is_packed(&next.chunk_map, next.window_size);
    if (write_end > (off_t)INODE_LSIZE(next))
        INODE_LSIZE(next) = write_end;
    ret = append_chunk_map_delta_to_fd(handle->meta_fd, &next,
                                       first_idx, consumed, entry_count);
    if (ret != 0)
    {
        free(new_chunks);
        pthread_rwlock_unlock(&handle->cache_lock);
        return ret;
    }

    myfs_chunk_t *old_chunks = inode->chunk_map.chunks;
    *inode = next;
    free(old_chunks);
    handle->seen_metadata_epoch =
        generation_bump_metadata_epoch_locked(handle);
    (void)generation_observe_write_locked(handle, full_windows, partial_rmw,
                                          adaptive_ticket);

    checkpoint_handle_metadata_locked(handle);

    LOG("[DEBUG] write OK: consumed=%u windows=%u chunks=%u logical_size=%zu\n",
        consumed, entry_count, new_count, (size_t)INODE_LSIZE(*inode));

    pthread_rwlock_unlock(&handle->cache_lock);

    return (int)size;
}

int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;

    myfs_adaptive_ticket_t ticket = {0};
    int ret = myfs_write_locked(path, buf, size, offset, fi, &ticket);
    myfs_unlock_file(lk);
    if (ret >= 0 && ticket.valid)
    {
        int schedule_ret = schedule_adaptive_compaction(path, &ticket);
        if (schedule_ret != 0)
        {
            generation_adaptive_schedule_failed(&ticket);
            LOG("[WARN] write: adaptive scheduling returned %d\n", schedule_ret);
        }
    }
    return ret;
}

/* Giải phóng file descriptor gắn với file đang mở. */
int myfs_release(const char *path, struct fuse_file_info *fi)
{
    myfs_file_handle_t *handle = get_file_handle(fi);
    myfs_adaptive_ticket_t adaptive_ticket = {0};
    if (handle)
    {
        myfs_file_lock_t *lk = myfs_lock_file(path);
        if (!lk)
            return -ENOMEM;

        int lock_ret = pthread_rwlock_wrlock(&handle->cache_lock);
        if (lock_ret != 0)
        {
            myfs_unlock_file(lk);
            return -lock_ret;
        }

        /* Removing the registry reference before retrying GC is the exact point
         * at which this generation can become deletion-eligible. */
        (void)generation_claim_release_locked(handle, &adaptive_ticket);
        unregister_generation_handle_locked(handle);
        fsync(handle->data_fd);
        close(handle->data_fd);
        close(handle->meta_fd);
        free(handle->cached_inode.chunk_map.chunks);
        handle->cached_inode.chunk_map.chunks = NULL;
        handle->cache_valid = false;
        fi->fh = 0;

        int gc_ret = run_generation_gc_locked(path);
        pthread_rwlock_unlock(&handle->cache_lock);
        pthread_rwlock_destroy(&handle->cache_lock);
        myfs_unlock_file(lk);
        free(handle);
        if (gc_ret != 0)
            LOG("[WARN] release: deferred GC returned %d\n", gc_ret);
    }

    bool release_compaction_scheduled = false;
    if (adaptive_ticket.valid)
    {
        int adaptive_ret = schedule_release_compaction(path,
                                                       &adaptive_ticket);
        if (adaptive_ret != 0)
        {
            generation_adaptive_schedule_failed(&adaptive_ticket);
            LOG("[WARN] release: adaptive scheduling returned %d\n",
                adaptive_ret);
        }
        else
            release_compaction_scheduled = true;
    }

    /* Generation GC above runs synchronously while the path lock and
     * handle cache write lock are held.  Only compaction scheduling happens
     * here, after those locks have been released; the worker or synchronous
     * fallback acquires the path lock before executing it. */
    if (!release_compaction_scheduled)
    {
        int compact_ret = schedule_compaction(path);
        if (compact_ret != 0)
            LOG("[WARN] release: compact scheduling returned %d\n", compact_ret);
    }

    return 0;
}
