#include <stdio.h>
#include <errno.h>
#include "guards.h"

int guard_chunk_metadata(const myfs_chunk_t *chunk, uint32_t chunk_idx)
{
    if (chunk->raw_size == 0 || chunk->stored_size == 0)
    {
        printf("[ERROR] chunk %u has zero size in metadata\n", chunk_idx);
        return -EIO;
    }

    if (chunk->physical_offset < 0)
    {
        printf("[ERROR] chunk %u has negative physical_offset\n", chunk_idx);
        return -EIO;
    }

    return 0;
}

int guard_chunk_logical_offset(off_t cur_offset, off_t chunk_logical_start)
{
    if (cur_offset < chunk_logical_start)
    {
        printf("[ERROR] cur_offset=%ld < chunk_logical_start=%ld\n",
               cur_offset, chunk_logical_start);
        return -EIO;
    }

    return 0;
}

int guard_chunk_bounds(size_t chunk_offset, size_t chunk_size)
{
    if (chunk_offset >= chunk_size)
    {
        printf("[ERROR] chunk_offset=%zu >= chunk_size=%zu\n",
               chunk_offset, chunk_size);
        return -EIO;
    }

    return 0;
}

int guard_pread_result(ssize_t n, size_t expected)
{
    if (n != (ssize_t)expected)
    {
        printf("[ERROR] pread expected %zu got %zd\n", expected, n);
        return -EIO;
    }

    return 0;
}

int guard_malloc(void *ptr)
{
    if (!ptr)
    {
        printf("[ERROR] malloc failed\n");
        return -ENOMEM;
    }

    return 0;
}

int guard_codec_type(int codec_type)
{
    if (codec_type != 0 && codec_type != 1)
    {
        printf("[ERROR] unsupported codec_type=%d\n", codec_type);
        return -EIO;
    }

    return 0;
}

int guard_decompress_size(size_t actual, size_t expected)
{
    if (actual != expected)
    {
        printf("[ERROR] decomp_size=%zu != expected=%zu\n",
               actual, expected);
        return -EIO;
    }

    return 0;
}

int cleanup_fd(int fd, int use_fi_fd, int err)
{
    if (!use_fi_fd)
        close(fd);
    return err;
}