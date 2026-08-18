#include "myfs.h"

/*
 * lock.c — per-file locking.
 * Mỗi path logic có một mutex riêng, cấp phát theo nhu cầu và thu hồi khi
 * không còn ai giữ. Thao tác trên các file khác nhau chạy song song; thao
 * tác trên cùng một file vẫn serialize như trước. Bảng lock được bảo vệ
 * bằng một mutex toàn cục nhưng chỉ giữ trong lúc tra cứu/cập nhật refcount
 * — không giữ trong suốt thao tác I/O.
 *
 * Thứ tự lock của toàn hệ thống (không bao giờ đảo ngược):
 *   file lock → registry mutex (compact.c) / queue mutex (compact.c)
 */

struct myfs_file_lock
{
    char path[PATH_MAX];
    pthread_mutex_t mu;
    unsigned refs;
    struct myfs_file_lock *next;
};

static pthread_mutex_t lock_table_mu = PTHREAD_MUTEX_INITIALIZER;
static struct myfs_file_lock *lock_table;

myfs_file_lock_t *myfs_lock_file(const char *path)
{
    pthread_mutex_lock(&lock_table_mu);
    struct myfs_file_lock *lk = lock_table;
    while (lk && strcmp(lk->path, path) != 0)
        lk = lk->next;
    if (!lk)
    {
        lk = calloc(1, sizeof(*lk));
        if (!lk)
        {
            pthread_mutex_unlock(&lock_table_mu);
            return NULL;
        }
        if (snprintf(lk->path, sizeof(lk->path), "%s", path)
            >= (int)sizeof(lk->path))
        {
            pthread_mutex_unlock(&lock_table_mu);
            free(lk);
            return NULL;
        }
        pthread_mutex_init(&lk->mu, NULL);
        lk->next = lock_table;
        lock_table = lk;
    }
    lk->refs++;
    pthread_mutex_unlock(&lock_table_mu);

    pthread_mutex_lock(&lk->mu);
    return lk;
}

void myfs_unlock_file(myfs_file_lock_t *lk)
{
    if (!lk)
        return;
    pthread_mutex_unlock(&lk->mu);

    pthread_mutex_lock(&lock_table_mu);
    if (--lk->refs == 0)
    {
        struct myfs_file_lock **cursor = &lock_table;
        while (*cursor && *cursor != lk)
            cursor = &(*cursor)->next;
        if (*cursor == lk)
            *cursor = lk->next;
        pthread_mutex_destroy(&lk->mu);
        free(lk);
    }
    pthread_mutex_unlock(&lock_table_mu);
}

void destroy_lock_table(void)
{
    pthread_mutex_lock(&lock_table_mu);
    struct myfs_file_lock *lk = lock_table;
    while (lk)
    {
        struct myfs_file_lock *next = lk->next;
        pthread_mutex_destroy(&lk->mu);
        free(lk);
        lk = next;
    }
    lock_table = NULL;
    pthread_mutex_unlock(&lock_table_mu);
}
