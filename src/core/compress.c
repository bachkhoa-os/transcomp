#include "myfs.h"
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
 * Giải nén một blob mà frame có thể chứa NHIỀU hơn want_size byte hợp lệ.
 * Xảy ra với meta cũ bị truncate shrink stored_size trước khi có cơ chế
 * rewrite chunk: frame trên disk vẫn giữ nội dung đầy đủ, chỉ want_size byte
 * đầu còn hợp lệ. Ghi đúng want_size byte đầu của frame vào dst.
 */
int zstd_decompress_prefix(const void *src, size_t src_size,
                           void *dst, size_t want_size)
{
    unsigned long long content = ZSTD_getFrameContentSize(src, src_size);
    if (content == ZSTD_CONTENTSIZE_ERROR || content == ZSTD_CONTENTSIZE_UNKNOWN)
    {
        LOG("[ERROR] zstd_decompress_prefix: cannot determine frame size\n");
        return -EIO;
    }
    if (content < (unsigned long long)want_size)
    {
        LOG("[ERROR] zstd_decompress_prefix: frame=%llu < want=%zu\n",
            content, want_size);
        return -EIO;
    }

    size_t decomp_size = 0;
    if (content == (unsigned long long)want_size)
    {
        int ret = zstd_decompress(src, src_size, dst, want_size, &decomp_size);
        if (ret != 0)
            return ret;
        return (decomp_size == want_size) ? 0 : -EIO;
    }

    /* Frame dài hơn phần hợp lệ: giải nén ra scratch rồi copy prefix. */
    char *scratch = malloc((size_t)content);
    if (!scratch)
        return -ENOMEM;
    int ret = zstd_decompress(src, src_size, scratch, (size_t)content, &decomp_size);
    if (ret == 0 && decomp_size != (size_t)content)
        ret = -EIO;
    if (ret == 0)
        memcpy(dst, scratch, want_size);
    free(scratch);
    return ret;
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