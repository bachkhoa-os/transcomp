#include "myfs.h"

#include <assert.h>

static void write_all_or_die(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        assert(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void put32(unsigned char *p, uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void test_legacy_metadata_defaults_to_64k(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/legacy.meta", dir);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);

    uint32_t count = 1;
    uint64_t logical_size = 7;
    myfs_chunk_t chunk = {
        .logical_offset = 0,
        .raw_size = 7,
        .stored_size = 7,
        .codec_type = 0,
        .physical_offset = 0,
    };
    write_all_or_die(fd, &count, sizeof(count));
    write_all_or_die(fd, &logical_size, sizeof(logical_size));
    write_all_or_die(fd, &chunk, sizeof(chunk));
    assert(close(fd) == 0);

    myfs_inode_t inode = {0};
    assert(load_chunk_map_from_path(path, &inode) == 0);
    assert(inode.window_size == MYFS_DEFAULT_WINDOW_SIZE);
    assert(inode.metadata_version == MYFS_META_VERSION_LEGACY);
    assert(inode.chunk_map.num_chunks == 1);
    assert(INODE_LSIZE(inode) == 7);
    free(inode.chunk_map.chunks);
}

static void test_v2_round_trip_and_delta_recovery(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/v2.meta", dir);

    myfs_inode_t inode = {0};
    inode.window_size = 16 * 1024;
    inode.chunk_map.fully_packed = true;
    assert(save_chunk_map_to_path(path, &inode) == 0);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    myfs_chunk_t *chunks = calloc(1, sizeof(*chunks));
    assert(chunks != NULL);
    chunks[0].logical_offset = 0;
    chunks[0].raw_size = 3;
    chunks[0].stored_size = 3;
    chunks[0].checksum = 0x12345678U;
    chunks[0].physical_offset = 9;
    inode.chunk_map.chunks = chunks;
    inode.chunk_map.num_chunks = 1;
    INODE_LSIZE(inode) = 3;
    assert(append_chunk_map_delta_to_fd(fd, &inode, 0, 0, 1) == 0);

    const unsigned char torn_tail[] = { 'M', 'Y', 'F', 'S', 'D' };
    assert(lseek(fd, 0, SEEK_END) >= 0);
    write_all_or_die(fd, torn_tail, sizeof(torn_tail));
    assert(close(fd) == 0);
    free(chunks);

    myfs_inode_t loaded = {0};
    assert(load_chunk_map_from_path(path, &loaded) == 0);
    assert(loaded.metadata_version == MYFS_META_VERSION_V2);
    assert(loaded.window_size == 16 * 1024);
    assert(loaded.chunk_map.num_chunks == 1);
    assert(INODE_LSIZE(loaded) == 3);
    assert(loaded.chunk_map.chunks[0].logical_offset == 0);
    assert(loaded.chunk_map.chunks[0].physical_offset == 9);
    assert(loaded.chunk_map.chunks[0].checksum == 0x12345678U);
    assert(loaded.metadata_delta_count == 1);

    /* A writable reopen must remove the ignored torn suffix before it
     * appends another committed delta.  Otherwise replay stops at the old
     * suffix and can never reach the new record. */
    loaded.chunk_map.chunks[0].physical_offset = 10;
    fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(append_chunk_map_delta_to_fd(fd, &loaded, 0, 1, 1) == 0);
    assert(close(fd) == 0);
    free(loaded.chunk_map.chunks);

    memset(&loaded, 0, sizeof(loaded));
    assert(load_chunk_map_from_path(path, &loaded) == 0);
    assert(loaded.chunk_map.num_chunks == 1);
    assert(loaded.chunk_map.chunks[0].physical_offset == 10);
    assert(loaded.metadata_delta_count == 2);
    free(loaded.chunk_map.chunks);
}

static void test_v1_metadata_derives_legacy_window(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/v1.meta", dir);
    unsigned char header[64] = {0};
    memcpy(header, "MYFSMETA", 8);
    put16(header + 8, MYFS_META_VERSION_V1);
    put16(header + 10, 64);
    put32(header + 16, 32);
    put32(header + 20, 123); /* v1 reserved field must be ignored */
    put32(header + 48, chunk_crc32(NULL, 0));
    put32(header + 52, 0);
    put32(header + 52, chunk_crc32(header, sizeof(header)));
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    write_all_or_die(fd, header, sizeof(header));
    assert(close(fd) == 0);

    myfs_inode_t inode = {0};
    assert(load_chunk_map_from_path(path, &inode) == 0);
    assert(inode.metadata_version == MYFS_META_VERSION_V1);
    assert(inode.window_size == MYFS_DEFAULT_WINDOW_SIZE);
    free(inode.chunk_map.chunks);
}

static void test_uncommitted_full_length_tail_is_ignored(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/uncommitted.meta", dir);
    myfs_inode_t inode = {.window_size = MYFS_DEFAULT_WINDOW_SIZE};
    assert(save_chunk_map_to_path(path, &inode) == 0);
    inode.chunk_map.chunks = calloc(1, sizeof(*inode.chunk_map.chunks));
    assert(inode.chunk_map.chunks != NULL);
    inode.chunk_map.num_chunks = 1;
    inode.chunk_map.logical_size = 1;
    inode.chunk_map.chunks[0].raw_size = 1;
    inode.chunk_map.chunks[0].stored_size = 1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(append_chunk_map_delta_to_fd(fd, &inode, 0, 0, 1) == 0);
    off_t end = lseek(fd, 0, SEEK_END);
    assert(end > 4);
    uint32_t no_commit = 0;
    assert(pwrite(fd, &no_commit, sizeof(no_commit), end - 4) == 4);
    assert(close(fd) == 0);
    free(inode.chunk_map.chunks);

    myfs_inode_t loaded = {0};
    assert(load_chunk_map_from_path(path, &loaded) == 0);
    assert(loaded.chunk_map.num_chunks == 0);
    assert(INODE_LSIZE(loaded) == 0);
    free(loaded.chunk_map.chunks);
}

static void test_v2_rejects_invalid_window_size(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/invalid-window.meta", dir);
    myfs_inode_t inode = {.window_size = MYFS_DEFAULT_WINDOW_SIZE};
    assert(save_chunk_map_to_path(path, &inode) == 0);
    unsigned char header[64];
    int fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    put32(header + 20, 8 * 1024);
    put32(header + 52, 0);
    put32(header + 52, chunk_crc32(header, sizeof(header)));
    assert(pwrite(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header));
    assert(close(fd) == 0);
    myfs_inode_t loaded = {0};
    assert(load_chunk_map_from_path(path, &loaded) == -EIO);
}

static void test_single_chunk_update_has_constant_metadata_growth(const char *dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/large-map.meta", dir);
    myfs_inode_t inode = {0};
    inode.window_size = MYFS_DEFAULT_WINDOW_SIZE;
    inode.chunk_map.num_chunks = 4096;
    inode.chunk_map.logical_size = 4096ULL * MYFS_DEFAULT_WINDOW_SIZE;
    inode.chunk_map.chunks = calloc(inode.chunk_map.num_chunks,
                                    sizeof(*inode.chunk_map.chunks));
    assert(inode.chunk_map.chunks != NULL);
    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
    {
        inode.chunk_map.chunks[i].logical_offset =
            (uint64_t)i * MYFS_DEFAULT_WINDOW_SIZE;
        inode.chunk_map.chunks[i].physical_offset = i;
        inode.chunk_map.chunks[i].raw_size = 1;
        inode.chunk_map.chunks[i].stored_size = 1;
    }
    assert(save_chunk_map_to_path(path, &inode) == 0);
    struct stat before;
    assert(stat(path, &before) == 0);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    inode.chunk_map.chunks[2048].physical_offset = 0xabcdef;
    assert(append_chunk_map_delta_to_fd(fd, &inode, 2048, 1, 1) == 0);
    assert(close(fd) == 0);
    struct stat after;
    assert(stat(path, &after) == 0);
    assert(after.st_size - before.st_size == 88);

    myfs_inode_t loaded = {0};
    assert(load_chunk_map_from_path(path, &loaded) == 0);
    assert(loaded.chunk_map.num_chunks == 4096);
    assert(loaded.chunk_map.chunks[2048].physical_offset == 0xabcdef);
    free(loaded.chunk_map.chunks);
    free(inode.chunk_map.chunks);
}

int main(void)
{
    char template[] = "/tmp/myfs-metadata-test-XXXXXX";
    char *dir = mkdtemp(template);
    assert(dir != NULL);

    test_legacy_metadata_defaults_to_64k(dir);
    test_v2_round_trip_and_delta_recovery(dir);
    test_v1_metadata_derives_legacy_window(dir);
    test_uncommitted_full_length_tail_is_ignored(dir);
    test_v2_rejects_invalid_window_size(dir);
    test_single_chunk_update_has_constant_metadata_growth(dir);
    return 0;
}
