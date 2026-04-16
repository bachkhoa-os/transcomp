#include "myfs.h"

// Hàm lấy thuộc tính của file (getattr)
int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    (void)fi;
    printf("[DEBUG] getattr: %s\n", path);

    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    if (stat(data_path, stbuf) == -1)
        return -errno;

    // Sử dụng logical_size từ chunk map (quan trọng nhất)
    myfs_inode_t inode = {0};
    if (load_chunk_map(path, &inode) == 0)
    {
        stbuf->st_size = inode.logical_size;
    }

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
    // int ret = save_chunk_map(path, &inode);
    // if (ret < 0)
    //     return ret;

    // fi->fh = 0;
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
    (void)fi;   // hiện tại chưa dùng fi
    printf("[DEBUG] truncate: %s to %ld bytes\n", path, size);

    myfs_inode_t inode = {0};

    // Load chunk map hiện tại
    if (load_chunk_map(path, &inode) != 0) {
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
    (void)path; // tạm thời bỏ qua path vì chúng ta chưa dùng chunk map ở read

    // Hiện tại vẫn đọc raw từ .data (Sprint 3 chỉ làm persistence)
    int res = pread(fi->fh, buf, size, offset);
    if (res == -1)
        return -errno;
    return res;
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