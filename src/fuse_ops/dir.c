#include "myfs.h"

/*
 * Xử lý truy vấn thuộc tính của một đường dẫn trong filesystem.
 * Hàm này ưu tiên kiểm tra thư mục trước, sau đó mới suy luận tới file dữ liệu
 * tương ứng và cập nhật kích thước logic từ metadata nếu đó là file thường.
 */
int myfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    LOG("[DEBUG] getattr: %s\n", path);

    /* Kiểm tra đường dẫn như một thư mục trên backing store trước. */
    char real_path[PATH_MAX];
    build_path(real_path, path);

    if (stat(real_path, stbuf) == 0 && S_ISDIR(stbuf->st_mode))
        return 0; /* Nếu là thư mục thì trả về ngay. */

    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    myfs_file_handle_t *handle = (fi && fi->fh != 0)
        ? (myfs_file_handle_t *)(uintptr_t)fi->fh : NULL;
    myfs_storage_t storage;
    int ret = 0;
    if (handle)
        storage = handle->storage;
    else
    {
        ret = resolve_storage(path, &storage);
        if (ret == 0)
        {
            int recovery_ret = recover_generations_for_path_locked(path, &storage);
            if (recovery_ret != 0)
                LOG("[WARN] getattr: generation recovery returned %d\n", recovery_ret);
        }
    }

    if (ret == 0)
    {
        if (handle)
            ret = (fstat(handle->data_fd, stbuf) == 0) ? 0 : -errno;
        else
            ret = (stat(storage.data_path, stbuf) == 0) ? 0 : -errno;
    }

    /* Cập nhật kích thước logic từ chunk map để phản ánh đúng nội dung file. */
    myfs_inode_t inode = {0};
    if (ret == 0)
    {
        int meta_ret = load_chunk_map_from_path(storage.meta_path, &inode);
        if (meta_ret == -ENOENT && handle)
            meta_ret = load_chunk_map_from_fd(handle->meta_fd, &inode);
        if (meta_ret == 0)
            stbuf->st_size = INODE_LSIZE(inode);
        else
            ret = meta_ret;
    }
    free(inode.chunk_map.chunks);

    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0 && ret == 0)
        ret = -unlock_ret;

    return ret;
}

static bool has_hex_generation_suffix(const char *name)
{
    const char *marker = NULL;
    const char *search = name;
    while ((search = strstr(search, ".g.")) != NULL)
    {
        marker = search;
        search += 3;
    }
    if (!marker)
        return false;
    const char *id = marker + 3;
    if (strlen(id) != MYFS_GENERATION_HEX_LEN)
        return false;
    for (size_t i = 0; i < MYFS_GENERATION_HEX_LEN; i++)
    {
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    }
    return true;
}

// Hàm đọc thư mục (readdir)
int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                 off_t offset, struct fuse_file_info *fi,
                 enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;
    LOG("[DEBUG] readdir: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);

    DIR *dp = opendir(realpath);
    if (!dp)
        return -errno;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    struct dirent *de;

    /* Dedup set: mỗi file logic chỉ được emit 1 lần dù có cả .data lẫn .meta */
    #define MAX_SEEN 2048
    char seen[MAX_SEEN][256];
    int seen_count = 0;

    while ((de = readdir(dp)) != NULL)
    {
        size_t len = strlen(de->d_name);

        if (has_hex_generation_suffix(de->d_name) ||
            strstr(de->d_name, ".current.tmp.") != NULL ||
            strstr(de->d_name, ".data.alias.") != NULL ||
            strstr(de->d_name, ".meta.alias.") != NULL)
            continue;

        if (len > 5 &&
            (strcmp(de->d_name + len - 5, ".meta") == 0 ||
             strcmp(de->d_name + len - 5, ".data") == 0))
        {
            /* Ẩn file metadata, thay vào đó emit tên logic (bỏ .data/.meta) */
            size_t blen = len - 5;
            if (blen >= sizeof(seen[0]))
                blen = sizeof(seen[0]) - 1;

            char base[256];
            memcpy(base, de->d_name, blen);
            base[blen] = '\0';

            /* Dedup: tránh emit trùng khi cả .data và .meta cùng tồn tại */
            int already = 0;
            for (int i = 0; i < seen_count; i++)
            {
                if (strcmp(seen[i], base) == 0)
                {
                    already = 1;
                    break;
                }
            }
            if (!already && seen_count < MAX_SEEN)
            {
                strncpy(seen[seen_count], base, sizeof(seen[0]) - 1);
                seen[seen_count][sizeof(seen[0]) - 1] = '\0';
                seen_count++;
                filler(buf, base, NULL, 0, 0);
            }
            continue;
        }

        if (len > 8 && strcmp(de->d_name + len - 8, ".current") == 0)
        {
            size_t blen = len - 8;
            if (blen >= sizeof(seen[0]))
                blen = sizeof(seen[0]) - 1;

            char base[256];
            memcpy(base, de->d_name, blen);
            base[blen] = '\0';
            int already = 0;
            for (int i = 0; i < seen_count; i++)
            {
                if (strcmp(seen[i], base) == 0)
                {
                    already = 1;
                    break;
                }
            }
            if (!already && seen_count < MAX_SEEN)
            {
                strncpy(seen[seen_count], base, sizeof(seen[0]) - 1);
                seen[seen_count][sizeof(seen[0]) - 1] = '\0';
                seen_count++;
                filler(buf, base, NULL, 0, 0);
            }
            continue;
        }

        /* Thư mục thật (và các entry không phải .data/.meta) hiện nguyên tên */
        filler(buf, de->d_name, NULL, 0, 0);
    }
    closedir(dp);
    return 0;
}

// Hàm tạo file (mknod)
int myfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    LOG("[DEBUG] mknod: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int res = mknod(realpath, mode, rdev);
    if (res == -1)
        return -errno;
    return 0;
}

/* Cập nhật timestamp truy cập/sửa đổi của file. Hiện tại thao tác này chưa được ánh xạ. */
int myfs_utimens(const char *path, const struct timespec tv[2],
                 struct fuse_file_info *fi)
{
    (void)path;
    (void)tv;
    (void)fi;
    LOG("[DEBUG] utimens: %s (stub - ignored)\n", path);
    return 0;
}

/* Tạo thư mục trên backing store tương ứng với path logic của FUSE. */
int myfs_mkdir(const char *path, mode_t mode)
{
    LOG("[DEBUG] mkdir: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int res = mkdir(realpath, mode);
    if (res == -1)
        return -errno;
    return 0;
}

/*
 * Xoá file logic bằng cách xoá cả dữ liệu .data lẫn metadata .meta.
 * Hàm cố tình bỏ qua ENOENT để thao tác trở thành idempotent, tức là an toàn
 * ngay cả khi một trong hai file đã không còn tồn tại.
 */
static int myfs_unlink_locked(const char *path)
{
    LOG("[DEBUG] unlink: %s\n", path);

    myfs_storage_t storage;
    int ret = resolve_storage(path, &storage);
    if (ret != 0)
        return ret;

    if (!storage.is_legacy)
    {
        int recovery_ret = recover_generations_for_path_locked(path, &storage);
        if (recovery_ret != 0)
            LOG("[WARN] unlink: generation recovery returned %d\n", recovery_ret);

        char current_path[PATH_MAX];
        char data_alias[PATH_MAX];
        char meta_alias[PATH_MAX];
        build_current_path(current_path, path);
        build_data_path(data_alias, path);
        build_meta_path(meta_alias, path);

        if (unlink(current_path) != 0)
            return -errno;
        ret = fsync_parent_path(current_path);
        if (ret != 0)
            return ret;

        if (unlink(data_alias) != 0 && errno != ENOENT)
            return -errno;
        if (unlink(meta_alias) != 0 && errno != ENOENT)
            return -errno;
        ret = fsync_parent_path(data_alias);
        if (ret != 0)
            return ret;

        ret = mark_generation_for_gc_locked(&storage, false);
        if (ret != 0)
            return ret;
        return run_generation_gc_locked();
    }

    char data_path[PATH_MAX];
    char meta_path[PATH_MAX];
    build_data_path(data_path, path);
    build_meta_path(meta_path, path);

    /* Bỏ qua ENOENT để thao tác xoá có thể lặp lại mà không gây lỗi. */
    LOG("[DEBUG] unlink data: %s\n", data_path);
    if (unlink(data_path) == -1 && errno != ENOENT)
    {
        LOG("[ERROR] unlink data failed: errno=%d (%s)\n", errno, strerror(errno));
        return -errno;
    }
    LOG("[DEBUG] unlink meta: %s\n", meta_path);
    if (unlink(meta_path) == -1 && errno != ENOENT)
    {
        LOG("[ERROR] unlink meta failed: errno=%d (%s)\n", errno, strerror(errno));
        return -errno;
    }

    return fsync_parent_path(data_path);
}

int myfs_unlink(const char *path)
{
    int lock_ret = pthread_mutex_lock(&myfs_metadata_mutex);
    if (lock_ret != 0)
        return -lock_ret;

    int ret = myfs_unlink_locked(path);
    int unlock_ret = pthread_mutex_unlock(&myfs_metadata_mutex);
    if (unlock_ret != 0)
        return -unlock_ret;

    return ret;
}

/* Xoá thư mục trên backing store tương ứng với path logic của FUSE. */
int myfs_rmdir(const char *path)
{
    LOG("[DEBUG] rmdir: %s\n", path);
    char realpath[PATH_MAX];
    build_path(realpath, path);
    int res = rmdir(realpath);
    if (res == -1)
        return -errno;
    return 0;
}
