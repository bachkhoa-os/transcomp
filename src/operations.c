#include "myfs.h"

int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    (void)fi;
    printf("[DEBUG] getattr: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    if (stat(realpath, stbuf) == -1)
        return -errno;
    return 0;
}

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
        filler(buf, de->d_name, NULL, 0, 0);
    }
    closedir(dp);
    return 0;
}

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

int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    printf("[DEBUG] create: %s\n", path);
    return myfs_mknod(path, mode | S_IFREG, 0);
}

int myfs_open(const char *path, struct fuse_file_info *fi)
{
    printf("[DEBUG] open: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int fd = open(realpath, fi->flags);
    if (fd == -1)
        return -errno;
    fi->fh = fd;
    return 0;
}

int myfs_read(const char *path, char *buf, size_t size,
              off_t offset, struct fuse_file_info *fi)
{
    (void)path;
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