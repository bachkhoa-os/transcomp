#include "myfs.h"

#include <inttypes.h>

pthread_mutex_t myfs_metadata_mutex = PTHREAD_MUTEX_INITIALIZER;

static int pread_full(int fd, void *buf, size_t size, off_t offset)
{
    char *cursor = buf;
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = pread(fd, cursor + done, size - done, offset + (off_t)done);
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

static int write_full(int fd, const void *buf, size_t size)
{
    const char *cursor = buf;
    size_t done = 0;
    while (done < size)
    {
        ssize_t n = write(fd, cursor + done, size - done);
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

/* Bất biến: mảng chunk luôn sắp theo logical_offset tăng dần. Meta cũ có thể
 * chưa sắp (sparse write ở offset giảm dần append vào cuối mảng); sắp lại tại
 * điểm nạp duy nhất này để mọi đường ghi/đọc dựa được vào bất biến.
 * Insertion sort ổn định — O(n) khi mảng đã sắp, là trường hợp thường gặp. */
static void sort_chunks_by_logical_offset(myfs_chunk_t *chunks, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
    {
        myfs_chunk_t key = chunks[i];
        uint32_t j = i;
        while (j > 0 && chunks[j - 1].logical_offset > key.logical_offset)
        {
            chunks[j] = chunks[j - 1];
            j--;
        }
        chunks[j] = key;
    }
}

int load_chunk_map_from_fd(int meta_fd, myfs_inode_t *inode)
{
    uint32_t num_chunks = 0;
    uint64_t logical_size = 0;
    off_t cursor = 0;

    int ret = pread_full(meta_fd, &num_chunks, sizeof(num_chunks), cursor);
    if (ret != 0)
        return ret;
    cursor += sizeof(num_chunks);
    ret = pread_full(meta_fd, &logical_size, sizeof(logical_size), cursor);
    if (ret != 0)
        return ret;
    cursor += sizeof(logical_size);

    if ((size_t)num_chunks > SIZE_MAX / sizeof(myfs_chunk_t))
        return -EIO;

    size_t chunks_size = (size_t)num_chunks * sizeof(myfs_chunk_t);
    myfs_chunk_t *chunks = NULL;
    if (chunks_size > 0)
    {
        chunks = malloc(chunks_size);
        if (!chunks)
            return -ENOMEM;
        ret = pread_full(meta_fd, chunks, chunks_size, cursor);
        if (ret != 0)
        {
            free(chunks);
            return ret;
        }
    }

    sort_chunks_by_logical_offset(chunks, num_chunks);

    free(inode->chunk_map.chunks);
    inode->chunk_map.chunks = chunks;
    inode->chunk_map.num_chunks = num_chunks;
    inode->chunk_map.logical_size = logical_size;
    /* Derive tại load — không persist: flag stale sau crash tự lành, và file
     * cũ tình cờ thoả bất biến cũng được hưởng fast path. */
    inode->chunk_map.fully_packed = chunk_map_is_packed(&inode->chunk_map);
    return 0;
}

int load_chunk_map_from_path(const char *meta_path, myfs_inode_t *inode)
{
    LOG("[DEBUG] load_chunk_map: %s\n", meta_path);
    int fd = open(meta_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    int ret = load_chunk_map_from_fd(fd, inode);
    if (close(fd) != 0 && ret == 0)
        ret = -errno;
    if (ret == 0)
    {
        LOG("[DEBUG] load_chunk_map success: %u chunks, logical_size=%" PRIu64 "\n",
            inode->chunk_map.num_chunks, inode->chunk_map.logical_size);
    }
    return ret;
}

int load_chunk_map(const char *path, myfs_inode_t *inode)
{
    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
        return ret;

    ret = load_chunk_map_from_path(storage.meta_path, inode);
    if (ret == -ENOENT && storage.is_legacy)
    {
        free(inode->chunk_map.chunks);
        inode->chunk_map.chunks = NULL;
        inode->chunk_map.num_chunks = 0;
        inode->chunk_map.logical_size = 0;
        inode->chunk_map.fully_packed = true;
        return 0;
    }
    return ret;
}

int save_chunk_map_to_path(const char *meta_path, myfs_inode_t *inode)
{
    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, PATH_MAX, "%s.tmp", meta_path) >= PATH_MAX)
        return -ENAMETOOLONG;

    LOG("[DEBUG] save_chunk_map: %s\n", meta_path);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -errno;

    int ret = write_full(fd, &inode->chunk_map.num_chunks,
                         sizeof(inode->chunk_map.num_chunks));
    if (ret == 0)
        ret = write_full(fd, &inode->chunk_map.logical_size,
                         sizeof(inode->chunk_map.logical_size));
    if (ret == 0 && inode->chunk_map.num_chunks > 0)
    {
        ret = write_full(fd, inode->chunk_map.chunks,
                         (size_t)inode->chunk_map.num_chunks * sizeof(myfs_chunk_t));
    }
    if (ret == 0 && fsync(fd) != 0)
        ret = -errno;
    if (close(fd) != 0 && ret == 0)
        ret = -errno;

    if (ret != 0)
    {
        unlink(tmp_path);
        return ret;
    }
    if (rename(tmp_path, meta_path) != 0)
    {
        ret = -errno;
        unlink(tmp_path);
        return ret;
    }
    ret = fsync_parent_path(meta_path);
    if (ret != 0)
        return ret;

    LOG("[DEBUG] save_chunk_map success: %u chunks, logical_size=%" PRIu64 "\n",
        inode->chunk_map.num_chunks, inode->chunk_map.logical_size);
    return 0;
}

static bool data_alias_is_active(const myfs_storage_t *storage)
{
    char alias_path[PATH_MAX];
    build_data_path(alias_path, storage->logical_path);
    struct stat source_st;
    struct stat alias_st;
    return stat(storage->data_path, &source_st) == 0 &&
           stat(alias_path, &alias_st) == 0 &&
           source_st.st_dev == alias_st.st_dev &&
           source_st.st_ino == alias_st.st_ino;
}

int save_chunk_map_for_storage(const myfs_storage_t *storage,
                               myfs_inode_t *inode)
{
    int ret = save_chunk_map_to_path(storage->meta_path, inode);
    if (ret != 0)
        return ret;

    /* Metadata replacement changes its inode.  Refresh compatibility hard links
     * only after migration has already made the data alias authoritative for
     * this generated pair; a pinned legacy generation keeps both old names. */
    if (!storage->is_legacy && data_alias_is_active(storage))
        return install_generation_aliases(storage->logical_path, storage);
    return 0;
}

int save_chunk_map(const char *path, myfs_inode_t *inode)
{
    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
        return ret;
    return save_chunk_map_for_storage(&storage, inode);
}
