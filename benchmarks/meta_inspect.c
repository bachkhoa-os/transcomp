#include "myfs.h"

#include <inttypes.h>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s META_PATH\n", argv[0]);
        return 2;
    }

    myfs_inode_t inode = {0};
    int ret = load_chunk_map_from_path(argv[1], &inode);
    if (ret != 0)
    {
        fprintf(stderr, "meta_inspect: %s: %s\n", argv[1],
                strerror(ret < 0 ? -ret : ret));
        return 1;
    }

    uint32_t raw_chunks = 0;
    for (uint32_t i = 0; i < inode.chunk_map.num_chunks; i++)
    {
        if (inode.chunk_map.chunks[i].codec_type == 0)
            raw_chunks++;
    }

    printf("chunks=%" PRIu32 "\tlogical_size=%" PRIu64
           "\traw_chunks=%" PRIu32 "\n",
           inode.chunk_map.num_chunks, inode.chunk_map.logical_size,
           raw_chunks);
    free(inode.chunk_map.chunks);
    return 0;
}
