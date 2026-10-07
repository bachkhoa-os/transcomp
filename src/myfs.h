#ifndef MYFS_H
#define MYFS_H

/* Cần cho O_DIRECT (glibc); phải đứng trước mọi include hệ thống. */
#define _GNU_SOURCE 1

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <limits.h>
#include <stdbool.h>
#include <pthread.h>
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
    struct tm _tm_storage; \
    struct tm *_tm = localtime_r(&_ts.tv_sec, &_tm_storage); \
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] " fmt, \
            _tm->tm_hour, _tm->tm_min, _tm->tm_sec, \
            _ts.tv_nsec / 1000000, ##__VA_ARGS__); \
} while(0)

#define CHUNK_SIZE (64 * 1024ULL) /* legacy/default: 64 KiB */
#define MYFS_MIN_WINDOW_SIZE (16U * 1024U)
#define MYFS_DEFAULT_WINDOW_SIZE ((uint32_t)CHUNK_SIZE)
#define MYFS_MAX_WINDOW_SIZE (1024U * 1024U)

#define MYFS_META_VERSION_LEGACY 0U
#define MYFS_META_VERSION_V1 1U
#define MYFS_META_VERSION_V2 2U

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
    bool fully_packed;     /* Derive khi load, KHÔNG serialize xuống disk:
                            * true nếu mọi chunk thoả window_size của inode.
                            * Cho phép read dùng lookup theo cửa sổ; file
                            * legacy/không packed rơi về linear scan. */
} myfs_chunk_map_t;        /* Cấu trúc chunk map của một file. */

typedef struct
{
    myfs_chunk_map_t chunk_map; /* Chunk map hiện hành của file. */
    uint32_t window_size;       /* Fixed window size for this generation. */
    uint16_t metadata_version;  /* Decoded on-disk metadata version. */
    uint64_t metadata_sequence; /* Last committed base/delta sequence. */
    uint32_t metadata_delta_count;
    uint64_t metadata_journal_bytes;
    off_t metadata_valid_end;   /* End of last committed metadata record. */
} myfs_inode_t;                 /* Lớp chứa metadata inode, có thể mở rộng về sau. */

/* Single source of truth cho logical size: luôn dùng macro này,
 * không truy cập chunk_map.logical_size trực tiếp từ bên ngoài helpers. */
#define INODE_LSIZE(inode) ((inode).chunk_map.logical_size)

struct myfs_config
{
    char root[PATH_MAX];
};

#define MYFS_GENERATION_HEX_LEN 32

/* A resolved, immutable identity for one physical data/metadata pair.  Files
 * without a .current pointer use the legacy .data/.meta pair.  Compaction is
 * the only operation allowed to create a non-legacy storage generation. */
typedef struct
{
    char logical_path[PATH_MAX];
    char generation_id[MYFS_GENERATION_HEX_LEN + 1];
    char generation_dir[PATH_MAX];
    char marker_path[PATH_MAX];
    char data_path[PATH_MAX];
    char meta_path[PATH_MAX];
    dev_t data_dev;
    ino_t data_ino;
    bool is_legacy;
} myfs_storage_t;

struct myfs_generation_record;

typedef struct
{
    uint64_t full_windows;
    uint64_t partial_rmw;
} myfs_window_stats_t;

typedef struct
{
    bool valid;
    myfs_storage_t source_storage;
    myfs_window_stats_t stats_snapshot;
    uint64_t evaluation_id;
} myfs_adaptive_ticket_t;

/* fi->fh stores a pointer to this structure, rather than a bare descriptor.
 * Both descriptors pin the selected generation for the complete FUSE handle
 * lifetime; the registry record supplies the GC open-reference count. */
typedef struct myfs_file_handle
{
    int data_fd;
    int meta_fd;
    int flags;
    myfs_storage_t storage;
    pthread_rwlock_t cache_lock;
    myfs_inode_t cached_inode;
    uint64_t seen_metadata_epoch;
    bool cache_valid;
    struct myfs_generation_record *generation_record;
    struct myfs_file_handle *registry_prev;
    struct myfs_file_handle *registry_next;
} myfs_file_handle_t;

/* Per-file mutation serializes on the logical path lock.  Handle operations
 * may then take cache_lock and briefly a generation-registry shard; fast reads
 * take cache_lock -> registry shard without a path lock.  Never wait for a
 * path or cache lock while holding a registry shard.  compact_queue_mu is
 * released before either worker or fallback waits for a path lock. */
typedef struct myfs_file_lock myfs_file_lock_t;
myfs_file_lock_t *myfs_lock_file(const char *path);
void myfs_unlock_file(myfs_file_lock_t *lk);
void destroy_lock_table(void);

/* Cấu hình mount toàn cục (set trong main) — cho worker thread ngoài FUSE ctx. */
extern struct myfs_config *myfs_conf;

/* release() runs eligible generation GC synchronously, then schedules only
 * compaction.  The worker and stopped-worker fallback both compact under the
 * corresponding logical path lock. */
int start_compaction_worker(void);
void stop_compaction_worker(void);
int schedule_compaction(const char *path);
int schedule_adaptive_compaction(const char *path,
                                 const myfs_adaptive_ticket_t *ticket);
int schedule_release_compaction(const char *path,
                                const myfs_adaptive_ticket_t *ticket);
int myfs_choose_resize_target(uint32_t current_window, uint64_t live_bytes,
                              myfs_window_stats_t stats, bool in_cooldown,
                              uint32_t *target_window);
/* Caller holds the logical file lock; this function serializes the handle
 * cache with live generation handoff through cache_lock. */
int myfs_refresh_handle_cache_locked(myfs_file_handle_t *handle);

/* Dựng các đường dẫn vật lý từ path logic của FUSE. */
void build_path(char *dest, const char *path);
void build_data_path(char *dest, const char *path);
void build_meta_path(char *dest, const char *path);
void build_current_path(char *dest, const char *path);
int resolve_storage(const char *path, myfs_storage_t *storage);
int build_generation_storage(const char *path, const char *generation_id,
                             myfs_storage_t *storage);
int validate_generation_marker(const myfs_storage_t *storage);
int create_generation_storage(const char *path, mode_t data_mode,
                              myfs_storage_t *storage);
int publish_generation(const char *path, const myfs_storage_t *storage);
int fsync_parent_path(const char *path);
int install_generation_aliases(const char *path,
                               const myfs_storage_t *storage);
int remove_generation_storage(const myfs_storage_t *storage);
bool storage_generation_equal(const myfs_storage_t *a,
                              const myfs_storage_t *b);

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
int load_chunk_map_from_path(const char *meta_path, myfs_inode_t *inode);
int load_chunk_map_from_fd(int meta_fd, myfs_inode_t *inode);
int save_chunk_map_to_path(const char *meta_path, myfs_inode_t *inode);
int save_chunk_map_for_storage(const myfs_storage_t *storage,
                               myfs_inode_t *inode);
int append_chunk_map_delta_to_fd(int meta_fd, myfs_inode_t *inode,
                                 uint32_t first_idx, uint32_t removed_count,
                                 uint32_t added_count);

int register_generation_handle_locked(myfs_file_handle_t *handle);
void unregister_generation_handle_locked(myfs_file_handle_t *handle);
void generation_state_snapshot(myfs_file_handle_t *handle,
                               uint64_t *metadata_epoch, bool *superseded);
uint64_t generation_bump_metadata_epoch_locked(myfs_file_handle_t *handle);
void generation_bump_storage_epoch_locked(const myfs_storage_t *storage);
bool generation_observe_write_locked(myfs_file_handle_t *handle,
                                     uint64_t full_windows,
                                     uint64_t partial_rmw,
                                     myfs_adaptive_ticket_t *ticket);
bool generation_claim_release_locked(myfs_file_handle_t *handle,
                                     myfs_adaptive_ticket_t *ticket);
void generation_adaptive_schedule_failed(
    const myfs_adaptive_ticket_t *ticket);
unsigned generation_writer_refs_locked(const myfs_storage_t *storage);
unsigned generation_open_refs_locked(const myfs_storage_t *storage);
int mark_generation_for_gc_locked(const myfs_storage_t *storage,
                                  bool install_aliases);
/* GC các generation pending của một path (caller giữ file lock của path đó);
 * path == NULL = quét tất cả (chỉ dùng lúc destroy, single-thread). */
int run_generation_gc_locked(const char *path);
int recover_generations_for_path_locked(const char *path,
                                        const myfs_storage_t *active);
void destroy_generation_registry(void);
int compact_data_file(const char *path);
int myfs_truncate(const char *path, off_t size, struct fuse_file_info *fi);
int myfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
int zstd_compress(const void *src, size_t src_size,
                  void *dst, size_t dst_capacity,
                  size_t *compressed_size);
int zstd_decompress(const void *src, size_t src_size,
                    void *dst, size_t dst_capacity,
                    size_t *decompressed_size);
int zstd_decompress_prefix(const void *src, size_t src_size,
                           void *dst, size_t want_size);

/* Engine chunk I/O dùng chung cho write path, truncate, read và compaction. */
bool myfs_window_size_valid(uint32_t window_size);
uint64_t myfs_window_base(uint64_t offset, uint32_t window_size);
int myfs_window_ceil(uint64_t offset, uint32_t window_size, uint64_t *result);
bool chunk_map_is_packed(const myfs_chunk_map_t *map, uint32_t window_size);
int myfs_chunk_payload_load(int fd, const myfs_chunk_t *chunk, char *dst);
int myfs_blob_append(int fd, off_t *eof, const char *payload, size_t len,
                     uint64_t logical_offset, myfs_chunk_t *out);
int myfs_repack_windows(int src_fd, int dst_fd, off_t *eof,
                        const myfs_chunk_map_t *map,
                        uint32_t window_size,
                        uint32_t first_idx, uint32_t consumed,
                        off_t region_lo, off_t region_hi,
                        const char *patch, off_t patch_off, size_t patch_len,
                        myfs_chunk_t **out_entries, uint32_t *out_count);
#endif
