#include "myfs.h"

#include <ctype.h>
#include <sys/random.h>

#define MYFS_GENERATION_MARKER_PREFIX "MYFS_GENERATION_V1 "

/* Cấu hình mount toàn cục — set một lần trong main() trước fuse_main().
 * Cho phép build_path hoạt động cả ngoài FUSE callback (background worker
 * thread không có fuse_context hợp lệ). */
struct myfs_config *myfs_conf = NULL;

static inline struct myfs_config *get_conf(void)
{
    if (myfs_conf)
        return myfs_conf;
    struct fuse_context *ctx = fuse_get_context();
    return ctx ? (struct myfs_config *)ctx->private_data : NULL;
}

void build_path(char *dest, const char *path)
{
    struct myfs_config *conf = get_conf();
    if (!conf || conf->root[0] == '\0')
    {
        LOG("[ERROR] config not initialized\n");
        dest[0] = '\0';
        return;
    }

    if (snprintf(dest, PATH_MAX, "%s%s", conf->root, path) >= PATH_MAX)
    {
        LOG("[ERROR] backing path too long\n");
        dest[0] = '\0';
    }
}

/* These two names remain the legacy storage pair and the compatibility names
 * inspected by the existing tests.  Once a file has generations, myfs itself
 * resolves .current and does not use these aliases for I/O. */
void build_data_path(char *dest, const char *path)
{
    build_path(dest, path);
    size_t len = strlen(dest);
    if (dest[0] == '\0' || len + sizeof(".data") >= PATH_MAX)
    {
        LOG("[ERROR] path too long for .data\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".data");
}

void build_meta_path(char *dest, const char *path)
{
    build_path(dest, path);
    size_t len = strlen(dest);
    if (dest[0] == '\0' || len + sizeof(".meta") >= PATH_MAX)
    {
        LOG("[ERROR] path too long for .meta\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".meta");
}

void build_current_path(char *dest, const char *path)
{
    build_path(dest, path);
    size_t len = strlen(dest);
    if (dest[0] == '\0' || len + sizeof(".current") >= PATH_MAX)
    {
        LOG("[ERROR] path too long for .current\n");
        dest[0] = '\0';
        return;
    }
    strcat(dest, ".current");
}

static int copy_logical_path(myfs_storage_t *storage, const char *path)
{
    if (snprintf(storage->logical_path, sizeof(storage->logical_path), "%s", path)
        >= (int)sizeof(storage->logical_path))
        return -ENAMETOOLONG;
    return 0;
}

static void populate_data_identity(myfs_storage_t *storage)
{
    struct stat st;
    if (stat(storage->data_path, &st) == 0)
    {
        storage->data_dev = st.st_dev;
        storage->data_ino = st.st_ino;
    }
}

static int fill_legacy_storage(const char *path, myfs_storage_t *storage)
{
    memset(storage, 0, sizeof(*storage));
    int ret = copy_logical_path(storage, path);
    if (ret != 0)
        return ret;

    storage->is_legacy = true;
    strcpy(storage->generation_id, "legacy");
    build_data_path(storage->data_path, path);
    build_meta_path(storage->meta_path, path);
    if (storage->data_path[0] == '\0' || storage->meta_path[0] == '\0')
        return -ENAMETOOLONG;
    populate_data_identity(storage);
    return 0;
}

static bool valid_generation_id(const char *id)
{
    if (strlen(id) != MYFS_GENERATION_HEX_LEN)
        return false;

    for (size_t i = 0; i < MYFS_GENERATION_HEX_LEN; i++)
    {
        if (!isdigit((unsigned char)id[i]) &&
            !(id[i] >= 'a' && id[i] <= 'f'))
            return false;
    }
    return true;
}

int validate_generation_marker(const myfs_storage_t *storage)
{
    struct stat st;
    if (lstat(storage->marker_path, &st) != 0 || !S_ISREG(st.st_mode))
        return -EIO;

    char marker[sizeof(MYFS_GENERATION_MARKER_PREFIX) + MYFS_GENERATION_HEX_LEN + 1];
    int marker_fd = open(storage->marker_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (marker_fd < 0)
        return -EIO;
    ssize_t marker_len = read(marker_fd, marker, sizeof(marker));
    int marker_errno = errno;
    close(marker_fd);
    if (marker_len < 0)
    {
        errno = marker_errno;
        return -EIO;
    }
    size_t expected_len = strlen(MYFS_GENERATION_MARKER_PREFIX) +
                          MYFS_GENERATION_HEX_LEN + 1;
    if ((size_t)marker_len != expected_len ||
        memcmp(marker, MYFS_GENERATION_MARKER_PREFIX,
               strlen(MYFS_GENERATION_MARKER_PREFIX)) != 0 ||
        memcmp(marker + strlen(MYFS_GENERATION_MARKER_PREFIX),
               storage->generation_id, MYFS_GENERATION_HEX_LEN) != 0 ||
        marker[expected_len - 1] != '\n')
        return -EIO;
    return 0;
}

int build_generation_storage(const char *path, const char *id,
                             myfs_storage_t *storage)
{
    if (!valid_generation_id(id))
        return -EINVAL;

    memset(storage, 0, sizeof(*storage));
    int ret = copy_logical_path(storage, path);
    if (ret != 0)
        return ret;

    storage->is_legacy = false;
    memcpy(storage->generation_id, id, MYFS_GENERATION_HEX_LEN + 1);

    char base_path[PATH_MAX];
    build_path(base_path, path);
    if (base_path[0] == '\0' ||
        snprintf(storage->generation_dir, PATH_MAX, "%s.g.%s",
                 base_path, id) >= PATH_MAX ||
        snprintf(storage->marker_path, PATH_MAX, "%s.g.%s.owner",
                 base_path, id) >= PATH_MAX ||
        snprintf(storage->data_path, PATH_MAX, "%s/data",
                 storage->generation_dir) >= PATH_MAX ||
        snprintf(storage->meta_path, PATH_MAX, "%s/meta",
                 storage->generation_dir) >= PATH_MAX)
        return -ENAMETOOLONG;

    populate_data_identity(storage);
    return 0;
}

/* Resolve the pointer once.  A malformed/dangling pointer is corruption and
 * must never silently fall back to the legacy pair. */
int resolve_storage(const char *path, myfs_storage_t *storage)
{
    char current_path[PATH_MAX];
    char base_path[PATH_MAX];
    build_current_path(current_path, path);
    build_path(base_path, path);
    if (current_path[0] == '\0' || base_path[0] == '\0')
        return -ENAMETOOLONG;

    struct stat pointer_st;
    if (lstat(current_path, &pointer_st) != 0)
    {
        if (errno == ENOENT)
            return fill_legacy_storage(path, storage);
        return -errno;
    }
    if (!S_ISLNK(pointer_st.st_mode))
        return -EIO;

    char target[NAME_MAX + 1];
    ssize_t target_len = readlink(current_path, target, NAME_MAX);
    if (target_len < 0)
        return -errno;
    if (target_len == 0 || target_len > NAME_MAX)
        return -EIO;
    target[target_len] = '\0';
    if (strchr(target, '/') != NULL)
        return -EIO;

    const char *base_name = strrchr(base_path, '/');
    base_name = base_name ? base_name + 1 : base_path;
    size_t base_len = strlen(base_name);
    const char marker[] = ".g.";
    if ((size_t)target_len != base_len + sizeof(marker) - 1 + MYFS_GENERATION_HEX_LEN ||
        strncmp(target, base_name, base_len) != 0 ||
        strncmp(target + base_len, marker, sizeof(marker) - 1) != 0)
        return -EIO;

    const char *id = target + base_len + sizeof(marker) - 1;
    if (!valid_generation_id(id))
        return -EIO;

    int ret = build_generation_storage(path, id, storage);
    if (ret != 0)
        return ret;

    struct stat st;
    if (validate_generation_marker(storage) != 0 ||
        lstat(storage->data_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        lstat(storage->meta_path, &st) != 0 || !S_ISREG(st.st_mode))
        return -EIO;

    populate_data_identity(storage);
    return 0;
}

static int random_generation_id(char id[MYFS_GENERATION_HEX_LEN + 1])
{
    unsigned char random_bytes[MYFS_GENERATION_HEX_LEN / 2];
    size_t done = 0;
    while (done < sizeof(random_bytes))
    {
        ssize_t n = getrandom(random_bytes + done, sizeof(random_bytes) - done, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        done += (size_t)n;
    }

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random_bytes); i++)
    {
        id[i * 2] = hex[random_bytes[i] >> 4];
        id[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }
    id[MYFS_GENERATION_HEX_LEN] = '\0';
    return 0;
}

/* Called only by compact_data_file(): allocate a unique unpublished generation
 * directory and its data file. */
int create_generation_storage(const char *path, mode_t data_mode,
                              myfs_storage_t *storage)
{
    for (int attempt = 0; attempt < 32; attempt++)
    {
        char id[MYFS_GENERATION_HEX_LEN + 1];
        int ret = random_generation_id(id);
        if (ret != 0)
            return ret;
        ret = build_generation_storage(path, id, storage);
        if (ret != 0)
            return ret;

        /* The durable sibling owner record is created before the directory.
         * Therefore a crash can never leave an unowned directory that recovery
         * would need to guess about. */
        int marker_fd = open(storage->marker_path,
                             O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (marker_fd < 0)
        {
            if (errno == EEXIST)
                continue;
            return -errno;
        }
        char marker[sizeof(MYFS_GENERATION_MARKER_PREFIX) +
                    MYFS_GENERATION_HEX_LEN + 1];
        int marker_len = snprintf(marker, sizeof(marker), "%s%s\n",
                                  MYFS_GENERATION_MARKER_PREFIX, id);
        int marker_ret = 0;
        size_t marker_done = 0;
        while (marker_done < (size_t)marker_len)
        {
            ssize_t marker_written = write(marker_fd, marker + marker_done,
                                           (size_t)marker_len - marker_done);
            if (marker_written < 0)
            {
                if (errno == EINTR)
                    continue;
                marker_ret = -errno;
                break;
            }
            if (marker_written == 0)
            {
                marker_ret = -EIO;
                break;
            }
            marker_done += (size_t)marker_written;
        }
        if (marker_ret == 0 && fsync(marker_fd) != 0)
            marker_ret = -errno;
        if (close(marker_fd) != 0 && marker_ret == 0)
            marker_ret = -errno;
        if (marker_ret != 0)
        {
            unlink(storage->marker_path);
            return marker_ret;
        }

        marker_ret = fsync_parent_path(storage->marker_path);
        if (marker_ret != 0)
        {
            unlink(storage->marker_path);
            return marker_ret;
        }

        if (mkdir(storage->generation_dir, 0700) != 0)
        {
            int saved_errno = errno;
            unlink(storage->marker_path);
            fsync_parent_path(storage->marker_path);
            if (saved_errno == EEXIST)
                continue;
            return -saved_errno;
        }

        int fd = open(storage->data_path,
                      O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                      data_mode & 0777);
        if (fd < 0)
        {
            int saved_errno = errno;
            rmdir(storage->generation_dir);
            unlink(storage->marker_path);
            fsync_parent_path(storage->marker_path);
            return -saved_errno;
        }
        if (close(fd) != 0)
            return -errno;
        populate_data_identity(storage);
        return 0;
    }
    return -EEXIST;
}

int fsync_parent_path(const char *path)
{
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", path) >= (int)sizeof(parent))
        return -ENAMETOOLONG;

    char *slash = strrchr(parent, '/');
    if (!slash)
        strcpy(parent, ".");
    else if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';

    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    int ret = 0;
    if (fsync(fd) != 0)
        ret = -errno;
    if (close(fd) != 0 && ret == 0)
        ret = -errno;
    return ret;
}

int publish_generation(const char *path, const myfs_storage_t *storage)
{
    char current_path[PATH_MAX];
    char tmp_path[PATH_MAX];
    build_current_path(current_path, path);
    if (current_path[0] == '\0' ||
        snprintf(tmp_path, PATH_MAX, "%s.tmp.%s", current_path,
                 storage->generation_id) >= PATH_MAX)
        return -ENAMETOOLONG;

    const char *target = strrchr(storage->generation_dir, '/');
    target = target ? target + 1 : storage->generation_dir;

    if (symlink(target, tmp_path) != 0)
        return -errno;

    int ret = fsync_parent_path(tmp_path);
    if (ret != 0)
    {
        unlink(tmp_path);
        return ret;
    }

    if (rename(tmp_path, current_path) != 0)
    {
        ret = -errno;
        unlink(tmp_path);
        return ret;
    }

    /* This directory fsync is the durability boundary.  Old generations are
     * not eligible for GC unless this call succeeds. */
    return fsync_parent_path(current_path);
}

static int replace_with_hardlink(const char *source, const char *dest,
                                 const char *tag)
{
    struct stat source_st;
    struct stat dest_st;
    if (stat(source, &source_st) != 0)
        return -errno;
    if (stat(dest, &dest_st) == 0 &&
        source_st.st_dev == dest_st.st_dev && source_st.st_ino == dest_st.st_ino)
        return 0;

    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, PATH_MAX, "%s.alias.%s.%ld", dest, tag,
                 (long)getpid()) >= PATH_MAX)
        return -ENAMETOOLONG;

    if (unlink(tmp_path) != 0 && errno != ENOENT)
        return -errno;
    if (link(source, tmp_path) != 0)
        return -errno;
    if (rename(tmp_path, dest) != 0)
    {
        int saved_errno = errno;
        unlink(tmp_path);
        return -saved_errno;
    }
    return 0;
}

/* Preserve the legacy backing names as non-authoritative hard-link aliases so
 * existing diagnostics and benchmarks still observe the active physical files. */
int install_generation_aliases(const char *path,
                               const myfs_storage_t *storage)
{
    if (storage->is_legacy)
        return 0;

    char data_alias[PATH_MAX];
    char meta_alias[PATH_MAX];
    build_data_path(data_alias, path);
    build_meta_path(meta_alias, path);

    int ret = replace_with_hardlink(storage->data_path, data_alias, "data");
    if (ret != 0)
        return ret;
    ret = replace_with_hardlink(storage->meta_path, meta_alias, "meta");
    if (ret != 0)
        return ret;
    return fsync_parent_path(data_alias);
}

int remove_generation_storage(const myfs_storage_t *storage)
{
    int ret = 0;
    char meta_tmp_path[PATH_MAX];
    if (snprintf(meta_tmp_path, PATH_MAX, "%s.tmp", storage->meta_path) >= PATH_MAX)
        return -ENAMETOOLONG;
    if (unlink(storage->data_path) != 0 && errno != ENOENT)
        ret = -errno;
    if (unlink(storage->meta_path) != 0 && errno != ENOENT && ret == 0)
        ret = -errno;

    if (!storage->is_legacy)
    {
        if (unlink(meta_tmp_path) != 0 && errno != ENOENT && ret == 0)
            ret = -errno;
        bool directory_removed = false;
        if (rmdir(storage->generation_dir) == 0 || errno == ENOENT)
            directory_removed = true;
        else if (ret == 0)
            ret = -errno;
        if (directory_removed &&
            unlink(storage->marker_path) != 0 && errno != ENOENT && ret == 0)
            ret = -errno;
        int sync_ret = fsync_parent_path(storage->generation_dir);
        if (sync_ret != 0 && ret == 0)
            ret = sync_ret;
    }
    else
    {
        int sync_ret = fsync_parent_path(storage->data_path);
        if (sync_ret != 0 && ret == 0)
            ret = sync_ret;
    }
    return ret;
}

bool storage_generation_equal(const myfs_storage_t *a,
                              const myfs_storage_t *b)
{
    if (strcmp(a->logical_path, b->logical_path) != 0 ||
        a->is_legacy != b->is_legacy)
        return false;

    if (!a->is_legacy)
        return strcmp(a->generation_id, b->generation_id) == 0;

    if (a->data_ino != 0 && b->data_ino != 0)
        return a->data_dev == b->data_dev && a->data_ino == b->data_ino;
    return strcmp(a->data_path, b->data_path) == 0;
}
