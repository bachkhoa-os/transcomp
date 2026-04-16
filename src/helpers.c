#include "myfs.h"
#include <inttypes.h>

// Hàm lấy cấu hình từ context của FUSE
static inline struct myfs_config *get_conf()
{
    return (struct myfs_config *)fuse_get_context()->private_data;
}

// Khởi tạo filesystem, thiết lập cấu hình, v.v.
void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 1;
    printf("[DEBUG] FUSE init called\n");
    return fuse_get_context()->private_data;
}

// Dọn dẹp tài nguyên đã cấp phát (nếu có)
void myfs_destroy(void *private_data)
{
    free(private_data);
    printf("[DEBUG] FUSE destroy called\n");
}

// Dựng đường dẫn đầy đủ cho file lưu trữ thực tế dựa trên đường dẫn logic
void build_path(char *dest, const char *path)
{
    struct myfs_config *conf = get_conf();
    if (!conf || conf->root[0] == '\0')
    {
        fprintf(stderr, "[ERROR] config not initialized\n");
        return;
    }
    snprintf(dest, PATH_MAX, "%s%s", conf->root, path);
}

// Dựng đường dẫn cho file data (.data)
void build_data_path(char *dest, const char *path)
{
    build_path(dest, path);                    // đã có null terminator
    size_t len = strlen(dest);
    if (len + 6 >= PATH_MAX) {                 // 5 ký tự + null
        fprintf(stderr, "[ERROR] path too long for .data\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".data");
}

// Dựng đường dẫn cho file metadata (.meta)
void build_meta_path(char *dest, const char *path)
{
    build_path(dest, path);
    size_t len = strlen(dest);
    if (len + 6 >= PATH_MAX) {
        fprintf(stderr, "[ERROR] path too long for .meta\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".meta");
}

// Load chunk map từ file .meta (binary)
int load_chunk_map(const char *path, myfs_inode_t *inode)
{
    char meta_path[PATH_MAX];
    build_meta_path(meta_path, path);

    printf("[DEBUG] load_chunk_map: %s\n", meta_path);

    struct stat st;

    // Nếu file .meta không tồn tại, khởi tạo inode trống
    if (stat(meta_path, &st) == -1)
    {
        inode->logical_size = 0;
        inode->chunk_map.num_chunks = 0;
        inode->chunk_map.chunks = NULL;
        return 0;
    }

    FILE *f = fopen(meta_path, "rb");
    if (!f)
    {
        perror("[ERROR] fopen meta");
        return -errno;
    }

    // Đọc số lượng chunk và logical size
    if (fread(&inode->chunk_map.num_chunks, sizeof(uint32_t), 1, f) != 1 ||
        fread(&inode->chunk_map.logical_size, sizeof(uint64_t), 1, f) != 1)
    {
        fclose(f);
        return -EIO;
    }

    // Cấp phát và đọc mảng chunk
    free(inode->chunk_map.chunks);
    inode->chunk_map.chunks = NULL;

    // Nếu có chunk nào, cấp phát mảng và đọc vào
    if (inode->chunk_map.num_chunks > 0)
    {
        // Cấp phát mảng chunk
        inode->chunk_map.chunks = malloc(inode->chunk_map.num_chunks * sizeof(myfs_chunk_t));

        // Kiểm tra cấp phát thành công
        if (!inode->chunk_map.chunks)
        {
            fclose(f);
            return -ENOMEM;
        }
        // Đọc mảng chunk từ file .meta
        if (fread(inode->chunk_map.chunks, sizeof(myfs_chunk_t),
                  inode->chunk_map.num_chunks, f) != inode->chunk_map.num_chunks)
        {
            free(inode->chunk_map.chunks);
            fclose(f);
            return -EIO;
        }
    }
    else
    {
        // Nếu không có chunk nào, đảm bảo con trỏ chunks là NULL
        inode->chunk_map.chunks = NULL;
    }

    fclose(f);
    printf("[DEBUG] load_chunk_map success: %u chunks, logical_size=%lu\n",
           inode->chunk_map.num_chunks, inode->chunk_map.logical_size);
    return 0;
}

// Save chunk map xuống file .meta (binary)
int save_chunk_map(const char *path, myfs_inode_t *inode)
{
    char meta_path[PATH_MAX];
    char tmp_path[PATH_MAX];

    build_meta_path(meta_path, path);

    // Tạo đường dẫn tạm để ghi dữ liệu trước khi rename
    if (snprintf(tmp_path, PATH_MAX, "%s.tmp", meta_path) >= PATH_MAX)
    {
        fprintf(stderr, "[ERROR] tmp path too long\n");
        return -ENAMETOOLONG;
    }
    printf("[DEBUG] save_chunk_map: %s\n", meta_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
    {
        perror("[ERROR] fopen tmp meta");
        return -errno;
    }

    // Ghi số lượng chunk và logical size
    if (fwrite(&inode->chunk_map.num_chunks, sizeof(uint32_t), 1, f) != 1 ||
        fwrite(&inode->chunk_map.logical_size, sizeof(uint64_t), 1, f) != 1)
    {
        fclose(f);
        return -EIO;
    }

    // Ghi mảng chunk nếu có
    if (inode->chunk_map.num_chunks > 0)
    {
        if (fwrite(inode->chunk_map.chunks, sizeof(myfs_chunk_t),
                   inode->chunk_map.num_chunks, f) != inode->chunk_map.num_chunks)
        {
            fclose(f);
            return -EIO;
        }
    }

    // Đảm bảo dữ liệu được ghi ra đĩa trước khi rename
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // Rename file tạm thành file .meta chính thức
    if (rename(tmp_path, meta_path) != 0)
    {
        perror("[ERROR] rename");
        return -errno;
    }

    printf("[DEBUG] save_chunk_map success: %u chunks, logical_size=%" PRIu64 "\n",
           inode->chunk_map.num_chunks, inode->chunk_map.logical_size);

    return 0;
}