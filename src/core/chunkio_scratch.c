#include "chunkio_scratch.h"

#include "myfs.h"

typedef struct
{
    char *buffer;
    size_t capacity;
    bool in_use;
#ifdef MYFS_TEST_FAILPOINTS
    size_t growth_count;
    size_t temporary_acquisition_count;
#endif
} myfs_chunkio_scratch_slot_t;

typedef struct
{
    myfs_chunkio_scratch_slot_t slots[MYFS_CHUNKIO_SCRATCH_ROLE_COUNT];
} myfs_chunkio_scratch_context_t;

static pthread_once_t chunkio_scratch_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t chunkio_scratch_key;
static int chunkio_scratch_key_status = EAGAIN;

#ifdef MYFS_TEST_FAILPOINTS
static size_t chunkio_scratch_test_destroyed;
static _Thread_local bool chunkio_scratch_test_tls_unavailable;
static _Thread_local bool chunkio_scratch_test_growth_failure;
#endif

static bool role_valid(myfs_chunkio_scratch_role_t role)
{
    return role >= MYFS_CHUNKIO_SCRATCH_RAW &&
           role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT;
}

size_t myfs_chunkio_scratch_retained_limit(
    myfs_chunkio_scratch_role_t role)
{
    if (!role_valid(role))
        return 0;
    if (role == MYFS_CHUNKIO_SCRATCH_COMP)
        return ZSTD_COMPRESSBOUND(MYFS_MAX_WINDOW_SIZE);
    return MYFS_MAX_WINDOW_SIZE;
}

static void destroy_thread_context(void *value)
{
    myfs_chunkio_scratch_context_t *context = value;
    if (!context)
        return;

    for (size_t role = 0; role < MYFS_CHUNKIO_SCRATCH_ROLE_COUNT; role++)
        free(context->slots[role].buffer);
    free(context);
#ifdef MYFS_TEST_FAILPOINTS
    (void)__atomic_add_fetch(&chunkio_scratch_test_destroyed, 1,
                             __ATOMIC_RELAXED);
#endif
}

static void create_context_key(void)
{
    chunkio_scratch_key_status =
        pthread_key_create(&chunkio_scratch_key, destroy_thread_context);
}

static bool ensure_context_key(void)
{
    return pthread_once(&chunkio_scratch_key_once, create_context_key) == 0 &&
           chunkio_scratch_key_status == 0;
}

#ifdef MYFS_TEST_FAILPOINTS
static myfs_chunkio_scratch_context_t *peek_thread_context(void)
{
    if (!ensure_context_key())
        return NULL;
    return pthread_getspecific(chunkio_scratch_key);
}
#endif

static myfs_chunkio_scratch_context_t *get_thread_context(void)
{
    if (!ensure_context_key())
        return NULL;

    myfs_chunkio_scratch_context_t *context =
        pthread_getspecific(chunkio_scratch_key);
    if (context)
        return context;

    context = calloc(1, sizeof(*context));
    if (!context)
        return NULL;
    if (pthread_setspecific(chunkio_scratch_key, context) != 0)
    {
        free(context);
        return NULL;
    }
    return context;
}

static void record_temporary_acquisition(
    myfs_chunkio_scratch_context_t *context,
    myfs_chunkio_scratch_role_t role)
{
#ifdef MYFS_TEST_FAILPOINTS
    if (context)
        context->slots[role].temporary_acquisition_count++;
#else
    (void)context;
    (void)role;
#endif
}

static int acquire_temporary(myfs_chunkio_scratch_context_t *context,
                             myfs_chunkio_scratch_role_t role,
                             size_t required_size,
                             myfs_chunkio_scratch_lease_t *lease)
{
    size_t allocation_size = required_size == 0 ? 1 : required_size;
    char *buffer = malloc(allocation_size);
    if (!buffer)
        return -ENOMEM;

    record_temporary_acquisition(context, role);
    lease->data = buffer;
    lease->capacity = allocation_size;
    lease->retained_slot = NULL;
    return 0;
}

int myfs_chunkio_scratch_acquire(myfs_chunkio_scratch_role_t role,
                                 size_t required_size,
                                 myfs_chunkio_scratch_lease_t *lease)
{
    if (!lease || !role_valid(role))
        return -EINVAL;
    *lease = (myfs_chunkio_scratch_lease_t){0};

    myfs_chunkio_scratch_context_t *context;
#ifdef MYFS_TEST_FAILPOINTS
    if (chunkio_scratch_test_tls_unavailable)
        context = peek_thread_context();
    else
#endif
        context = get_thread_context();

#ifdef MYFS_TEST_FAILPOINTS
    if (chunkio_scratch_test_tls_unavailable)
        return acquire_temporary(context, role, required_size, lease);
#endif
    if (!context ||
        required_size > myfs_chunkio_scratch_retained_limit(role))
        return acquire_temporary(context, role, required_size, lease);

    myfs_chunkio_scratch_slot_t *slot = &context->slots[role];
    if (slot->in_use)
        return acquire_temporary(context, role, required_size, lease);

    size_t allocation_size = required_size == 0 ? 1 : required_size;
    if (slot->capacity < allocation_size)
    {
        char *grown = NULL;
#ifdef MYFS_TEST_FAILPOINTS
        if (chunkio_scratch_test_growth_failure)
            chunkio_scratch_test_growth_failure = false;
        else
#endif
            grown = realloc(slot->buffer, allocation_size);

        if (!grown)
            return acquire_temporary(context, role, required_size, lease);
        slot->buffer = grown;
        slot->capacity = allocation_size;
#ifdef MYFS_TEST_FAILPOINTS
        slot->growth_count++;
#endif
    }

    slot->in_use = true;
    lease->data = slot->buffer;
    lease->capacity = slot->capacity;
    lease->retained_slot = slot;
    return 0;
}

void myfs_chunkio_scratch_release(myfs_chunkio_scratch_lease_t *lease)
{
    if (!lease || !lease->data)
        return;

    if (lease->retained_slot)
    {
        myfs_chunkio_scratch_slot_t *slot = lease->retained_slot;
        slot->in_use = false;
    }
    else
    {
        free(lease->data);
    }
    *lease = (myfs_chunkio_scratch_lease_t){0};
}

#ifdef MYFS_TEST_FAILPOINTS
int myfs_chunkio_scratch_test_snapshot(
    myfs_chunkio_scratch_role_t role,
    myfs_chunkio_scratch_snapshot_t *snapshot)
{
    if (!snapshot || !role_valid(role))
        return -EINVAL;
    *snapshot = (myfs_chunkio_scratch_snapshot_t){0};

    myfs_chunkio_scratch_context_t *context = peek_thread_context();
    if (!context)
        return 0;
    myfs_chunkio_scratch_slot_t *slot = &context->slots[role];
    snapshot->retained_address = slot->buffer;
    snapshot->retained_capacity = slot->capacity;
    snapshot->in_use = slot->in_use;
    snapshot->growth_count = slot->growth_count;
    snapshot->temporary_acquisition_count =
        slot->temporary_acquisition_count;
    return 0;
}

size_t myfs_chunkio_scratch_test_destroy_count(void)
{
    return __atomic_load_n(&chunkio_scratch_test_destroyed,
                           __ATOMIC_RELAXED);
}

void myfs_chunkio_scratch_test_force_tls_unavailable(bool unavailable)
{
    chunkio_scratch_test_tls_unavailable = unavailable;
}

void myfs_chunkio_scratch_test_fail_next_growth(void)
{
    chunkio_scratch_test_growth_failure = true;
}
#endif
