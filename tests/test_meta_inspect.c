#include "myfs.h"

#include <assert.h>
#include <inttypes.h>
#include <sys/wait.h>

static const char *inspector_path;

static void write_all_or_die(int fd, const void *buf, size_t len)
{
    const unsigned char *cursor = buf;
    while (len > 0)
    {
        ssize_t written = write(fd, cursor, len);
        assert(written > 0);
        cursor += written;
        len -= (size_t)written;
    }
}

static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put64(unsigned char *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static int inspect(const char *meta_path, char *output, size_t output_size)
{
    char command[PATH_MAX * 2];
    int n = snprintf(command, sizeof(command), "%s %s 2>/dev/null",
                     inspector_path, meta_path);
    assert(n > 0 && (size_t)n < sizeof(command));

    FILE *pipe = popen(command, "r");
    assert(pipe != NULL);
    size_t used = fread(output, 1, output_size - 1, pipe);
    output[used] = '\0';
    int status = pclose(pipe);
    assert(status != -1);
    if (!WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void assert_inspection(const char *path, const char *expected)
{
    char output[256];
    assert(inspect(path, output, sizeof(output)) == 0);
    assert(strcmp(output, expected) == 0);
}

static void make_legacy_fixture(const char *path)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    uint32_t count = 2;
    uint64_t logical_size = 65537;
    myfs_chunk_t chunks[2] = {
        {
            .logical_offset = 0,
            .raw_size = 65536,
            .stored_size = 65536,
            .codec_type = 0,
            .physical_offset = 0,
        },
        {
            .logical_offset = 65536,
            .raw_size = 1,
            .stored_size = 1,
            .codec_type = 1,
            .physical_offset = 65536,
        },
    };
    write_all_or_die(fd, &count, sizeof(count));
    write_all_or_die(fd, &logical_size, sizeof(logical_size));
    write_all_or_die(fd, chunks, sizeof(chunks));
    assert(close(fd) == 0);
}

static void encode_wire_entry(unsigned char *entry, uint64_t logical_offset,
                              uint64_t physical_offset, uint32_t raw_size,
                              uint32_t stored_size, uint8_t codec)
{
    memset(entry, 0, 32);
    put64(entry, logical_offset);
    put64(entry + 8, physical_offset);
    put32(entry + 16, raw_size);
    put32(entry + 20, stored_size);
    entry[28] = codec;
}

static void make_v1_fixture(const char *path)
{
    unsigned char header[64] = {0};
    unsigned char entries[64];
    encode_wire_entry(entries, 0, 0, 4096, 128, 1);
    encode_wire_entry(entries + 32, 4096, 128, 17, 17, 0);

    memcpy(header, "MYFSMETA", 8);
    put16(header + 8, MYFS_META_VERSION_V1);
    put16(header + 10, sizeof(header));
    put32(header + 16, 32);
    put32(header + 24, 2);
    put64(header + 32, 4113);
    put32(header + 48, chunk_crc32(entries, sizeof(entries)));
    put32(header + 52, 0);
    put32(header + 52, chunk_crc32(header, sizeof(header)));

    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    write_all_or_die(fd, header, sizeof(header));
    write_all_or_die(fd, entries, sizeof(entries));
    assert(close(fd) == 0);
}

static void make_v2_delta_fixture(const char *path)
{
    myfs_chunk_t base_chunk = {
        .logical_offset = 0,
        .raw_size = 100,
        .stored_size = 100,
        .codec_type = 0,
        .physical_offset = 0,
    };
    myfs_inode_t inode = {
        .chunk_map = {
            .num_chunks = 1,
            .logical_size = 100,
            .chunks = &base_chunk,
        },
        .window_size = MYFS_DEFAULT_WINDOW_SIZE,
    };
    assert(save_chunk_map_to_path(path, &inode) == 0);

    myfs_chunk_t replacement[2] = {
        {
            .logical_offset = 0,
            .raw_size = 100,
            .stored_size = 40,
            .codec_type = 1,
            .physical_offset = 100,
        },
        {
            .logical_offset = 100,
            .raw_size = 100,
            .stored_size = 100,
            .codec_type = 0,
            .physical_offset = 140,
        },
    };
    inode.chunk_map.num_chunks = 2;
    inode.chunk_map.logical_size = 200;
    inode.chunk_map.chunks = replacement;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(append_chunk_map_delta_to_fd(fd, &inode, 0, 1, 2) == 0);
    assert(close(fd) == 0);
}

static void test_supported_formats(const char *dir)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/v0.meta", dir);
    make_legacy_fixture(path);
    assert_inspection(path,
        "chunks=2\tlogical_size=65537\traw_chunks=1\n");

    snprintf(path, sizeof(path), "%s/v1.meta", dir);
    make_v1_fixture(path);
    assert_inspection(path,
        "chunks=2\tlogical_size=4113\traw_chunks=1\n");

    snprintf(path, sizeof(path), "%s/v2-delta.meta", dir);
    make_v2_delta_fixture(path);
    assert_inspection(path,
        "chunks=2\tlogical_size=200\traw_chunks=1\n");
}

static void test_invalid_metadata_is_rejected(const char *dir)
{
    char path[PATH_MAX];
    char output[256];

    snprintf(path, sizeof(path), "%s/unknown.meta", dir);
    make_v1_fixture(path);
    uint16_t unknown_version = 3;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(pwrite(fd, &unknown_version, sizeof(unknown_version), 8) ==
           (ssize_t)sizeof(unknown_version));
    assert(close(fd) == 0);
    assert(inspect(path, output, sizeof(output)) != 0);
    assert(output[0] == '\0');

    snprintf(path, sizeof(path), "%s/bad-crc.meta", dir);
    make_v2_delta_fixture(path);
    unsigned char changed = 0xff;
    fd = open(path, O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    assert(pwrite(fd, &changed, sizeof(changed), 32) ==
           (ssize_t)sizeof(changed));
    assert(close(fd) == 0);
    assert(inspect(path, output, sizeof(output)) != 0);
    assert(output[0] == '\0');
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    inspector_path = argv[1];

    char template[] = "/tmp/myfs-meta-inspect-test-XXXXXX";
    char *dir = mkdtemp(template);
    assert(dir != NULL);

    test_supported_formats(dir);
    test_invalid_metadata_is_rejected(dir);

    char path[PATH_MAX];
    const char *names[] = {
        "v0.meta", "v1.meta", "v2-delta.meta", "unknown.meta", "bad-crc.meta"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        assert(unlink(path) == 0);
    }
    assert(rmdir(dir) == 0);
    return 0;
}
