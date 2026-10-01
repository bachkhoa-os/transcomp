#include "myfs.h"

#include <inttypes.h>

#define META_HEADER_SIZE 64U
#define META_ENTRY_SIZE 32U
#define DELTA_HEADER_SIZE 48U
#define DELTA_TRAILER_SIZE 8U
#define DELTA_COMMIT_MARKER 0x54494d43U

static const unsigned char meta_magic[8] =
    {'M', 'Y', 'F', 'S', 'M', 'E', 'T', 'A'};
static const unsigned char delta_magic[8] =
    {'M', 'Y', 'F', 'S', 'D', 'L', 'T', 'A'};

static uint16_t get_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const unsigned char *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static void put_le16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_le32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put_le64(unsigned char *p, uint64_t value)
{
    put_le32(p, (uint32_t)value);
    put_le32(p + 4, (uint32_t)(value >> 32));
}

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

static void encode_entry(unsigned char out[META_ENTRY_SIZE],
                         const myfs_chunk_t *chunk)
{
    memset(out, 0, META_ENTRY_SIZE);
    put_le64(out, chunk->logical_offset);
    put_le64(out + 8, chunk->physical_offset);
    put_le32(out + 16, chunk->raw_size);
    put_le32(out + 20, chunk->stored_size);
    put_le32(out + 24, chunk->checksum);
    out[28] = chunk->codec_type;
    out[29] = chunk->flags;
}

static void decode_entry(const unsigned char in[META_ENTRY_SIZE],
                         myfs_chunk_t *chunk)
{
    memset(chunk, 0, sizeof(*chunk));
    chunk->logical_offset = get_le64(in);
    chunk->physical_offset = get_le64(in + 8);
    chunk->raw_size = get_le32(in + 16);
    chunk->stored_size = get_le32(in + 20);
    chunk->checksum = get_le32(in + 24);
    chunk->codec_type = in[28];
    chunk->flags = in[29];
}

static int load_legacy_map(int fd, off_t file_size, myfs_inode_t *inode)
{
    uint32_t count;
    uint64_t logical_size;
    if (file_size < (off_t)(sizeof(count) + sizeof(logical_size)))
        return -EIO;
    int ret = pread_full(fd, &count, sizeof(count), 0);
    if (ret == 0)
        ret = pread_full(fd, &logical_size, sizeof(logical_size), sizeof(count));
    if (ret != 0)
        return ret;
    uint64_t bytes64 = (uint64_t)count * sizeof(myfs_chunk_t);
    if (bytes64 > SIZE_MAX)
        return -EIO;
    size_t bytes = (size_t)bytes64;
    if ((uint64_t)file_size < sizeof(count) + sizeof(logical_size) + bytes)
        return -EIO;

    myfs_chunk_t *chunks = bytes ? malloc(bytes) : NULL;
    if (bytes && !chunks)
        return -ENOMEM;
    if (bytes)
    {
        ret = pread_full(fd, chunks, bytes,
                         (off_t)(sizeof(count) + sizeof(logical_size)));
        if (ret != 0)
        {
            free(chunks);
            return ret;
        }
    }

    sort_chunks_by_logical_offset(chunks, count);
    free(inode->chunk_map.chunks);
    memset(inode, 0, sizeof(*inode));
    inode->chunk_map.chunks = chunks;
    inode->chunk_map.num_chunks = count;
    inode->chunk_map.logical_size = logical_size;
    inode->window_size = MYFS_DEFAULT_WINDOW_SIZE;
    inode->metadata_version = MYFS_META_VERSION_LEGACY;
    inode->metadata_valid_end = file_size;
    inode->chunk_map.fully_packed =
        chunk_map_is_packed(&inode->chunk_map, inode->window_size);
    return 0;
}

static int splice_delta(myfs_chunk_t **chunks, uint32_t *count,
                        uint32_t first, uint32_t removed,
                        const unsigned char *wire_entries, uint32_t added)
{
    if (first > *count || removed > *count - first)
        return -EIO;
    uint64_t new_count64 = (uint64_t)*count - removed + added;
    if (new_count64 > UINT32_MAX ||
        new_count64 > SIZE_MAX / sizeof(myfs_chunk_t))
        return -EIO;
    uint32_t new_count = (uint32_t)new_count64;
    myfs_chunk_t *replacement = new_count
        ? malloc((size_t)new_count * sizeof(*replacement)) : NULL;
    if (new_count && !replacement)
        return -ENOMEM;

    if (first)
        memcpy(replacement, *chunks, (size_t)first * sizeof(*replacement));
    for (uint32_t i = 0; i < added; i++)
        decode_entry(wire_entries + (size_t)i * META_ENTRY_SIZE,
                     &replacement[first + i]);
    uint32_t tail = *count - first - removed;
    if (tail)
        memcpy(replacement + first + added, *chunks + first + removed,
               (size_t)tail * sizeof(*replacement));
    free(*chunks);
    *chunks = replacement;
    *count = new_count;
    return 0;
}

static int load_versioned_map(int fd, off_t file_size, myfs_inode_t *inode)
{
    unsigned char header[META_HEADER_SIZE];
    if (file_size < (off_t)sizeof(header))
        return -EIO;
    int ret = pread_full(fd, header, sizeof(header), 0);
    if (ret != 0)
        return ret;
    if (memcmp(header, meta_magic, sizeof(meta_magic)) != 0 ||
        get_le16(header + 10) != META_HEADER_SIZE ||
        get_le32(header + 16) != META_ENTRY_SIZE)
        return -EIO;

    uint16_t version = get_le16(header + 8);
    if (version != MYFS_META_VERSION_V1 && version != MYFS_META_VERSION_V2)
        return -EIO;
    uint32_t stored_header_crc = get_le32(header + 52);
    put_le32(header + 52, 0);
    if (chunk_crc32(header, sizeof(header)) != stored_header_crc)
        return -EIO;

    uint32_t window_size = version == MYFS_META_VERSION_V1
        ? MYFS_DEFAULT_WINDOW_SIZE : get_le32(header + 20);
    if (!myfs_window_size_valid(window_size))
        return -EIO;
    uint32_t count = get_le32(header + 24);
    uint64_t logical_size = get_le64(header + 32);
    uint64_t sequence = get_le64(header + 40);
    uint64_t entries_bytes64 = (uint64_t)count * META_ENTRY_SIZE;
    uint64_t base_end64 = META_HEADER_SIZE + entries_bytes64;
    if (base_end64 > (uint64_t)file_size || entries_bytes64 > SIZE_MAX)
        return -EIO;
    size_t entries_bytes = (size_t)entries_bytes64;
    unsigned char *wire = entries_bytes ? malloc(entries_bytes) : NULL;
    if (entries_bytes && !wire)
        return -ENOMEM;
    if (entries_bytes)
    {
        ret = pread_full(fd, wire, entries_bytes, META_HEADER_SIZE);
        if (ret != 0)
        {
            free(wire);
            return ret;
        }
    }
    if (chunk_crc32(wire, entries_bytes) != get_le32(header + 48))
    {
        free(wire);
        return -EIO;
    }

    myfs_chunk_t *chunks = count
        ? malloc((size_t)count * sizeof(*chunks)) : NULL;
    if (count && !chunks)
    {
        free(wire);
        return -ENOMEM;
    }
    for (uint32_t i = 0; i < count; i++)
        decode_entry(wire + (size_t)i * META_ENTRY_SIZE, &chunks[i]);
    free(wire);

    off_t cursor = (off_t)base_end64;
    uint32_t delta_count = 0;
    uint64_t journal_bytes = 0;
    while (file_size - cursor >= (off_t)DELTA_HEADER_SIZE)
    {
        unsigned char delta_header[DELTA_HEADER_SIZE];
        ret = pread_full(fd, delta_header, sizeof(delta_header), cursor);
        if (ret != 0)
            break;
        if (memcmp(delta_header, delta_magic, sizeof(delta_magic)) != 0)
        {
            ret = -EIO;
            break;
        }
        uint32_t record_len = get_le32(delta_header + 8);
        uint32_t added = get_le32(delta_header + 32);
        uint64_t expected_len = DELTA_HEADER_SIZE +
            (uint64_t)added * META_ENTRY_SIZE + DELTA_TRAILER_SIZE;
        if (record_len != expected_len || expected_len > SIZE_MAX)
        {
            ret = -EIO;
            break;
        }
        if ((uint64_t)(file_size - cursor) < expected_len)
        {
            ret = 0;
            break;
        }
        unsigned char *record = malloc(record_len);
        if (!record)
        {
            ret = -ENOMEM;
            break;
        }
        ret = pread_full(fd, record, record_len, cursor);
        if (ret == 0 &&
            get_le32(record + record_len - 4) != DELTA_COMMIT_MARKER)
        {
            /* The last record reached its intended length but not its commit
             * marker.  Treat it exactly like any other torn append tail. */
            free(record);
            ret = 0;
            break;
        }
        if (ret == 0 &&
            chunk_crc32(record, record_len - DELTA_TRAILER_SIZE) !=
                get_le32(record + record_len - DELTA_TRAILER_SIZE))
            ret = -EIO;
        uint64_t delta_sequence = get_le64(record + 16);
        if (ret == 0 && delta_sequence != sequence + 1)
            ret = -EIO;
        if (ret == 0)
            ret = splice_delta(&chunks, &count, get_le32(record + 24),
                               get_le32(record + 28),
                               record + DELTA_HEADER_SIZE, added);
        if (ret == 0)
        {
            logical_size = get_le64(record + 40);
            sequence = delta_sequence;
            delta_count++;
            journal_bytes += record_len;
            cursor += record_len;
        }
        free(record);
        if (ret != 0)
            break;
    }
    if (ret != 0)
    {
        free(chunks);
        return ret;
    }

    sort_chunks_by_logical_offset(chunks, count);
    free(inode->chunk_map.chunks);
    memset(inode, 0, sizeof(*inode));
    inode->chunk_map.chunks = chunks;
    inode->chunk_map.num_chunks = count;
    inode->chunk_map.logical_size = logical_size;
    inode->window_size = window_size;
    inode->metadata_version = version;
    inode->metadata_sequence = sequence;
    inode->metadata_delta_count = delta_count;
    inode->metadata_journal_bytes = journal_bytes;
    inode->metadata_valid_end = cursor;
    inode->chunk_map.fully_packed =
        chunk_map_is_packed(&inode->chunk_map, window_size);
    return 0;
}

int load_chunk_map_from_fd(int meta_fd, myfs_inode_t *inode)
{
    struct stat st;
    if (fstat(meta_fd, &st) != 0)
        return -errno;
    unsigned char prefix[sizeof(meta_magic)];
    if (st.st_size < (off_t)sizeof(prefix))
        return -EIO;
    int ret = pread_full(meta_fd, prefix, sizeof(prefix), 0);
    if (ret != 0)
        return ret;
    if (memcmp(prefix, meta_magic, sizeof(meta_magic)) == 0)
        return load_versioned_map(meta_fd, st.st_size, inode);
    return load_legacy_map(meta_fd, st.st_size, inode);
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
        memset(inode, 0, sizeof(*inode));
        inode->window_size = MYFS_DEFAULT_WINDOW_SIZE;
        inode->metadata_version = MYFS_META_VERSION_V2;
        inode->chunk_map.fully_packed = true;
        return 0;
    }
    return ret;
}

int save_chunk_map_to_path(const char *meta_path, myfs_inode_t *inode)
{
    uint32_t window_size = inode->window_size
        ? inode->window_size : MYFS_DEFAULT_WINDOW_SIZE;
    if (!myfs_window_size_valid(window_size))
        return -EINVAL;
    uint32_t count = inode->chunk_map.num_chunks;
    if (count && !inode->chunk_map.chunks)
        return -EINVAL;
    size_t entries_bytes = (size_t)count * META_ENTRY_SIZE;
    unsigned char *wire = entries_bytes ? malloc(entries_bytes) : NULL;
    if (entries_bytes && !wire)
        return -ENOMEM;
    for (uint32_t i = 0; i < count; i++)
        encode_entry(wire + (size_t)i * META_ENTRY_SIZE,
                     &inode->chunk_map.chunks[i]);

    unsigned char header[META_HEADER_SIZE] = {0};
    memcpy(header, meta_magic, sizeof(meta_magic));
    put_le16(header + 8, MYFS_META_VERSION_V2);
    put_le16(header + 10, META_HEADER_SIZE);
    put_le32(header + 16, META_ENTRY_SIZE);
    put_le32(header + 20, window_size);
    put_le32(header + 24, count);
    put_le64(header + 32, inode->chunk_map.logical_size);
    put_le64(header + 40, inode->metadata_sequence);
    put_le32(header + 48, chunk_crc32(wire, entries_bytes));
    put_le32(header + 52, 0);
    put_le32(header + 52, chunk_crc32(header, sizeof(header)));

    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", meta_path)
        >= (int)sizeof(tmp_path))
    {
        free(wire);
        return -ENAMETOOLONG;
    }
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
    {
        free(wire);
        return -errno;
    }
    int ret = write_full(fd, header, sizeof(header));
    if (ret == 0 && entries_bytes)
        ret = write_full(fd, wire, entries_bytes);
    free(wire);
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
    if (ret == 0)
    {
        inode->window_size = window_size;
        inode->metadata_version = MYFS_META_VERSION_V2;
        inode->metadata_delta_count = 0;
        inode->metadata_journal_bytes = 0;
        inode->metadata_valid_end =
            (off_t)(META_HEADER_SIZE + (uint64_t)entries_bytes);
        inode->chunk_map.fully_packed =
            chunk_map_is_packed(&inode->chunk_map, window_size);
    }
    return ret;
}

int append_chunk_map_delta_to_fd(int meta_fd, myfs_inode_t *inode,
                                 uint32_t first_idx, uint32_t removed_count,
                                 uint32_t added_count)
{
    if (!inode || (inode->metadata_version != MYFS_META_VERSION_V1 &&
                   inode->metadata_version != MYFS_META_VERSION_V2) ||
        first_idx > inode->chunk_map.num_chunks ||
        added_count > inode->chunk_map.num_chunks - first_idx)
        return -EINVAL;
    uint64_t record_len64 = DELTA_HEADER_SIZE +
        (uint64_t)added_count * META_ENTRY_SIZE + DELTA_TRAILER_SIZE;
    if (record_len64 > UINT32_MAX || record_len64 > SIZE_MAX)
        return -EFBIG;
    uint32_t record_len = (uint32_t)record_len64;
    unsigned char *record = calloc(1, record_len);
    if (!record)
        return -ENOMEM;
    memcpy(record, delta_magic, sizeof(delta_magic));
    put_le32(record + 8, record_len);
    put_le64(record + 16, inode->metadata_sequence + 1);
    put_le32(record + 24, first_idx);
    put_le32(record + 28, removed_count);
    put_le32(record + 32, added_count);
    put_le64(record + 40, inode->chunk_map.logical_size);
    for (uint32_t i = 0; i < added_count; i++)
        encode_entry(record + DELTA_HEADER_SIZE + (size_t)i * META_ENTRY_SIZE,
                     &inode->chunk_map.chunks[first_idx + i]);
    put_le32(record + record_len - DELTA_TRAILER_SIZE,
             chunk_crc32(record, record_len - DELTA_TRAILER_SIZE));
    put_le32(record + record_len - 4, DELTA_COMMIT_MARKER);

    struct stat st;
    int ret = fstat(meta_fd, &st) == 0 ? 0 : -errno;
    off_t original_end = inode->metadata_valid_end;
    if (ret == 0 && (original_end < (off_t)META_HEADER_SIZE ||
                     original_end > st.st_size))
        ret = -EIO;
    bool rollback_safe = ret == 0;
    if (ret == 0 && st.st_size != original_end &&
        ftruncate(meta_fd, original_end) != 0)
        ret = -errno;
    if (ret == 0 && lseek(meta_fd, original_end, SEEK_SET) < 0)
        ret = -errno;
    if (ret == 0)
        ret = write_full(meta_fd, record, record_len);
    if (ret == 0 && fdatasync(meta_fd) != 0)
        ret = -errno;
    if (ret != 0 && rollback_safe)
    {
        if (ftruncate(meta_fd, original_end) != 0)
            LOG("[WARN] metadata delta rollback truncate failed: %s\n",
                strerror(errno));
    }
    free(record);
    if (ret == 0)
    {
        inode->metadata_sequence++;
        inode->metadata_delta_count++;
        inode->metadata_journal_bytes += record_len;
        inode->metadata_valid_end += record_len;
    }
    return ret;
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
