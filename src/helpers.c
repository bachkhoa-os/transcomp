#include "myfs.h"
#include <inttypes.h>

/*
 * Kiểm tra nhanh xem dữ liệu có thuộc định dạng đã biết là incompressible không.
 * Dựa trên magic bytes ở đầu buffer — không cần scan toàn bộ dữ liệu.
 * Trả về 1 nếu nên skip compression, 0 nếu nên thử nén bình thường.
 */
static int is_incompressible(const void *src, size_t src_size)
{
    if (!src || src_size < 4)
        return 0;

    const uint8_t *b = (const uint8_t *)src;

    // JPEG: FF D8 FF
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return 1;

    // PNG: 89 50 4E 47
    if (b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47)
        return 1;

    // ZIP / JAR / DOCX / XLSX (PK header): 50 4B 03 04
    if (b[0] == 0x50 && b[1] == 0x4B && b[2] == 0x03 && b[3] == 0x04)
        return 1;

    // GIF: 47 49 46 38
    if (b[0] == 0x47 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x38)
        return 1;

    // MP4 / MOV: ftyp box tại offset 4 (bytes 4-7 = 66 74 79 70)
    if (src_size >= 8 &&
        b[4] == 0x66 && b[5] == 0x74 && b[6] == 0x79 && b[7] == 0x70)
        return 1;

    // Zstd frame (dữ liệu đã nén bằng Zstd): FD 2F B5 28
    if (b[0] == 0xFD && b[1] == 0x2F && b[2] == 0xB5 && b[3] == 0x28)
        return 1;

    // gzip: 1F 8B
    if (b[0] == 0x1F && b[1] == 0x8B)
        return 1;

    return 0;
}

/*
 * Truy xuất cấu hình của hệ thống tập tin từ private_data của FUSE context.
 * Hàm này đóng vai trò là điểm truy cập chung để các helper khác lấy cấu hình
 * hiện hành mà không cần truyền thủ công qua từng lời gọi.
 */
static inline struct myfs_config *get_conf()
{
    return (struct myfs_config *)fuse_get_context()->private_data;
}

/*
 * Khởi tạo filesystem và thiết lập các tuỳ chọn runtime cho FUSE.
 * Hiện tại kernel cache được bật để giảm số lần truy cập không cần thiết
 * tới lớp user-space trong các thao tác đọc lặp lại.
 */
void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 1;
    LOG("[DEBUG] FUSE init called\n");
    return fuse_get_context()->private_data;
}

/*
 * Giải phóng tài nguyên mà filesystem đã cấp phát trong suốt vòng đời hoạt động.
 * Mọi con trỏ được gắn vào private_data cần được thu hồi tại đây để tránh rò rỉ
 * bộ nhớ khi mountpoint bị tháo gỡ.
 */
void myfs_destroy(void *private_data)
{
    free(private_data);
    LOG("[DEBUG] FUSE destroy called\n");
}

/*
 * Dựng đường dẫn vật lý đầy đủ trên backing storage từ đường dẫn logic của FUSE.
 * Hàm này ghép thư mục gốc cấu hình với path do FUSE cung cấp để các thao tác
 * phía sau luôn làm việc trên đúng đối tượng lưu trữ.
 */
void build_path(char *dest, const char *path)
{
    struct myfs_config *conf = get_conf();
    if (!conf || conf->root[0] == '\0')
    {
        LOG("[ERROR] config not initialized\n");
        return;
    }
    snprintf(dest, PATH_MAX, "%s%s", conf->root, path);
}

/*
 * Tạo đường dẫn tới file dữ liệu thực tế tương ứng với một path logic.
 * Tệp .data lưu nội dung người dùng, tách biệt khỏi metadata để việc quản lý
 * dữ liệu và trạng thái nén được rõ ràng hơn.
 */
void build_data_path(char *dest, const char *path)
{
    /* build_path đã đảm bảo dest là chuỗi kết thúc bằng null. */
    build_path(dest, path);
    size_t len = strlen(dest);
    if (len + 6 >= PATH_MAX)
    {
        /* 5 ký tự của ".data" cộng với ký tự null kết thúc chuỗi. */
        LOG("[ERROR] path too long for .data\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".data");
}

/*
 * Tạo đường dẫn tới file metadata tương ứng với một path logic.
 * File .meta lưu chunk map và logical size của đối tượng, tách riêng khỏi
 * payload để cập nhật metadata không ảnh hưởng trực tiếp đến dữ liệu chính.
 */
void build_meta_path(char *dest, const char *path)
{
    build_path(dest, path);
    size_t len = strlen(dest);
    if (len + 6 >= PATH_MAX)
    {
        LOG("[ERROR] path too long for .meta\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".meta");
}

/*
 * Nạp chunk map từ file metadata dạng nhị phân.
 * Trạng thái được lưu gồm số lượng chunk, logical size và danh sách chunk.
 * Nếu metadata chưa tồn tại, inode được khởi tạo ở trạng thái rỗng hợp lệ.
 */
int load_chunk_map(const char *path, myfs_inode_t *inode)
{
    char meta_path[PATH_MAX];
    build_meta_path(meta_path, path);

    LOG("[DEBUG] load_chunk_map: %s\n", meta_path);

    struct stat st;

    /* Nếu file .meta chưa tồn tại, xem đây là inode mới và khởi tạo rỗng. */
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

    /* Đọc phần header của metadata: số chunk và kích thước logic. */
    if (fread(&inode->chunk_map.num_chunks, sizeof(uint32_t), 1, f) != 1 ||
        fread(&inode->chunk_map.logical_size, sizeof(uint64_t), 1, f) != 1)
    {
        fclose(f);
        return -EIO;
    }

    /* Giải phóng dữ liệu cũ trước khi nạp trạng thái mới từ đĩa. */
    free(inode->chunk_map.chunks);
    inode->chunk_map.chunks = NULL;

    /* Nếu metadata có danh sách chunk, cấp phát đúng kích thước và đọc vào RAM. */
    if (inode->chunk_map.num_chunks > 0)
    {
        inode->chunk_map.chunks = malloc(inode->chunk_map.num_chunks * sizeof(myfs_chunk_t));

        /* Bảo đảm cấp phát bộ nhớ thành công trước khi đọc dữ liệu. */
        if (!inode->chunk_map.chunks)
        {
            fclose(f);
            return -ENOMEM;
        }
        /* Đọc toàn bộ mảng chunk từ file metadata. */
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
        /* Không có chunk nào thì giữ con trỏ ở trạng thái NULL rõ ràng. */
        inode->chunk_map.chunks = NULL;
    }

    fclose(f);

    /*
     * Đồng bộ logical size từ chunk_map sang inode.
     * Cấu trúc chunk_map là nguồn dữ liệu vừa được nạp từ đĩa, còn inode.logical_size
     * là trường được các hàm như myfs_read và myfs_getattr sử dụng trực tiếp.
     * Giữ hai giá trị này nhất quán là cần thiết để tránh đọc sai kích thước file.
     */
    inode->logical_size = inode->chunk_map.logical_size;

    LOG("[DEBUG] load_chunk_map success: %u chunks, logical_size=%lu\n",
        inode->chunk_map.num_chunks, inode->chunk_map.logical_size);
    return 0;
}

/*
 * Ghi chunk map xuống file metadata nhị phân theo cách an toàn hơn.
 * Dữ liệu được ghi vào file tạm trước, sau đó mới rename sang file chính thức
 * để hạn chế nguy cơ metadata bị hỏng nếu quá trình ghi bị gián đoạn.
 */
int save_chunk_map(const char *path, myfs_inode_t *inode)
{
    char meta_path[PATH_MAX];
    char tmp_path[PATH_MAX];

    build_meta_path(meta_path, path);

    /* Dựng đường dẫn file tạm để thực hiện ghi theo kiểu atomic-ish. */
    if (snprintf(tmp_path, PATH_MAX, "%s.tmp", meta_path) >= PATH_MAX)
    {
        LOG("[ERROR] tmp path too long\n");
        return -ENAMETOOLONG;
    }
    LOG("[DEBUG] save_chunk_map: %s\n", meta_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
    {
        perror("[ERROR] fopen tmp meta");
        return -errno;
    }

    /* Ghi header metadata gồm số chunk và logical size hiện tại. */
    if (fwrite(&inode->chunk_map.num_chunks, sizeof(uint32_t), 1, f) != 1 ||
        fwrite(&inode->chunk_map.logical_size, sizeof(uint64_t), 1, f) != 1)
    {
        fclose(f);
        return -EIO;
    }

    /* Nếu tồn tại chunk map, ghi toàn bộ danh sách chunk theo sau header. */
    if (inode->chunk_map.num_chunks > 0)
    {
        if (fwrite(inode->chunk_map.chunks, sizeof(myfs_chunk_t),
                   inode->chunk_map.num_chunks, f) != inode->chunk_map.num_chunks)
        {
            fclose(f);
            return -EIO;
        }
    }

    /* Buộc flush dữ liệu xuống thiết bị trước khi đổi tên file tạm. */
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    /* Hoàn tất bằng cách thay thế file metadata cũ bằng file tạm đã ghi xong. */
    if (rename(tmp_path, meta_path) != 0)
    {
        perror("[ERROR] rename");
        return -errno;
    }

    LOG("[DEBUG] save_chunk_map success: %u chunks, logical_size=%" PRIu64 "\n",
        inode->chunk_map.num_chunks, inode->chunk_map.logical_size);

    return 0;
}

/*
 * Compaction: loại bỏ các orphan blob trên .data sinh ra sau mỗi lần RMW.
 *
 * Mỗi lần write_rmw() ghi blob mới vào cuối .data và cập nhật physical_offset
 * trong chunk map, blob cũ bị bỏ lại (orphan) — không còn được tham chiếu nhưng
 * vẫn chiếm dung lượng. Hàm này thực hiện rewrite .data chỉ giữ lại các blob
 * còn sống, sau đó cập nhật physical_offset trong chunk map.
 *
 * Quy trình:
 *   1. Load chunk map để biết tập hợp blob còn sống.
 *   2. Tính wasted_ratio = (data_file_size - sum(raw_size)) / data_file_size.
 *      Nếu < COMPACT_THRESHOLD thì skip — không đáng compaction.
 *   3. Mở .data gốc để đọc, tạo .data.compact để ghi.
 *   4. Với mỗi chunk (theo thứ tự logical_offset), đọc blob từ offset cũ,
 *      ghi vào offset mới trong file compact, cập nhật physical_offset.
 *   5. fsync + rename .data.compact → .data (atomic).
 *   6. save_chunk_map() với physical_offset đã cập nhật.
 *
 * Trả về 0 nếu thành công hoặc skip, âm errno nếu lỗi nghiêm trọng.
 */
#define COMPACT_THRESHOLD 0.25  /* Compaction khi wasted >= 25% disk size */

int compact_data_file(const char *path)
{
    myfs_inode_t inode = {0};
    if (load_chunk_map(path, &inode) != 0)
        return -EIO;

    /* Không có chunk nào thì không cần compact. */
    if (inode.chunk_map.num_chunks == 0)
    {
        free(inode.chunk_map.chunks);
        return 0;
    }

    char data_path[PATH_MAX];
    build_data_path(data_path, path);

    struct stat st;
    if (stat(data_path, &st) != 0)
    {
        free(inode.chunk_map.chunks);
        return -errno;
    }
    off_t data_file_size = st.st_size;

    /* Tính tổng raw_size của các blob còn sống. */
    uint64_t live_bytes = 0;
    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
        live_bytes += inode.chunk_map.chunks[i].raw_size;

    /* Tính wasted ratio và skip nếu chưa đáng compact. */
    double wasted = (data_file_size > 0)
        ? (double)(data_file_size - (off_t)live_bytes) / (double)data_file_size
        : 0.0;

    /* Skip nếu wasted ratio thấp hoặc absolute wasted < 64KB — không đáng compact. */
    uint64_t wasted_bytes = (data_file_size > (off_t)live_bytes)
        ? (uint64_t)(data_file_size - (off_t)live_bytes) : 0;

    if (wasted < COMPACT_THRESHOLD || wasted_bytes < 65536)
    {
        LOG("[DEBUG] compact: skip (wasted=%.1f%% wasted_bytes=%llu)\n",
            wasted * 100, (unsigned long long)wasted_bytes);
        free(inode.chunk_map.chunks);
        return 0;
    }

    LOG("[DEBUG] compact: start — data_size=%lld live=%llu wasted=%.1f%%\n",
        (long long)data_file_size, (unsigned long long)live_bytes, wasted * 100);

    /* Mở file .data gốc để đọc các blob còn sống. */
    int src_fd = open(data_path, O_RDONLY);
    if (src_fd == -1)
    {
        free(inode.chunk_map.chunks);
        return -errno;
    }

    /* Tạo file compact tạm để ghi dữ liệu mới. */
    char compact_path[PATH_MAX];
    if (snprintf(compact_path, PATH_MAX, "%s.compact", data_path) >= PATH_MAX)
    {
        close(src_fd);
        free(inode.chunk_map.chunks);
        return -ENAMETOOLONG;
    }

    int dst_fd = open(compact_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd == -1)
    {
        close(src_fd);
        free(inode.chunk_map.chunks);
        return -errno;
    }

    /* Copy từng blob theo thứ tự logical_offset, cập nhật physical_offset. */
    off_t new_phys = 0;
    int ret = 0;

    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
    {
        myfs_chunk_t *chunk = &inode.chunk_map.chunks[i];
        uint32_t blob_size = chunk->raw_size;

        char *buf = malloc(blob_size);
        if (!buf) { ret = -ENOMEM; break; }

        ssize_t n = pread(src_fd, buf, blob_size, chunk->physical_offset);
        if (n != (ssize_t)blob_size)
        {
            LOG("[ERROR] compact: pread chunk %u failed\n", i);
            free(buf);
            ret = -EIO;
            break;
        }

        ssize_t w = pwrite(dst_fd, buf, blob_size, new_phys);
        free(buf);
        if (w != (ssize_t)blob_size)
        {
            LOG("[ERROR] compact: pwrite chunk %u failed\n", i);
            ret = -EIO;
            break;
        }

        /* Cập nhật physical_offset sang vị trí mới trong file compact. */
        chunk->physical_offset = new_phys;
        new_phys += blob_size;
    }

    close(src_fd);

    if (ret != 0)
    {
        close(dst_fd);
        unlink(compact_path);
        free(inode.chunk_map.chunks);
        return ret;
    }

    /* fsync để đảm bảo dữ liệu mới thực sự trên disk trước khi rename. */
    fsync(dst_fd);
    close(dst_fd);

    /* Atomic rename: .data.compact → .data */
    if (rename(compact_path, data_path) != 0)
    {
        unlink(compact_path);
        free(inode.chunk_map.chunks);
        return -errno;
    }

    /* Lưu chunk map với physical_offset đã được cập nhật. */
    ret = save_chunk_map(path, &inode);
    free(inode.chunk_map.chunks);

    LOG("[DEBUG] compact: done — new_data_size=%lld (was %lld, saved %lld bytes)\n",
        (long long)new_phys, (long long)data_file_size,
        (long long)(data_file_size - new_phys));

    return ret;
}

/*
 * Compress a chunk using Zstd one-shot API at default compression level.
 * Returns 0 on success (saved >= 12.5%), -EFBIG if not worth compressing,
 * -EIO on Zstd internal error.
 */
int zstd_compress(const void *src, size_t src_size,
                  void *dst, size_t dst_capacity,
                  size_t *compressed_size)
{
    // Fast path: skip hoàn toàn nếu detect được magic bytes incompressible
    // Tránh lãng phí CPU gọi ZSTD_compress() trên JPEG, ZIP, MP4...
    if (is_incompressible(src, src_size))
    {
        LOG("[DEBUG] zstd_compress: magic byte skip\n");
        return -EFBIG;
    }

    size_t const result = ZSTD_compress(dst, dst_capacity, src, src_size,
                                        ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(result))
    {
        LOG("[ERROR] ZSTD_compress failed: %s\n",
            ZSTD_getErrorName(result));
        return -EIO;
    }

    /*
     * Bỏ qua nén nếu mức giảm không đạt tối thiểu 12.5%.
     * Ngưỡng này giúp tránh tốn CPU cho dữ liệu vốn đã nén tốt hoặc có tính entropy cao.
     */
    if (result >= src_size - src_size / 8)
    {
        LOG("[DEBUG] zstd_compress: skip (compressed=%zu original=%zu)\n",
            result, src_size);
        return -EFBIG;
    }

    *compressed_size = result;
    return 0;
}

/*
 * Giải nén một chunk đã được mã hoá bằng Zstd.
 * Kết quả giải nén được ghi vào dst và kích thước thực tế được trả qua decompressed_size.
 */
int zstd_decompress(const void *src, size_t src_size,
                    void *dst, size_t dst_capacity,
                    size_t *decompressed_size)
{
    size_t const result = ZSTD_decompress(dst, dst_capacity, src, src_size);
    if (ZSTD_isError(result))
    {
        LOG("[ERROR] ZSTD_decompress failed: %s\n",
            ZSTD_getErrorName(result));
        return -EIO;
    }
    *decompressed_size = result;
    return 0;
}

/*
 * Tạo một ZSTD_DCtx có thể tái sử dụng cho các lần giải nén sau.
 * Việc giữ context ở dạng reusable có thể giảm chi phí khởi tạo nếu luồng xử lý
 * thực hiện nhiều thao tác giải nén liên tiếp.
 */
ZSTD_DCtx *zstd_create_dctx(void)
{
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    if (!dctx)
    {
        LOG("[ERROR] ZSTD_createDCtx failed\n");
        return NULL;
    }
    return dctx;
}