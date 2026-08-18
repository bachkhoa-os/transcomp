#include "myfs.h"
#include "guards.h"
#define min(a, b) ((a) < (b) ? (a) : (b))

static myfs_file_handle_t *get_file_handle(struct fuse_file_info *fi)
{
    if (!fi || fi->fh == 0)
        return NULL;
    return (myfs_file_handle_t *)(uintptr_t)fi->fh;
}

/* Caller holds myfs_metadata_mutex so the resolved generation cannot be
 * superseded between opening its descriptors and registering the reference. */
static int attach_file_handle_locked(const myfs_storage_t *storage, int data_fd,
                                     int flags, struct fuse_file_info *fi)
{
    int meta_fd = open(storage->meta_path, O_RDONLY | O_CLOEXEC);
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

    int ret = register_generation_handle_locked(handle);
    if (ret != 0)
    {
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

    /* Tạo file .data ở chế độ đọc/ghi để fd này có thể dùng xuyên suốt vòng đời mở file. */
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
 * Mở một file dữ liệu hiện có và lưu file descriptor vào fi->fh.
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
    int fd = open(storage.data_path, (fi->flags & ~O_DIRECT) | O_CLOEXEC);
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
        /* Chunk map rỗng và logical size bằng 0. */
        if (save_chunk_map_for_storage(&storage, &inode) != 0)
        {
            close(fd);
            return -EIO;
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
 * Truncate cắt vào giữa một chunk NÉN: không thể chỉ shrink stored_size trong
 * metadata — blob cũ giải nén ra nhiều hơn stored_size mới nên mọi lần đọc sau
 * sẽ fail (frame không vừa buffer). Ghi lại phần còn giữ thành blob mới: đọc +
 * verify CRC, giải nén, cắt, nén lại (raw fallback như write path), append vào
 * cuối .data, fdatasync rồi cập nhật entry. Blob cũ thành orphan chờ compact.
 */
static int rewrite_truncated_chunk(const myfs_storage_t *storage,
                                   myfs_chunk_t *chunk, uint32_t new_stored)
{
    int fd = open(storage->data_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    char *plain_buf = malloc(chunk->stored_size);
    if (!plain_buf)
    {
        close(fd);
        return -ENOMEM;
    }

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
    close(fd);
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

    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
        return ret;

    myfs_file_handle_t *handle = get_file_handle(fi);
    if (handle && !storage_generation_equal(&handle->storage, &storage))
        return -ESTALE;

    myfs_inode_t inode = {0};
    ret = load_chunk_map_from_path(storage.meta_path, &inode);
    if (ret != 0)
        return ret;

    /*
     * Trường hợp 1: truncate về 0.
     * Đây là tình huống thường gặp khi redirect ghi đè file. Cần xoá cả dữ liệu
     * lẫn metadata để file thực sự trở về trạng thái rỗng.
     */
    if (size == 0)
    {
        bool opened = false;
        int fd = handle ? handle->data_fd
                        : open(storage.data_path, O_RDWR | O_CLOEXEC);
        if (!handle && fd >= 0)
            opened = true;
        if (fd == -1)
        {
            perror("[ERROR] truncate: open .data");
            free(inode.chunk_map.chunks);
            return -errno;
        }

        if (ftruncate(fd, 0) != 0)
        {
            perror("[ERROR] truncate: ftruncate .data");
            if (opened)
                close(fd);
            free(inode.chunk_map.chunks);
            return -errno;
        }

        if (opened)
            close(fd);

        free(inode.chunk_map.chunks);
        inode.chunk_map.chunks = NULL;
        inode.chunk_map.num_chunks = 0;
        INODE_LSIZE(inode) = 0;
        return save_chunk_map_for_storage(&storage, &inode);
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
                    /* Chunk nén phải được ghi lại — chỉ shrink metadata sẽ làm
                     * mọi lần decompress sau fail vì frame lớn hơn buffer. */
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

/*
 * Đọc dữ liệu theo offset logic bằng cách ánh xạ sang các chunk hiện có.
 * Hàm này nạp metadata, kiểm tra biên, giải nén khi cần và ghép dữ liệu vào
 * buffer đích theo đúng vùng người dùng yêu cầu.
 */
int myfs_read(const char *path, char *buf, size_t size,
              off_t offset, struct fuse_file_info *fi)
{
    LOG("[DEBUG] myfs_read: %s offset=%ld size=%zu\n", path, offset, size);

    myfs_inode_t inode = {0};
    myfs_file_handle_t *handle = get_file_handle(fi);
    int fd = -1;
    int ret;

    if (handle)
    {
        /* The generation pathname is retained until this handle is released.
         * The pinned metadata descriptor is a fallback if a backing pathname
         * disappears because of external interference. */
        ret = load_chunk_map_from_path(handle->storage.meta_path, &inode);
        if (ret == -ENOENT)
            ret = load_chunk_map_from_fd(handle->meta_fd, &inode);
        if (ret != 0)
            return ret;
        fd = dup(handle->data_fd);
        if (fd < 0)
        {
            free(inode.chunk_map.chunks);
            return -errno;
        }
    }
    else
    {
        myfs_file_lock_t *lk = myfs_lock_file(path);
        if (!lk)
            return -ENOMEM;

        myfs_storage_t storage;
        ret = resolve_storage(path, &storage);
        if (ret == 0)
            ret = load_chunk_map_from_path(storage.meta_path, &inode);
        if (ret == 0)
        {
            fd = open(storage.data_path, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                ret = -errno;
        }
        myfs_unlock_file(lk);
        if (ret != 0)
        {
            free(inode.chunk_map.chunks);
            return ret;
        }
    }

    if (offset >= (off_t)INODE_LSIZE(inode))
    {
        close(fd);
        free(inode.chunk_map.chunks);
        return 0;
    }

    size_t bytes_read = 0;
    off_t cur_offset = offset;
    size_t remaining = size;

    while (remaining > 0 && cur_offset < (off_t)INODE_LSIZE(inode))
    {
        int32_t chunk_idx = -1;
        if (inode.chunk_map.fully_packed)
        {
            /* Packed: chunk chứa cur_offset chỉ có thể là chunk bắt đầu đúng
             * tại WINDOW_BASE(cur_offset) — binary search thay vì linear. */
            uint64_t want = WINDOW_BASE(cur_offset);
            uint32_t lo = 0;
            uint32_t hi = inode.chunk_map.num_chunks;
            while (lo < hi)
            {
                uint32_t mid = lo + (hi - lo) / 2;
                if (inode.chunk_map.chunks[mid].logical_offset < want)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < inode.chunk_map.num_chunks &&
                inode.chunk_map.chunks[lo].logical_offset == want &&
                cur_offset < (off_t)want +
                             (off_t)inode.chunk_map.chunks[lo].stored_size)
                chunk_idx = (int32_t)lo;
        }
        else
        {
            /* File legacy chưa packed: linear scan chịu được chunk kích thước
             * bất kỳ — tầng fallback vĩnh viễn. */
            for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
            {
                myfs_chunk_t *c = &inode.chunk_map.chunks[i];
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
            off_t next_start = (off_t)INODE_LSIZE(inode);
            for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
            {
                off_t c_start = (off_t)inode.chunk_map.chunks[i].logical_offset;
                if (c_start > cur_offset)
                {
                    next_start = c_start;
                    break;
                }
            }
            size_t hole_remaining = (size_t)(INODE_LSIZE(inode) - (size_t)cur_offset);
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

        myfs_chunk_t *chunk = &inode.chunk_map.chunks[(uint32_t)chunk_idx];
        off_t chunk_logical_start = chunk->logical_offset;

        if (guard_chunk_logical_offset(cur_offset, chunk_logical_start) < 0)
            return cleanup_fd(fd, 1, -EIO);

        if (guard_chunk_metadata(chunk, chunk_idx) < 0)
            return cleanup_fd(fd, 1, -EIO);

        off_t chunk_offset = cur_offset - chunk_logical_start;
        size_t chunk_size = chunk->stored_size;

        if (guard_chunk_bounds((size_t)chunk_offset, chunk_size) < 0)
            return cleanup_fd(fd, 1, -EIO);

        size_t logical_remaining = (size_t)(INODE_LSIZE(inode) - (size_t)cur_offset);
        size_t bytes_in_chunk = min(remaining, min(chunk_size - chunk_offset, logical_remaining));

        /* pread + verify CRC32 + decompress-or-copy — dùng chung engine với
         * write/truncate/compact; chịu được frame dài hơn stored_size (meta
         * cũ bị truncate shrink trước khi có cơ chế rewrite). */
        char *payload = malloc(chunk->stored_size);
        if (guard_malloc(payload) < 0)
            return cleanup_fd(fd, 1, -ENOMEM);

        if (myfs_chunk_payload_load(fd, chunk, payload) != 0)
        {
            free(payload);
            return cleanup_fd(fd, 1, -EIO);
        }

        memcpy(buf + bytes_read, payload + chunk_offset, bytes_in_chunk);
        free(payload);

        bytes_read += bytes_in_chunk;
        cur_offset += bytes_in_chunk;
        remaining -= bytes_in_chunk;
    }

    close(fd);
    free(inode.chunk_map.chunks);

    LOG("[DEBUG] myfs_read success: read %zu bytes\n", bytes_read);
    return (int)bytes_read;
}

/*
 * Ghi dữ liệu vào file logic theo bất biến cửa sổ 64KB: vùng ghi được chia
 * theo các cửa sổ chứa nó, mỗi cửa sổ bị chạm được repack (merge dữ liệu cũ +
 * patch mới) thành đúng một chunk head-aligned. Append thuần, overwrite một
 * phần và write vào hole đều đi chung một đường — không còn nhánh RMW riêng.
 */
static int myfs_write_locked(const char *path, const char *buf, size_t size,
                             off_t offset, struct fuse_file_info *fi)
{
    LOG("[DEBUG] write: %s offset=%ld size=%zu\n", path, offset, size);

    if (size == 0)
        return 0;

    myfs_file_handle_t *handle = get_file_handle(fi);
    if (!handle)
        return -EIO;

    myfs_storage_t active_storage;
    int ret = resolve_storage(path, &active_storage);
    if (ret != 0)
        return ret;
    if (!storage_generation_equal(&handle->storage, &active_storage))
        return -ESTALE;

    /* Nạp metadata để biết cấu trúc chunk hiện tại trước khi ghi. */
    myfs_inode_t inode = {0};
    ret = load_chunk_map_from_path(active_storage.meta_path, &inode);
    if (ret != 0)
        return ret;

    off_t write_end = offset + (off_t)size;

    /*
     * Xác định dải chunk bị tiêu thụ và miền cửa sổ cần repack.
     * Miền khởi đầu là các cửa sổ 64KB phủ vùng ghi; chunk legacy có thể vắt
     * qua ranh giới cửa sổ nên miền mở rộng theo fixpoint cho tới khi không
     * kéo thêm chunk nào nữa (file đã packed: hội tụ ngay vòng đầu).
     */
    off_t region_lo = (off_t)WINDOW_BASE(offset);
    off_t region_hi = (off_t)WINDOW_CEIL(write_end);
    uint32_t first_idx = 0;
    uint32_t end_idx = 0;
    bool changed = true;
    uint32_t guard_iter = 0;
    while (changed)
    {
        if (guard_iter++ > inode.chunk_map.num_chunks + 1)
        {
            /* Miền đơn điệu tăng nên không thể lặp mãi — chặn phòng hờ. */
            free(inode.chunk_map.chunks);
            return -EIO;
        }
        changed = false;

        first_idx = 0;
        while (first_idx < inode.chunk_map.num_chunks)
        {
            myfs_chunk_t *c = &inode.chunk_map.chunks[first_idx];
            if ((off_t)c->logical_offset + (off_t)c->stored_size > region_lo)
                break;
            first_idx++;
        }

        off_t min_start = region_lo;
        off_t max_end = region_hi;
        end_idx = first_idx;
        while (end_idx < inode.chunk_map.num_chunks)
        {
            myfs_chunk_t *c = &inode.chunk_map.chunks[end_idx];
            if ((off_t)c->logical_offset >= region_hi)
                break;
            off_t c_end = (off_t)c->logical_offset + (off_t)c->stored_size;
            if ((off_t)c->logical_offset < min_start)
                min_start = (off_t)c->logical_offset;
            if (c_end > max_end)
                max_end = c_end;
            end_idx++;
        }

        off_t new_lo = (off_t)WINDOW_BASE(min_start);
        off_t new_hi = (off_t)WINDOW_CEIL(max_end);
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
    int fd = open(active_storage.data_path, O_RDWR | O_CLOEXEC);
    if (fd == -1)
    {
        free(inode.chunk_map.chunks);
        return -errno;
    }
    off_t eof = lseek(fd, 0, SEEK_END);
    if (eof < 0)
    {
        close(fd);
        free(inode.chunk_map.chunks);
        return -errno;
    }

    myfs_chunk_t *entries = NULL;
    uint32_t entry_count = 0;
    ret = myfs_repack_windows(fd, fd, &eof, &inode.chunk_map,
                              first_idx, consumed, region_lo, region_hi,
                              buf, offset, size, &entries, &entry_count);
    if (ret == 0 && fdatasync(fd) != 0)
        ret = -errno;
    close(fd);
    if (ret != 0)
    {
        free(entries);
        free(inode.chunk_map.chunks);
        return ret;
    }

    /* Splice mảng chunk: thay dải [first_idx, first_idx+consumed) bằng các
     * entry cửa sổ mới — giữ nguyên thứ tự sắp xếp theo logical_offset. */
    uint32_t old_count = inode.chunk_map.num_chunks;
    uint32_t new_count = old_count - consumed + entry_count;
    if (entry_count > consumed)
    {
        myfs_chunk_t *grown = realloc(inode.chunk_map.chunks,
                                      new_count * sizeof(myfs_chunk_t));
        if (!grown)
        {
            free(entries);
            free(inode.chunk_map.chunks);
            return -ENOMEM;
        }
        inode.chunk_map.chunks = grown;
    }
    memmove(&inode.chunk_map.chunks[first_idx + entry_count],
            &inode.chunk_map.chunks[first_idx + consumed],
            (old_count - first_idx - consumed) * sizeof(myfs_chunk_t));
    if (entry_count > 0)
        memcpy(&inode.chunk_map.chunks[first_idx], entries,
               entry_count * sizeof(myfs_chunk_t));
    inode.chunk_map.num_chunks = new_count;
    free(entries);

    if (write_end > (off_t)INODE_LSIZE(inode))
        INODE_LSIZE(inode) = write_end;

    /* Ghi metadata ra đĩa sau khi chunk map đã được cập nhật đầy đủ. */
    if (save_chunk_map_for_storage(&active_storage, &inode) != 0)
    {
        free(inode.chunk_map.chunks);
        return -EIO;
    }

    free(inode.chunk_map.chunks);

    LOG("[DEBUG] write OK: consumed=%u windows=%u chunks=%u logical_size=%zu\n",
        consumed, entry_count, new_count, (size_t)INODE_LSIZE(inode));

    return (int)size;
}

int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi)
{
    myfs_file_lock_t *lk = myfs_lock_file(path);
    if (!lk)
        return -ENOMEM;

    int ret = myfs_write_locked(path, buf, size, offset, fi);
    myfs_unlock_file(lk);
    return ret;
}

/* Giải phóng file descriptor gắn với file đang mở. */
int myfs_release(const char *path, struct fuse_file_info *fi)
{
    myfs_file_handle_t *handle = get_file_handle(fi);
    if (handle)
    {
        myfs_file_lock_t *lk = myfs_lock_file(path);
        if (!lk)
            return -ENOMEM;

        /* Removing the registry reference before retrying GC is the exact point
         * at which this generation can become deletion-eligible. */
        unregister_generation_handle_locked(handle);
        fsync(handle->data_fd);
        close(handle->data_fd);
        close(handle->meta_fd);
        fi->fh = 0;

        int gc_ret = run_generation_gc_locked(path);
        myfs_unlock_file(lk);
        free(handle);
        if (gc_ret != 0)
            LOG("[WARN] release: deferred GC returned %d\n", gc_ret);
    }

    /* Compaction chạy trên background worker — release chỉ enqueue path,
     * không còn trả tiền compact/GC đồng bộ trong FUSE op. Worker sẽ lấy
     * file lock của path khi xử lý. */
    int compact_ret = schedule_compaction(path);
    if (compact_ret != 0)
        LOG("[WARN] release: compact scheduling returned %d\n", compact_ret);

    return 0;
}
