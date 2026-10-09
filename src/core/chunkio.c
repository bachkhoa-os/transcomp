#include "myfs.h"
#include "chunkio_scratch.h"

/*
 * chunkio.c — engine I/O mức chunk dùng chung cho write path, truncate,
 * read và compaction. Hợp nhất ba đoạn logic trước đây bị lặp ở file.c và
 * compact.c: nạp+verify+giải nén payload, nén+append blob, và repack nội
 * dung vào các cửa sổ theo kích thước của generation (bất biến packing).
 */

static int pread_all(int fd, void *buf, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pread(fd, (char *)buf + done, size - done,
                          offset + (off_t)done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        done += (size_t)n;
    }
    return 0;
}

static int pwrite_all(int fd, const void *buf, size_t size, off_t offset)
{
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pwrite(fd, (const char *)buf + done, size - done,
                           offset + (off_t)done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;
        done += (size_t)n;
    }
    return 0;
}

/*
 * Kiểm tra bất biến packing: mọi chunk bắt đầu đúng tại ranh giới window_size
 * của generation, không rỗng, không vượt quá cửa sổ, và offset tăng nghiêm ngặt
 * (⇒ không chồng lấn, không vắt qua cửa sổ). Derive tại thời điểm load thay
 * vì persist một flag — flag có thể stale sau crash, còn derive tự lành.
 */
bool myfs_window_size_valid(uint32_t window_size)
{
    return window_size >= MYFS_MIN_WINDOW_SIZE &&
           window_size <= MYFS_MAX_WINDOW_SIZE &&
           (window_size & (window_size - 1U)) == 0;
}

uint64_t myfs_window_base(uint64_t offset, uint32_t window_size)
{
    return offset & ~((uint64_t)window_size - 1U);
}

int myfs_window_ceil(uint64_t offset, uint32_t window_size, uint64_t *result)
{
    if (!result || !myfs_window_size_valid(window_size))
        return -EINVAL;
    uint64_t mask = (uint64_t)window_size - 1U;
    if (offset > UINT64_MAX - mask)
        return -EOVERFLOW;
    *result = (offset + mask) & ~mask;
    return 0;
}

bool chunk_map_is_packed(const myfs_chunk_map_t *map, uint32_t window_size)
{
    if (!myfs_window_size_valid(window_size))
        return false;
    for (uint32_t i = 0; i < map->num_chunks; i++)
    {
        const myfs_chunk_t *c = &map->chunks[i];
        if (c->logical_offset % window_size != 0 ||
            c->stored_size == 0 || c->stored_size > window_size)
            return false;
        if (i > 0 && c->logical_offset <= map->chunks[i - 1].logical_offset)
            return false;
    }
    return true;
}

/*
 * Nạp payload (đã giải nén) của một chunk vào dst[stored_size]:
 * pread blob + verify CRC32 + decompress-or-copy.
 */
int myfs_chunk_payload_load(int fd, const myfs_chunk_t *chunk, char *dst)
{
    myfs_chunkio_scratch_lease_t raw_lease = {0};
    int ret = myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_RAW,
                                           chunk->raw_size, &raw_lease);
    if (ret != 0)
        return ret;
    char *raw_buf = raw_lease.data;

    ret = pread_all(fd, raw_buf, chunk->raw_size,
                    (off_t)chunk->physical_offset);
    if (ret == 0 && chunk->checksum != 0 &&
        chunk_crc32(raw_buf, chunk->raw_size) != chunk->checksum)
    {
        LOG("[ERROR] payload_load: CRC32 mismatch phys=%llu raw=%u\n",
            (unsigned long long)chunk->physical_offset, chunk->raw_size);
        ret = -EIO;
    }
    if (ret == 0)
    {
        if (chunk->codec_type == 0)
        {
            /* Blob raw: stored_size byte đầu là phần hợp lệ (truncate có thể
             * đã shrink stored_size mà giữ nguyên blob). */
            if (chunk->raw_size < chunk->stored_size)
                ret = -EIO;
            else
                memcpy(dst, raw_buf, chunk->stored_size);
        }
        else if (chunk->codec_type == 1)
        {
            ret = zstd_decompress_prefix(raw_buf, chunk->raw_size,
                                         dst, chunk->stored_size);
        }
        else
        {
            LOG("[ERROR] payload_load: unsupported codec=%d\n",
                chunk->codec_type);
            ret = -EIO;
        }
    }
    myfs_chunkio_scratch_release(&raw_lease);
    return ret;
}

/*
 * Nén payload (raw-fallback theo ngưỡng 12.5% + magic byte như write path)
 * và append thành blob mới tại *eof; điền entry metadata đầy đủ và tiến eof.
 * KHÔNG fdatasync — caller sync một lần cho cả batch trước khi publish meta.
 */
int myfs_blob_append(int fd, off_t *eof, const char *payload, size_t len,
                     uint64_t logical_offset, myfs_chunk_t *out)
{
    size_t bound = ZSTD_compressBound(len);
    myfs_chunkio_scratch_lease_t comp_lease = {0};
    int ret = myfs_chunkio_scratch_acquire(MYFS_CHUNKIO_SCRATCH_COMP,
                                           bound, &comp_lease);
    if (ret != 0)
        return ret;
    char *comp_buf = comp_lease.data;

    const char *blob = payload;
    size_t blob_size = len;
    uint8_t codec = 0;
    size_t comp_size = 0;
    if (zstd_compress(payload, len, comp_buf, bound, &comp_size) == 0)
    {
        blob = comp_buf;
        blob_size = comp_size;
        codec = 1;
    }

    ret = pwrite_all(fd, blob, blob_size, *eof);
    if (ret == 0)
    {
        out->logical_offset = logical_offset;
        out->physical_offset = (uint64_t)*eof;
        out->raw_size = (uint32_t)blob_size;
        out->stored_size = (uint32_t)len;
        out->codec_type = codec;
        out->flags = (codec == 1) ? 1 : 0;
        out->checksum = chunk_crc32(blob, blob_size);
        *eof += (off_t)blob_size;
    }
    myfs_chunkio_scratch_release(&comp_lease);
    return ret;
}

/*
 * Repack: ghép nội dung của các chunk [first_idx, first_idx+consumed) và
 * patch (nếu có) vào các cửa sổ kích thước window_size trong
 * [region_lo, region_hi), ghi mỗi cửa sổ không rỗng thành một blob mới qua
 * dst_fd (head-aligned: chunk mới
 * luôn bắt đầu tại ranh giới cửa sổ, hole đầu cửa sổ materialize thành zero).
 * Cửa sổ hoàn toàn rỗng không tạo chunk — sparse được giữ nguyên.
 * Trả mảng entry mới qua out_entries/out_count (caller free + splice + sync).
 */
int myfs_repack_windows(int src_fd, int dst_fd, off_t *eof,
                        const myfs_chunk_map_t *map,
                        uint32_t window_size,
                        uint32_t first_idx, uint32_t consumed,
                        off_t region_lo, off_t region_hi,
                        const char *patch, off_t patch_off, size_t patch_len,
                        myfs_chunk_t **out_entries, uint32_t *out_count)
{
    *out_entries = NULL;
    *out_count = 0;
    if (region_hi <= region_lo)
        return 0;
    if (!myfs_window_size_valid(window_size) || region_lo < 0 || region_hi < 0)
        return -EINVAL;

    uint64_t ceil_hi;
    int ret = myfs_window_ceil((uint64_t)region_hi, window_size, &ceil_hi);
    if (ret != 0)
        return ret;
    uint64_t base_lo = myfs_window_base((uint64_t)region_lo, window_size);
    uint64_t max_windows64 = (ceil_hi - base_lo) / window_size;
    if (max_windows64 > UINT32_MAX ||
        max_windows64 > SIZE_MAX / sizeof(myfs_chunk_t))
        return -EFBIG;
    uint32_t max_windows = (uint32_t)max_windows64;
    myfs_chunk_t *entries = malloc((size_t)max_windows * sizeof(*entries));
    if (!entries)
    {
        return -ENOMEM;
    }

    ret = 0;
    uint32_t count = 0;
    myfs_chunkio_scratch_lease_t window_lease = {0};
    char *win_buf = NULL;
    /* Cache payload của chunk vắt qua nhiều cửa sổ đích (layout legacy hoặc
     * adaptive shrink) — giải nén 1 lần. */
    int64_t cached_idx = -1;
    myfs_chunkio_scratch_lease_t cached_lease = {0};
    char *cached_payload = NULL;

    off_t win = (off_t)myfs_window_base((uint64_t)region_lo, window_size);
    while (win < region_hi && ret == 0)
    {
        off_t win_end = win + (off_t)window_size;

        /* Nguồn dữ liệu kế tiếp từ vị trí win — cho phép nhảy qua các cửa sổ
         * trống của file sparse thay vì memset window_size byte vô ích cho
         * từng cửa sổ. */
        off_t next_data = region_hi;
        if (patch && patch_len > 0 && patch_off + (off_t)patch_len > win)
        {
            off_t p_lo = patch_off > win ? patch_off : win;
            if (p_lo < next_data)
                next_data = p_lo;
        }
        for (uint32_t i = first_idx; i < first_idx + consumed; i++)
        {
            const myfs_chunk_t *c = &map->chunks[i];
            off_t c_end = (off_t)c->logical_offset + (off_t)c->stored_size;
            if (c_end <= win)
                continue;
            off_t c_lo = (off_t)c->logical_offset > win
                             ? (off_t)c->logical_offset : win;
            if (c_lo < next_data)
                next_data = c_lo;
            break; /* mảng sorted → chunk đầu tiên còn dữ liệu là nhỏ nhất */
        }
        if (next_data >= win_end)
        {
            if (next_data >= region_hi)
                break;
            win = (off_t)myfs_window_base((uint64_t)next_data, window_size);
            continue;
        }

        /* Fast path: patch phủ trọn cửa sổ → không cần merge, nén thẳng từ
         * buffer người dùng (trường hợp ghi tuần tự khối lớn). */
        if (patch && patch_off <= win &&
            patch_off + (off_t)patch_len >= win_end)
        {
            ret = myfs_blob_append(dst_fd, eof, patch + (win - patch_off),
                                   (size_t)window_size, (uint64_t)win,
                                   &entries[count]);
            if (ret == 0)
                count++;
            win += (off_t)window_size;
            continue;
        }

        if (!window_lease.data)
        {
            ret = myfs_chunkio_scratch_acquire(
                MYFS_CHUNKIO_SCRATCH_WINDOW, window_size, &window_lease);
            if (ret != 0)
                break;
            win_buf = window_lease.data;
        }
        memset(win_buf, 0, window_size);
        size_t win_used = 0;

        /* Ghép phần giao của các source chunk vào cửa sổ. */
        for (uint32_t i = first_idx; i < first_idx + consumed && ret == 0; i++)
        {
            const myfs_chunk_t *c = &map->chunks[i];
            off_t c_start = (off_t)c->logical_offset;
            off_t c_end = c_start + (off_t)c->stored_size;
            if (c_end <= win)
                continue;
            if (c_start >= win_end)
                break;

            off_t lo = c_start > win ? c_start : win;
            off_t hi = c_end < win_end ? c_end : win_end;

            if (c_start >= win && c_end <= win_end)
            {
                /* Chunk nằm gọn trong cửa sổ: nạp thẳng vào đúng chỗ. */
                ret = myfs_chunk_payload_load(src_fd, c,
                                              win_buf + (c_start - win));
            }
            else
            {
                if (cached_idx != (int64_t)i)
                {
                    myfs_chunkio_scratch_release(&cached_lease);
                    cached_payload = NULL;
                    cached_idx = -1;
                    ret = myfs_chunkio_scratch_acquire(
                        MYFS_CHUNKIO_SCRATCH_CACHED, c->stored_size,
                        &cached_lease);
                    if (ret != 0)
                        break;
                    cached_payload = cached_lease.data;
                    ret = myfs_chunk_payload_load(src_fd, c, cached_payload);
                    if (ret != 0)
                        break;
                    cached_idx = (int64_t)i;
                }
                memcpy(win_buf + (lo - win), cached_payload + (lo - c_start),
                       (size_t)(hi - lo));
            }
            if (ret == 0 && (size_t)(hi - win) > win_used)
                win_used = (size_t)(hi - win);
        }
        if (ret != 0)
            break;

        /* Đắp patch lên trên dữ liệu cũ. */
        if (patch && patch_len > 0)
        {
            off_t p_end = patch_off + (off_t)patch_len;
            if (p_end > win && patch_off < win_end)
            {
                off_t lo = patch_off > win ? patch_off : win;
                off_t hi = p_end < win_end ? p_end : win_end;
                memcpy(win_buf + (lo - win), patch + (lo - patch_off),
                       (size_t)(hi - lo));
                if ((size_t)(hi - win) > win_used)
                    win_used = (size_t)(hi - win);
            }
        }

        if (win_used > 0)
        {
            ret = myfs_blob_append(dst_fd, eof, win_buf, win_used,
                                   (uint64_t)win, &entries[count]);
            if (ret == 0)
                count++;
        }
        win += (off_t)window_size;
    }

    myfs_chunkio_scratch_release(&cached_lease);
    myfs_chunkio_scratch_release(&window_lease);
    if (ret != 0)
    {
        free(entries);
        return ret;
    }
    *out_entries = entries;
    *out_count = count;
    return 0;
}
