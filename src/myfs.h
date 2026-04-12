#ifndef MYFS_H
#define MYFS_H

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <limits.h>

struct myfs_config
{
    char root[PATH_MAX];
};

extern struct myfs_config *conf;

void build_path(char *dest, const char *path);

void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg);
void myfs_destroy(void *private_data);

int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi,
                 enum fuse_readdir_flags flags);
int myfs_mknod(const char *path, mode_t mode, dev_t rdev);
int myfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int myfs_open(const char *path, struct fuse_file_info *fi);
int myfs_read(const char *path, char *buf, size_t size,
              off_t offset, struct fuse_file_info *fi);
int myfs_write(const char *path, const char *buf, size_t size,
               off_t offset, struct fuse_file_info *fi);
int myfs_release(const char *path, struct fuse_file_info *fi);
int myfs_mkdir(const char *path, mode_t mode);
int myfs_rmdir(const char *path);

#endif