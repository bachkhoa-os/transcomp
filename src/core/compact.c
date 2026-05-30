#include "myfs.h"
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

        /* Verify CRC32 trước khi copy — nếu blob nguồn đã corrupt thì từ chối
         * compact ngay lập tức thay vì âm thầm nhân bản dữ liệu hỏng. */
        if (chunk->checksum != 0)
        {
            uint32_t actual = chunk_crc32(buf, blob_size);
            if (actual != chunk->checksum)
            {
                LOG("[ERROR] compact: CRC32 mismatch chunk %u: expected=0x%08x got=0x%08x\n",
                    i, chunk->checksum, actual);
                free(buf);
                ret = -EIO;
                break;
            }
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