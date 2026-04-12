#include "myfs.h"

struct myfs_config *conf = NULL;

void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 1;
    printf("[DEBUG] FUSE init called\n");
    return fuse_get_context()->private_data;
}

void myfs_destroy(void *private_data)
{
    free(private_data);
    printf("[DEBUG] FUSE destroy called\n");
}

void build_path(char *dest, const char *path)
{
    snprintf(dest, PATH_MAX, "%s%s", conf->root, path);
}