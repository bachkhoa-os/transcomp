#include "myfs.h"
#include "guards.h"
#define min(a, b) ((a) < (b) ? (a) : (b))

// Hàm lấy thuộc tính của file (getattr)
int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    (void)fi;
    printf("[DEBUG] getattr: %s\n", path);

    // Thử build_path trước (cho thư mục)
    char real_path[PATH_MAX];
    build_path(real_path, path);

    if (stat(real_path, stbuf) == 0 && S_ISDIR(stbuf->st_mode))
        return 0; // là thư mục → trả về luôn

    // Nếu không phải thư mục, thử với .data (cho file)
    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    if (stat(data_path, stbuf) == -1)
        return -errno;

    // Cập nhật logical_size từ chunk map
    myfs_inode_t inode = {0};
    if (load_chunk_map(path, &inode) == 0)
        stbuf->st_size = inode.logical_size;

    return 0;
}

// Hàm đọc thư mục (readdir)
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi,
                 enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;
    printf("[DEBUG] readdir: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);

    DIR *dp = opendir(realpath);
    if (!dp)
        return -errno;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    struct dirent *de;
    while ((de = readdir(dp)) != NULL)
    {
        size_t len = strlen(de->d_name);
        if (len > 5 && strcmp(de->d_name + len - 5, ".meta") == 0)
            continue;
        if (len > 5 && strcmp(de->d_name + len - 5, ".data") == 0)
            continue;

        filler(buf, de->d_name, NULL, 0, 0);
    }
    closedir(dp);
    return 0;
}

// Hàm tạo file (mknod)
int myfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    printf("[DEBUG] mknod: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int res = mknod(realpath, mode, rdev);
    if (res == -1)
        return -errno;
    return 0;
}

// Hàm tạo file (create) - wrapper cho mknod với S_IFREG
int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    printf("[DEBUG] create: %s\n", path);

    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    // Tạo .data file
    int fd = open(data_path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd == -1)
        return -errno;
    close(fd);

    // Tạo .meta file rỗng
    myfs_inode_t inode = {0};
    save_chunk_map(path, &inode);
    return 0;
}

int myfs_open(const char *path, struct fuse_file_info *fi)
{
    printf("[DEBUG] open: %s\n", path);

    // Mở file .data thay vì file gốc
    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    int fd = open(data_path, fi->flags);
    if (fd == -1)
    {
        perror("[ERROR] open .data");
        return -errno;
    }
    fi->fh = fd;

    // Load chunk map (lazy load) để chuẩn bị cho read/write sau này
    myfs_inode_t inode = {0};
    // if (load_chunk_map(path, &inode) != 0) {
    //     close(fd);
    //     return -EIO;
    // }

    load_chunk_map(path, &inode);
    return 0;
}

// Hàm cập nhật thời gian truy cập/ sửa đổi (utimens)
int myfs_utimens(const char *path, const struct timespec tv[2],
                 struct fuse_file_info *fi)
{
    (void)path;
    (void)tv;
    (void)fi;
    printf("[DEBUG] utimens: %s (stub - ignored)\n", path);
    return 0;
}

// Hàm truncate để thay đổi kích thước logical của file
int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi; // hiện tại chưa dùng fi
    printf("[DEBUG] truncate: %s to %ld bytes\n", path, size);

    myfs_inode_t inode = {0};

    // Load chunk map hiện tại
    if (load_chunk_map(path, &inode) != 0)
    {
        return -EIO;
    }

    // Cập nhật logical size
    inode.logical_size = size;

    // TODO Sprint 5: xử lý resize chunk map thực sự (cut hoặc extend chunk)
    // Hiện tại chỉ lưu size logic
    return save_chunk_map(path, &inode);
}

int myfs_read(const char *path, char *buf, size_t size,
              off_t offset, struct fuse_file_info *fi)
{
    (void)fi;
    printf("[DEBUG] myfs_read: %s offset=%ld size=%zu\n", path, offset, size);

    // Load chunk map để biết cấu trúc file
    myfs_inode_t inode = {0};

    if (load_chunk_map(path, &inode) != 0)
        return -EIO;

    if (offset >= (off_t)inode.logical_size)
        return 0;

    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    // Nếu đã mở file bằng fi, dùng fd đó, nếu không thì mở file .data ở đây
    int use_fi_fd = (fi != NULL && fi->fh != 0);
    int fd = fi ? (int)fi->fh : open(data_path, O_RDONLY);
    if (fd == -1)
    {
        perror("[ERROR] open .data for read");
        return -errno;
    }

    size_t bytes_read = 0;
    off_t cur_offset = offset;
    size_t remaining = size;

    while (remaining > 0 && cur_offset < (off_t)inode.logical_size)
    {
        uint32_t chunk_idx = cur_offset / CHUNK_SIZE;
        if (chunk_idx >= inode.chunk_map.num_chunks)
            break;

        myfs_chunk_t *chunk = &inode.chunk_map.chunks[chunk_idx];
        off_t chunk_logical_start = chunk->logical_offset;

        if (guard_chunk_logical_offset(cur_offset, chunk_logical_start) < 0)
            return cleanup_fd(fd, use_fi_fd, -EIO);

        if (guard_chunk_metadata(chunk, chunk_idx) < 0)
            return cleanup_fd(fd, use_fi_fd, -EIO);

        off_t chunk_offset = cur_offset - chunk_logical_start;
        size_t chunk_size = chunk->stored_size;

        if (guard_chunk_bounds((size_t)chunk_offset, chunk_size) < 0)
            return cleanup_fd(fd, use_fi_fd, -EIO);

        size_t logical_remaining = (size_t)(inode.logical_size - (size_t)cur_offset);
        size_t bytes_in_chunk = min(remaining,
                                    min(chunk_size - chunk_offset,
                                        logical_remaining));

        char *raw_buf = malloc(chunk->raw_size);
        if (guard_malloc(raw_buf) < 0)
            return cleanup_fd(fd, use_fi_fd, -ENOMEM);

        ssize_t n = pread(fd, raw_buf, chunk->raw_size, chunk->physical_offset);
        if (guard_pread_result(n, chunk->raw_size) < 0)
        {
            free(raw_buf);
            return cleanup_fd(fd, use_fi_fd, -EIO);
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
                return cleanup_fd(fd, use_fi_fd, -ENOMEM);
            }

            if (zstd_decompress(raw_buf, chunk->raw_size,
                                decomp_buf, chunk->stored_size,
                                &decomp_size) != 0)
            {
                free(decomp_buf);
                free(raw_buf);
                return cleanup_fd(fd, use_fi_fd, -EIO);
            }

            if (guard_decompress_size(decomp_size, chunk->stored_size) < 0)
            {
                free(decomp_buf);
                free(raw_buf);
                return cleanup_fd(fd, use_fi_fd, -EIO);
            }

            free(raw_buf);
            raw_buf = NULL;
        }
        else
        {
            free(raw_buf);
            return cleanup_fd(fd, use_fi_fd, -EIO);
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

    if (!use_fi_fd)
        close(fd);

    printf("[DEBUG] myfs_read success: read %zu bytes\n", bytes_read);
    return (int)bytes_read;
}

int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi)
{
    (void)path;

    int res = pwrite(fi->fh, buf, size, offset);
    if (res == -1)
        return -errno;
    return res;
}

int myfs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    close(fi->fh);
    return 0;
}

// Hàm tạo thư mục (mkdir)
int myfs_mkdir(const char *path, mode_t mode)
{
    printf("[DEBUG] mkdir: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int res = mkdir(realpath, mode);
    if (res == -1)
        return -errno;
    return 0;
}

// Hàm xóa thư mục (rmdir)
int myfs_rmdir(const char *path)
{
    printf("[DEBUG] rmdir: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int res = rmdir(realpath);
    if (res == -1)
        return -errno;
    return 0;
}