/*
 * Benchmark-only frozen fixture from
 * add02903ade010764c2f6a0313fc0793744c62cc:src/core/lock.c.
 *
 * Only private type and exported symbol names differ so this implementation
 * can be linked beside the production implementation in one benchmark.
 */
#include "myfs.h"

struct linear_file_lock
{
    char path[PATH_MAX];
    pthread_mutex_t mu;
    unsigned refs;
    struct linear_file_lock *next;
};

static pthread_mutex_t linear_lock_table_mu = PTHREAD_MUTEX_INITIALIZER;
static struct linear_file_lock *linear_lock_table;

void *linear_lock_file(const char *path)
{
    pthread_mutex_lock(&linear_lock_table_mu);
    struct linear_file_lock *lk = linear_lock_table;
    while (lk && strcmp(lk->path, path) != 0)
        lk = lk->next;
    if (!lk)
    {
        lk = calloc(1, sizeof(*lk));
        if (!lk)
        {
            pthread_mutex_unlock(&linear_lock_table_mu);
            return NULL;
        }
        if (snprintf(lk->path, sizeof(lk->path), "%s", path)
            >= (int)sizeof(lk->path))
        {
            pthread_mutex_unlock(&linear_lock_table_mu);
            free(lk);
            return NULL;
        }
        pthread_mutex_init(&lk->mu, NULL);
        lk->next = linear_lock_table;
        linear_lock_table = lk;
    }
    lk->refs++;
    pthread_mutex_unlock(&linear_lock_table_mu);

    pthread_mutex_lock(&lk->mu);
    return lk;
}

void linear_unlock_file(void *opaque_lock)
{
    struct linear_file_lock *lk = opaque_lock;
    if (!lk)
        return;
    pthread_mutex_unlock(&lk->mu);

    pthread_mutex_lock(&linear_lock_table_mu);
    if (--lk->refs == 0)
    {
        struct linear_file_lock **cursor = &linear_lock_table;
        while (*cursor && *cursor != lk)
            cursor = &(*cursor)->next;
        if (*cursor == lk)
            *cursor = lk->next;
        pthread_mutex_destroy(&lk->mu);
        free(lk);
    }
    pthread_mutex_unlock(&linear_lock_table_mu);
}

void linear_destroy_lock_table(void)
{
    pthread_mutex_lock(&linear_lock_table_mu);
    struct linear_file_lock *lk = linear_lock_table;
    while (lk)
    {
        struct linear_file_lock *next = lk->next;
        pthread_mutex_destroy(&lk->mu);
        free(lk);
        lk = next;
    }
    linear_lock_table = NULL;
    pthread_mutex_unlock(&linear_lock_table_mu);
}
