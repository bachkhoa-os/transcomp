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
    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    int ret = myfs_create_locked(path, mode, fi);
    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0)
        return -unlock_ret;

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

    int fd = open(storage.data_path, fi->flags | O_CLOEXEC);
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
    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    int ret = myfs_open_locked(path, fi);
    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0)
        return -unlock_ret;

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
                c->stored_size = (uint32_t)(size - (off_t)c->logical_offset);
                /* raw_size vẫn giữ nguyên vì blob trên disk chưa được ghi lại. */
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
    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    int ret = myfs_truncate_locked(path, size, fi);
    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0)
        return -unlock_ret;

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
        int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
        if (lock_ret != 0)
            return -lock_ret;

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
        int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
        if (unlock_ret != 0 && ret == 0)
            ret = -unlock_ret;
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
        // Linear scan để tìm chunk chứa cur_offset — nhất quán với myfs_write().
        // Không dùng cur_offset/CHUNK_SIZE vì chunk thực tế không đảm bảo đúng 64KB.
        int32_t chunk_idx = -1;
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
        if (chunk_idx < 0)
            break; // cur_offset không nằm trong chunk nào → sparse region, trả zero

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

        char *raw_buf = malloc(chunk->raw_size);
        if (guard_malloc(raw_buf) < 0)
            return cleanup_fd(fd, 1, -ENOMEM);

        ssize_t n = pread(fd, raw_buf, chunk->raw_size, chunk->physical_offset);
        if (guard_pread_result(n, chunk->raw_size) < 0)
        {
            free(raw_buf);
            return cleanup_fd(fd, 1, -EIO);
        }

        /* Verify CRC32 nếu chunk có checksum hợp lệ (khác 0).
         * Checksum = 0 nghĩa là chunk được ghi trước khi có CRC32 → bỏ qua.
         * Phát hiện data corruption im lặng (bit rot, disk error...). */
        if (chunk->checksum != 0)
        {
            uint32_t actual = chunk_crc32(raw_buf, chunk->raw_size);
            if (actual != chunk->checksum)
            {
                LOG("[ERROR] CRC32 mismatch chunk %u: expected=0x%08x got=0x%08x\n",
                    chunk_idx, chunk->checksum, actual);
                free(raw_buf);
                return cleanup_fd(fd, 1, -EIO);
            }
        }

        char *decomp_buf = NULL;
        size_t decomp_size = 0;

        if (chunk->codec_type == 0)
        {
            decomp_buf = raw_buf;
            decomp_size = chunk->stored_size;
        }
        else if (chunk->codec_type == 1)
        {
            decomp_buf = malloc(chunk->stored_size);
            if (guard_malloc(decomp_buf) < 0)
            {
                free(raw_buf);
                return cleanup_fd(fd, 1, -ENOMEM);
            }

            if (zstd_decompress(raw_buf, chunk->raw_size,
                                decomp_buf, chunk->stored_size,
                                &decomp_size) != 0)
            {
                free(decomp_buf);
                free(raw_buf);
                return cleanup_fd(fd, 1, -EIO);
            }

            if (guard_decompress_size(decomp_size, chunk->stored_size) < 0)
            {
                free(decomp_buf);
                free(raw_buf);
                return cleanup_fd(fd, 1, -EIO);
            }

            free(raw_buf);
            raw_buf = NULL;
        }
        else
        {
            free(raw_buf);
            return cleanup_fd(fd, 1, -EIO);
        }

        memcpy(buf + bytes_read, decomp_buf + chunk_offset, bytes_in_chunk);

        bytes_read += bytes_in_chunk;
        cur_offset += bytes_in_chunk;
        remaining -= bytes_in_chunk;

        if (chunk->codec_type == 0)
            free(raw_buf);
        else
            free(decomp_buf);
    }

    close(fd);
    free(inode.chunk_map.chunks);

    LOG("[DEBUG] myfs_read success: read %zu bytes\n", bytes_read);
    return (int)bytes_read;
}

/*
 * Cơ chế read-modify-write cho trường hợp ghi đè một phần lên vùng dữ liệu đã tồn tại.
 * Thay vì ghi đè trực tiếp lên blob cũ, hàm gom các chunk bị chồng lấn vào một
 * vùng làm việc, áp patch dữ liệu mới, rồi ghi lại thành một blob mới ở cuối file.
 */
static int write_rmw(const myfs_storage_t *storage,
                     const char *buf, size_t size, off_t offset,
                     myfs_inode_t *inode, uint32_t chunk_idx)
{
    /* Xác định toàn bộ các chunk bị chồng lấn với vùng ghi mới. */
    LOG("[DEBUG] write_rmw: offset=%ld size=%zu chunk_idx=%u num_chunks=%u\n",
        offset, size, chunk_idx, inode->chunk_map.num_chunks);
    uint32_t last_idx = chunk_idx;
    off_t write_end = offset + (off_t)size;
    for (uint32_t i = chunk_idx + 1; i < inode->chunk_map.num_chunks; i++)
    {
        myfs_chunk_t *c = &inode->chunk_map.chunks[i];
        if ((off_t)c->logical_offset < write_end)
            last_idx = i;
        else
            break;
    }

    /* Tính miền logical cần gom lại, từ chunk đầu đến chunk cuối bị ảnh hưởng. */
    myfs_chunk_t *first = &inode->chunk_map.chunks[chunk_idx];
    myfs_chunk_t *last = &inode->chunk_map.chunks[last_idx];
    off_t region_start = (off_t)first->logical_offset;
    off_t region_end = (off_t)last->logical_offset + (off_t)last->stored_size;
    /* Nếu dữ liệu ghi vượt qua chunk cuối, mở rộng vùng cần xử lý tương ứng. */
    if (write_end > region_end)
        region_end = write_end;
    size_t region_size = (size_t)(region_end - region_start);

    /* Cấp phát buffer làm việc cho toàn bộ miền cần xử lý và khởi tạo bằng 0. */
    char *work_buf = calloc(region_size, 1);
    if (!work_buf)
        return -ENOMEM;

    /* Đọc và giải nén từng chunk vào đúng vị trí tương ứng trong work buffer. */
    int read_fd = open(storage->data_path, O_RDONLY | O_CLOEXEC);
    if (read_fd == -1)
    {
        free(work_buf);
        return -errno;
    }

    for (uint32_t i = chunk_idx; i <= last_idx; i++)
    {
        myfs_chunk_t *c = &inode->chunk_map.chunks[i];
        char *raw_buf = malloc(c->raw_size);
        if (!raw_buf)
        {
            close(read_fd);
            free(work_buf);
            return -ENOMEM;
        }

        ssize_t n = pread(read_fd, raw_buf, c->raw_size, c->physical_offset);
        
        if (c->checksum != 0)
        {
            uint32_t actual = chunk_crc32(raw_buf, c->raw_size);
            if (actual != c->checksum)
            {
                LOG("[ERROR] rmw: CRC32 mismatch chunk %u: expected=0x%08x got=0x%08x\n",
                    i, c->checksum, actual);
                free(raw_buf);
                close(read_fd);
                free(work_buf);
                return -EIO;
            }
        }
        LOG("[DEBUG] rmw pread: chunk=%u raw_size=%u phys_off=%lu got=%ld\n",
            i, c->raw_size, c->physical_offset, n);
        if (n != (ssize_t)c->raw_size)
        {
            LOG("[ERROR] rmw pread failed: expected %u got %ld errno=%d\n",
                c->raw_size, n, errno);
            free(raw_buf);
            close(read_fd);
            free(work_buf);
            return -EIO;
        }

        /* Vị trí ghi của chunk hiện tại bên trong work buffer. */
        size_t woff = (size_t)((off_t)c->logical_offset - region_start);

        if (c->codec_type == 1)
        {
            size_t decomp_size = 0;
            if (zstd_decompress(raw_buf, c->raw_size,
                                work_buf + woff, c->stored_size,
                                &decomp_size) != 0)
            {
                free(raw_buf);
                close(read_fd);
                free(work_buf);
                return -EIO;
            }
        }
        else
            memcpy(work_buf + woff, raw_buf, c->stored_size);

        free(raw_buf);
    }
    close(read_fd);

    /* Đắp dữ liệu mới vào đúng vùng người dùng yêu cầu. */
    size_t patch_off = (size_t)(offset - region_start);
    memcpy(work_buf + patch_off, buf, size);

    /* Nén lại toàn bộ miền sau khi đã ghép dữ liệu mới. */
    size_t compress_bound = ZSTD_compressBound(region_size);
    char *comp_buf = malloc(compress_bound);
    if (!comp_buf)
    {
        free(work_buf);
        return -ENOMEM;
    }

    size_t compressed_size = 0;
    int compress_ret = zstd_compress(work_buf, region_size,
                                     comp_buf, compress_bound,
                                     &compressed_size);
    const char *write_buf_ptr;
    size_t write_size;
    uint8_t new_codec;

    if (compress_ret == 0)
    {
        write_buf_ptr = comp_buf;
        write_size = compressed_size;
        new_codec = 1;
        LOG("[DEBUG] rmw: recompressed %zu → %zu bytes\n",
            region_size, compressed_size);
    }
    else
    {
        write_buf_ptr = work_buf;
        write_size = region_size;
        new_codec = 0;
        LOG("[DEBUG] rmw: storing raw %zu bytes\n", region_size);
    }

    /* Ghi blob mới vào cuối file .data để tránh ghi đè trực tiếp dữ liệu cũ. */
    int append_fd = open(storage->data_path, O_WRONLY | O_CLOEXEC);
    if (append_fd == -1)
    {
        free(comp_buf);
        free(work_buf);
        return -errno;
    }

    off_t new_phys = lseek(append_fd, 0, SEEK_END);
    if (new_phys < 0)
    {
        close(append_fd);
        free(comp_buf);
        free(work_buf);
        return -errno;
    }

    ssize_t written = pwrite(append_fd, write_buf_ptr, write_size, new_phys);
    /* Tính CRC32 TRƯỚC khi free — khi new_codec=1, write_buf_ptr == comp_buf */
    uint32_t blob_crc = chunk_crc32(write_buf_ptr, write_size);

    close(append_fd);
    free(comp_buf);
    free(work_buf);
    if (written != (ssize_t)write_size)
        return -EIO;

    /*
     * Thay thế toàn bộ dải chunk bị ảnh hưởng bằng một chunk mới duy nhất.
     * Chunk đầu tiên giữ vai trò đại diện cho miền đã được hợp nhất, các chunk
     * còn lại sẽ bị loại bỏ khỏi chunk map.
     */
    inode->chunk_map.chunks[chunk_idx].logical_offset = (uint64_t)region_start;
    inode->chunk_map.chunks[chunk_idx].physical_offset = new_phys;
    inode->chunk_map.chunks[chunk_idx].stored_size = (uint32_t)region_size;
    inode->chunk_map.chunks[chunk_idx].raw_size = (uint32_t)write_size;
    inode->chunk_map.chunks[chunk_idx].codec_type = new_codec;
    inode->chunk_map.chunks[chunk_idx].flags = (new_codec == 1) ? 1 : 0;
    inode->chunk_map.chunks[chunk_idx].checksum = blob_crc;

    /* Dịch các chunk phía sau lên để lấp khoảng trống do hợp nhất. */
    uint32_t removed = last_idx - chunk_idx; /* Số chunk bị gộp và loại bỏ. */
    if (removed > 0)
    {
        uint32_t total = inode->chunk_map.num_chunks;
        memmove(&inode->chunk_map.chunks[chunk_idx + 1],
                &inode->chunk_map.chunks[last_idx + 1],
                (total - last_idx - 1) * sizeof(myfs_chunk_t));
        inode->chunk_map.num_chunks -= removed;
    }

    /* Đồng bộ logical size nếu vùng ghi mới làm file dài hơn trước. */
    if (write_end > (off_t)INODE_LSIZE(*inode))
    {
        INODE_LSIZE(*inode) = write_end;
    }

    return save_chunk_map_for_storage(storage, inode);
}

/*
 * Ghi dữ liệu vào file logic, bao gồm cả trường hợp append mới lẫn ghi đè
 * một phần dữ liệu đã tồn tại. Hàm này quyết định nhánh xử lý dựa trên
 * metadata hiện hành và chỉ trả về số byte đã ghi khi thao tác thành công.
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

    int32_t chunk_idx = -1;
    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
    {
        myfs_chunk_t *c = &inode.chunk_map.chunks[i];
        if ((off_t)c->logical_offset <= offset &&
            offset < (off_t)c->logical_offset + (off_t)c->stored_size)
        {
            chunk_idx = (int32_t)i;
            break;
        }
    }

    if (chunk_idx >= 0)
    {
        /*
         * Nhánh RMW: dữ liệu ghi chồng lên một chunk đã tồn tại.
         * FUSE kỳ vọng số byte đã ghi khi thành công, vì vậy cần chuyển mã
         * kết quả từ write_rmw sang đúng giá trị trả về cho kernel.
         */
        int rmw_ret = write_rmw(&active_storage, buf, size, offset,
                                &inode, (uint32_t)chunk_idx);
        free(inode.chunk_map.chunks);
        return (rmw_ret == 0) ? (int)size : rmw_ret;
    }
    /* Thử nén chunk mới trước khi ghi để tiết kiệm dung lượng nếu có lợi. */
    size_t compress_bound = ZSTD_compressBound(size);
    char *comp_buf = malloc(compress_bound);
    if (!comp_buf)
    {
        free(inode.chunk_map.chunks);
        return -ENOMEM;
    }

    size_t compressed_size = 0;
    int compress_ret = zstd_compress(buf, size, comp_buf, compress_bound, &compressed_size);

    /*
     * Quyết định lưu dưới dạng nén hay raw:
     * - compress_ret == 0      : nén thành công và đủ hiệu quả.
     * - compress_ret == -EFBIG : dữ liệu khó nén, lưu raw cho hợp lý hơn.
     * - compress_ret == -EIO   : lỗi Zstd, vẫn fallback sang raw để tránh mất dữ liệu.
     */
    const char *write_buf;
    size_t write_size;
    uint8_t codec_type;

    if (compress_ret == 0)
    {
        write_buf = comp_buf;
        write_size = compressed_size;
        codec_type = 1;
        LOG("[DEBUG] write: compressed %zu → %zu bytes (%.1f%%)\n",
            size, compressed_size, 100.0 * compressed_size / size);
    }
    else
    {
        write_buf = buf;
        write_size = size;
        codec_type = 0;
        if (compress_ret == -EIO)
            LOG("[WARN] write: zstd error, fallback to raw\n");
        else
            LOG("[DEBUG] write: incompressible, storing raw\n");
    }

    /* Ghi chunk mới vào cuối file .data để tránh ghi đè trực tiếp lên dữ liệu cũ. */
    int append_fd = open(active_storage.data_path, O_WRONLY | O_CLOEXEC);
    if (append_fd == -1)
    {
        free(comp_buf);
        free(inode.chunk_map.chunks);
        return -errno;
    }

    off_t physical_offset = lseek(append_fd, 0, SEEK_END);
    if (physical_offset < 0)
    {
        close(append_fd);
        free(comp_buf);
        free(inode.chunk_map.chunks);
        return -errno;
    }

    /* Ghi dữ liệu mới vào vị trí vật lý vừa xác định. */
    ssize_t written = pwrite(append_fd, write_buf, write_size, physical_offset);
    /* Tính CRC32 TRƯỚC khi free(comp_buf) — khi codec=1, write_buf == comp_buf.
     * free() trước rồi dùng write_buf sẽ là use-after-free → segfault. */
    uint32_t blob_crc = chunk_crc32(write_buf, write_size);
    free(comp_buf);
    close(append_fd);
    if (written != (ssize_t)write_size)
    {
        free(inode.chunk_map.chunks);
        return -EIO;
    }

    /* Mở rộng mảng chunk metadata và ghi nhận chunk mới vừa được append. */
    uint32_t new_count = inode.chunk_map.num_chunks + 1;
    myfs_chunk_t *new_chunks = realloc(inode.chunk_map.chunks,
                                       new_count * sizeof(myfs_chunk_t));
    if (!new_chunks)
    {
        free(inode.chunk_map.chunks);
        return -ENOMEM;
    }
    inode.chunk_map.chunks = new_chunks;

    myfs_chunk_t *chunk = &inode.chunk_map.chunks[inode.chunk_map.num_chunks];
    chunk->logical_offset = offset;
    chunk->physical_offset = physical_offset;
    chunk->stored_size = size;    // kích thước logical (sau decompress)
    chunk->raw_size = write_size; // kích thước thực tế trên disk
    chunk->codec_type = codec_type;
    chunk->flags = (codec_type == 1) ? 1 : 0;
    chunk->checksum = blob_crc; /* Đã tính trước free(comp_buf) */

    inode.chunk_map.num_chunks = new_count;

    /* Đồng bộ logical size giữa inode và chunk_map sau khi append dữ liệu mới. */
    off_t end_pos = offset + (off_t)size;
    if (end_pos > (off_t)INODE_LSIZE(inode))
        INODE_LSIZE(inode) = end_pos;

    /* Ghi metadata ra đĩa sau khi chunk map đã được cập nhật đầy đủ. */
    if (save_chunk_map_for_storage(&active_storage, &inode) != 0)
    {
        free(inode.chunk_map.chunks);
        return -EIO;
    }

    free(inode.chunk_map.chunks);

    LOG("[DEBUG] write OK: chunks=%u logical_size=%zu codec=%d\n",
        new_count, (size_t)INODE_LSIZE(inode), codec_type);

    return (int)size;
}

int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi)
{
    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    int ret = myfs_write_locked(path, buf, size, offset, fi);
    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0)
        return -unlock_ret;

    return ret;
}

/* Giải phóng file descriptor gắn với file đang mở. */
int myfs_release(const char *path, struct fuse_file_info *fi)
{
    myfs_file_handle_t *handle = get_file_handle(fi);
    if (handle)
    {
        int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
        if (lock_ret != 0)
            return -lock_ret;

        /* Removing the registry reference before retrying GC is the exact point
         * at which this generation can become deletion-eligible. */
        unregister_generation_handle_locked(handle);
        fsync(handle->data_fd);
        close(handle->data_fd);
        close(handle->meta_fd);
        fi->fh = 0;

        int gc_ret = run_generation_gc_locked();
        int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
        free(handle);
        if (unlock_ret != 0)
            return -unlock_ret;
        if (gc_ret != 0)
            LOG("[WARN] release: deferred GC returned %d\n", gc_ret);
    }

    /* Compaction: thu hồi không gian của orphan blob sinh ra sau RMW.
     * Chỉ thực hiện khi wasted ratio >= COMPACT_THRESHOLD (25%).
     * Gọi sau khi handle writer đã rời registry. */
    int compact_ret = compact_data_file(path);
    if (compact_ret != 0)
        LOG("[WARN] release: compact returned %d\n", compact_ret);

    return 0;
}
