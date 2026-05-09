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
    .unlink = myfs_unlink,
    .init = myfs_init,
    .destroy = myfs_destroy,
    .truncate = myfs_truncate,
    .utimens = myfs_utimens
};

int main(int argc, char *argv[])
{
    printf("[DEBUG] FUSE file system is starting up...\n");

    /*
     * Điểm khởi động của chương trình mount FUSE.
     * Quy trình ở đây gồm kiểm tra tham số, chuẩn hoá đường dẫn backing store,
     * xác thực thư mục lưu trữ và cuối cùng chuyển quyền điều khiển cho fuse_main.
     */
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <mountpoint> <backing_dir>\n", argv[0]);
        return 1;
    }

    /* Khởi tạo cấu hình runtime của filesystem và cấp phát vùng nhớ chứa nó. */
    struct myfs_config *conf = calloc(1, sizeof(struct myfs_config));
    if (!conf)
    {
        perror("calloc");
        return 1;
    }

    /* Chuẩn hoá backing directory thành đường dẫn tuyệt đối để tránh nhập nhằng. */
    if (!realpath(argv[argc - 1], conf->root))
    {
        perror("realpath");
        free(conf);
        return 1;
    }

    /* Xác nhận backing path tồn tại và thực sự là một thư mục hợp lệ. */
    struct stat st;
    if (stat(conf->root, &st) == -1 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "Invalid backing directory: %s\n", conf->root);
        free(conf);
        return 1;
    }
    printf("[DEBUG] backing dir = %s\n", conf->root);

    /*
     * Fuse nhận cấu hình qua private_data, vì vậy loại bỏ backing_dir khỏi argv
     * trước khi gọi fuse_main để chỉ còn các tham số dành cho FUSE.
     */
    argv[argc - 1] = NULL;
    argc--;

    return fuse_main(argc, argv, &myfs_oper, conf);
}