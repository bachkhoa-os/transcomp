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

#define CHUNK_SIZE (64 * 1024ULL) // 64 KB

typedef struct
{
    uint64_t logical_offset;  // offset in the logical file
    uint32_t raw_size;        // size of the chunk data on disk
    uint32_t stored_size;     // size of the chunk data after decompression (if compressed)
    uint8_t codec_type;       // 0 = none, 1 = zstd, etc.
    uint8_t flags;            // bit 0: compressed, bit 1: encrypted, etc.
    uint32_t checksum;        // checksum of the chunk data for integrity verification
    uint64_t physical_offset; // offset on disk where the chunk data is stored
} myfs_chunk_t;               // metadata for each chunk of a file

typedef struct
{
    uint32_t num_chunks;   // number of chunks in the file
    uint64_t logical_size; // total logical size of the file (sum of stored_size of all chunks)
    myfs_chunk_t *chunks;  // array of chunk metadata
} myfs_chunk_map_t;        // metadata for a file, including its chunk map

typedef struct
{
    uint64_t logical_size;      // sync with chunk_map.logical_size for convenience
    myfs_chunk_map_t chunk_map; // chunk map for this file
} myfs_inode_t;                 // placeholder for inode metadata (can be extended as needed)

struct myfs_config
{
    char root[PATH_MAX];
};

// Hàm lấy cấu hình từ context của FUSE
void build_path(char *dest, const char *path);
void build_data_path(char *dest, const char *path);
void build_meta_path(char *dest, const char *path);

// Khởi tạo filesystem, thiết lập cấu hình, v.v.
void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg);

// Dọn dẹp tài nguyên đã cấp phát (nếu có)
void myfs_destroy(void *private_data);

// Prototypes
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

int load_chunk_map(const char *path, myfs_inode_t *inode);
int save_chunk_map(const char *path, myfs_inode_t *inode);
int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int myfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);

#endif