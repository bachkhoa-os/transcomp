#include "myfs.h"

struct myfs_config *conf = NULL;

static void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 1;
    return fuse_get_context()->private_data;
}

static void myfs_destroy(void *private_data)
{
    free(private_data);
}

void build_path(char *dest, const char *path)
{
    snprintf(dest, PATH_MAX, "%s%s", conf->root, path);
}