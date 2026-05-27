#include "myfs.h"
#include <inttypes.h>

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
        inode->chunk_map.logical_size = 0;
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