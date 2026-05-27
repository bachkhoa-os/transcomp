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
#include <zstd.h>
#include <zlib.h>

/* Tính CRC32 của blob trên disk (write_buf, write_size).
 * Dùng để verify data integrity khi đọc lại. */
static inline uint32_t chunk_crc32(const void *buf, size_t size)
{
    return (uint32_t)crc32(0L, (const Bytef *)buf, (uInt)size);
}
#include <time.h>

/*
 * Macro ghi log debug kèm timestamp theo định dạng [HH:MM:SS.mmm].
 * Mục đích là giúp đối chiếu thứ tự sự kiện và thời điểm phát sinh lỗi
 * trong quá trình mount, đọc, ghi và đồng bộ metadata của filesystem.
 */
#define LOG(fmt, ...) do { \
    struct timespec _ts; \
    clock_gettime(CLOCK_REALTIME, &_ts); \
    struct tm *_tm = localtime(&_ts.tv_sec); \
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] " fmt, \
            _tm->tm_hour, _tm->tm_min, _tm->tm_sec, \
            _ts.tv_nsec / 1000000, ##__VA_ARGS__); \
} while(0)

#define CHUNK_SIZE (64 * 1024ULL) /* 64 KB */

typedef struct
{
    uint64_t logical_offset;  /* Vị trí của chunk trong không gian logic của file. */
    uint32_t raw_size;        /* Kích thước dữ liệu thực tế được lưu trên đĩa. */
    uint32_t stored_size;     /* Kích thước dữ liệu sau giải nén nếu chunk được nén. */
    uint8_t codec_type;       /* Mã codec: 0 = none, 1 = zstd, ... */
    uint8_t flags;            /* Các cờ trạng thái của chunk, ví dụ compressed, encrypted. */
    uint32_t checksum;        /* Giá trị kiểm tra toàn vẹn của dữ liệu chunk. */
    uint64_t physical_offset; /* Vị trí vật lý của chunk trong file lưu trữ. */
} myfs_chunk_t;               /* Metadata của một chunk trong file. */

typedef struct
{
    uint32_t num_chunks;   /* Tổng số chunk hiện có trong file. */
    uint64_t logical_size; /* Kích thước logic tổng cộng của file. */
    myfs_chunk_t *chunks;  /* Mảng metadata cho từng chunk. */
} myfs_chunk_map_t;        /* Cấu trúc chunk map của một file. */

typedef struct
{
    myfs_chunk_map_t chunk_map; /* Chunk map hiện hành của file. */
} myfs_inode_t;                 /* Lớp chứa metadata inode, có thể mở rộng về sau. */

/* Single source of truth cho logical size: luôn dùng macro này,
 * không truy cập chunk_map.logical_size trực tiếp từ bên ngoài helpers. */
#define INODE_LSIZE(inode) ((inode).chunk_map.logical_size)

struct myfs_config
{
    char root[PATH_MAX];
};

/* Dựng các đường dẫn vật lý từ path logic của FUSE. */
void build_path(char *dest, const char *path);
void build_data_path(char *dest, const char *path);
void build_meta_path(char *dest, const char *path);

/* Khởi tạo filesystem, thiết lập cấu hình runtime và bật các tuỳ chọn cần thiết. */
void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg);

/* Giải phóng tài nguyên đã cấp phát khi filesystem bị tháo mount. */
void myfs_destroy(void *private_data);

/* Các callback và helper chính của filesystem. */
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
int myfs_unlink(const char *path);

int load_chunk_map(const char *path, myfs_inode_t *inode);
int save_chunk_map(const char *path, myfs_inode_t *inode);
int compact_data_file(const char *path);
int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int myfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
int zstd_compress(const void *src, size_t src_size,
                  void *dst, size_t dst_capacity,
                  size_t *compressed_size);
int zstd_decompress(const void *src, size_t src_size,
                    void *dst, size_t dst_capacity,
                    size_t *decompressed_size);
ZSTD_DCtx *zstd_create_dctx(void);

#endif