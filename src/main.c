#include "myfs.h"

static struct fuse_operations myfs_oper = {
    .getattr = myfs_getattr,
    .readdir = myfs_readdir,
    .mknod = myfs_mknod,
    .create = myfs_create,
    .open = myfs_open,
    .read = myfs_read,
    .write = myfs_write,
    .release = myfs_release,
    .mkdir = myfs_mkdir,
    .rmdir = myfs_rmdir,
    .init = myfs_init,
    .destroy = myfs_destroy,
};

int main(int argc, char *argv[])
{
    printf("[DEBUG] FUSE file system is starting up...\n");

    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <mountpoint> <backing_dir>\n", argv[0]);
        return 1;
    }

    conf = malloc(sizeof(struct myfs_config));
    if (!conf)
    {
        perror("malloc");
        return 1;
    }

    if (!realpath(argv[argc - 1], conf->root))
    {
        perror("realpath");
        return 1;
    }

    struct stat st;
    if (stat(conf->root, &st) == -1 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Invalid backing directory: %s\n", conf->root);
        return 1;
    }

    argv[argc - 1] = NULL;
    argc--;

    return fuse_main(argc, argv, &myfs_oper, conf);
}