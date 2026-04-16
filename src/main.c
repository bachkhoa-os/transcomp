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
    .truncate = myfs_truncate,
    .utimens = myfs_utimens
};

int main(int argc, char *argv[])
{
    printf("[DEBUG] FUSE file system is starting up...\n");

    // Đảm bảo người dùng cung cấp đủ tham số
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <mountpoint> <backing_dir>\n", argv[0]);
        return 1;
    }

    // Khởi tạo cấu hình và xác thực thư mục lưu trữ
    struct myfs_config *conf = calloc(1, sizeof(struct myfs_config));
    if (!conf)
    {
        perror("calloc");
        return 1;
    }

    // Lấy đường dẫn tuyệt đối của thư mục lưu trữ
    if (!realpath(argv[argc - 1], conf->root))
    {
        perror("realpath");
        free(conf);
        return 1;
    }

    // Kiểm tra xem đường dẫn lưu trữ có tồn tại và là thư mục hay không
    struct stat st;
    if (stat(conf->root, &st) == -1 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Invalid backing directory: %s\n", conf->root);
        free(conf);
        return 1;
    }
    printf("[DEBUG] backing dir = %s\n", conf->root);

    // Truyền cấu hình qua context của FUSE
    argv[argc - 1] = NULL;
    argc--;

    return fuse_main(argc, argv, &myfs_oper, conf);
}