#include "myfs.h"

/*
 * Truy xuất cấu hình của hệ thống tập tin từ private_data của FUSE context.
 * Hàm này đóng vai trò là điểm truy cập chung để các helper khác lấy cấu hình
 * hiện hành mà không cần truyền thủ công qua từng lời gọi.
 */
static inline struct myfs_config *get_conf()
{
    return (struct myfs_config *)fuse_get_context()->private_data;
}

/*
 * Dựng đường dẫn vật lý đầy đủ trên backing storage từ đường dẫn logic của FUSE.
 * Hàm này ghép thư mục gốc cấu hình với path do FUSE cung cấp để các thao tác
 * phía sau luôn làm việc trên đúng đối tượng lưu trữ.
 */
void build_path(char *dest, const char *path)
{
    struct myfs_config *conf = get_conf();
    if (!conf || conf->root[0] == '\0')
    {
        LOG("[ERROR] config not initialized\n");
        return;
    }
    snprintf(dest, PATH_MAX, "%s%s", conf->root, path);
}

/*
 * Tạo đường dẫn tới file dữ liệu thực tế tương ứng với một path logic.
 * Tệp .data lưu nội dung người dùng, tách biệt khỏi metadata để việc quản lý
 * dữ liệu và trạng thái nén được rõ ràng hơn.
 */
void build_data_path(char *dest, const char *path)
{
    /* build_path đã đảm bảo dest là chuỗi kết thúc bằng null. */
    build_path(dest, path);
    size_t len = strlen(dest);
    if (len + 6 >= PATH_MAX)
    {
        /* 5 ký tự của ".data" cộng với ký tự null kết thúc chuỗi. */
        LOG("[ERROR] path too long for .data\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".data");
}

/*
 * Tạo đường dẫn tới file metadata tương ứng với một path logic.
 * File .meta lưu chunk map và logical size của đối tượng, tách riêng khỏi
 * payload để cập nhật metadata không ảnh hưởng trực tiếp đến dữ liệu chính.
 */
void build_meta_path(char *dest, const char *path)
{
    build_path(dest, path);
    size_t len = strlen(dest);
    if (len + 6 >= PATH_MAX)
    {
        LOG("[ERROR] path too long for .meta\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".meta");
}