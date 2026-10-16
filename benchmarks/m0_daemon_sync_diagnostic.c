/*
 * Isolated M0 diagnostics, with mutually exclusive link-time populations.
 * Default (or M0_DIAGNOSTIC_SYNC_CALLS=1): m0-daemon-sync-v1, fsync/fdatasync.
 * M0_DIAGNOSTIC_BACKING_IO=1: m0-daemon-backing-io-v1,
 *   -Wl,--wrap=pread,--wrap=pwrite,--wrap=read,--wrap=write,--wrap=__pread_chk
 * Never link both populations. The ordinary production build includes neither.
 * Backing mode uses M0_IO_FD/TRIAL/TOKEN/AFFINITY, with a 256-character lowercase
 * hexadecimal expected CPU mask (CPU 0 is the low bit of the rightmost digit).
 * Affinity observations precede each real-call interval; they do not establish
 * whole-thread-lifetime placement. There are no per-event file-content reads.
 *
 * Backing events are compact, explicitly ordered JSON arrays. Even full-width
 * size_t/off_t/ssize_t values and all 1024 affinity bits fit 492 bytes per line.
 * 8192*492 + 4096 header + 1024 footer = 4,035,584 < 4,194,304 bytes.
 * Native backing-event storage is sizeof(struct sync_event)*8192, emitted in the
 * header and compile-time bounded below. No average-size assumption is used.
 *
 * Brief bookkeeping disables cancellation, restoring the caller's state before
 * the real libc cancellation point. Cancellation cleanup marks an unreturned
 * operation dropped/incomplete, rather than fabricating its return or duration.
 * Cancellation type is never changed. These mutex-based diagnostics are not
 * async-signal-safe; the reviewed production call sites are not signal handlers.
 * A late destructor priority permits ordinary linked-object destructors to finish
 * first; concurrent surviving threads make the final trace invalid. Binary
 * coverage review must exclude later destructor/signal-handler wrapped calls.
 *
 * Link this object ONLY into an independently identified diagnostic executable:
 *   -Wl,--wrap=fsync,--wrap=fdatasync
 * Undefined references from linked objects are eligible; shared-library internals,
 * raw syscalls and references transformed into other symbols are not covered.
 * No ordinary production build includes this source.
 *
 * Explicit activation requires M0_SYNC_FD, M0_SYNC_TRIAL and M0_SYNC_TOKEN. The
 * runner exclusively creates a private regular artifact and passes its writable
 * descriptor. Both identifiers are 32 lowercase hexadecimal characters. This
 * constructor validates the descriptor, clears the environment handoff and sets
 * close-on-exec; an atfork child handler closes the writer and disables recording
 * in fork descendants. It opens no artifact paths. Invalid activation leaves no valid
 * receipt; the runner must invalidate that diagnostic without changing sync.
 *
 * Completed events occupy a fixed 8192-entry array (sizeof(event)*8192 bytes).
 * Mutex bookkeeping occurs outside the real call; the mutex is never held across
 * synchronization. There is no per-event filesystem I/O, allocation or pathname
 * lookup. Event order is completion-record order, not invocation order. Recording
 * changes scheduling and CPU usage; elapsed call duration includes kernel work,
 * blocking and scheduling, and is not application waiting or disk service time.
 *
 * A normal-exit destructor serializes JSONL header, events and footer directly to
 * the inherited descriptor, with a strict 4 MiB emitted-byte cap. It performs no
 * fsync/fdatasync of diagnostic data. Incomplete/failed writes retain partial
 * evidence. Abrupt termination can leave an empty file. The independent reader
 * must require normal owned-process exit, matching identities, a complete footer,
 * consistent counts, no errors, no overflow and intact bytes. The recorder checks
 * that only the finalizing thread remains: active process threads or in-flight
 * wrapped operations make evidence incomplete. No diagnostic error changes the
 * real operation's return value or errno.
 *
 * JSONL integers use native values rendered as decimal, independent of byte order.
 * Header module_base is the ELF image base from dladdr; event caller is the raw
 * return address (subtract the base, then symbolize against the pinned Build ID).
 * This is caller evidence, not FD-to-path/type identification. File descriptors
 * can be reused. A missing module base is explicit and invalid for this protocol.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(M0_DIAGNOSTIC_BACKING_IO) && defined(M0_DIAGNOSTIC_SYNC_CALLS)
#error "select exactly one M0 diagnostic build mode"
#endif
#if defined(M0_DIAGNOSTIC_BACKING_IO) && M0_DIAGNOSTIC_BACKING_IO != 1
#error "M0_DIAGNOSTIC_BACKING_IO must be 1"
#endif

#define M0_IO_EVENT_LINE_MAX 492U
#define M0_IO_HEADER_MAX 4096U
#define M0_IO_FOOTER_MAX 1024U
#define M0_SYNC_MAX_EVENTS 8192U
#define M0_SYNC_MAX_BYTES (4U * 1024U * 1024U)
#define M0_SYNC_FOOTER_RESERVE 1024U

#ifdef M0_DIAGNOSTIC_BACKING_IO
extern ssize_t __real_pread(int, void *, size_t, off_t);
extern ssize_t __real_pwrite(int, const void *, size_t, off_t);
extern ssize_t __real_read(int, void *, size_t);
extern ssize_t __real_write(int, const void *, size_t);
/* /usr/include/.../bits/unistd-decl.h: preserve the fortified fifth argument. */
extern ssize_t __real___pread_chk(int, void *, size_t, off_t, size_t);
ssize_t __wrap_pread(int, void *, size_t, off_t);
#define M0_SCHEMA "m0-daemon-backing-io-v1"
#define M0_ENV_FD "M0_IO_FD"
#define M0_ENV_TRIAL "M0_IO_TRIAL"
#define M0_ENV_TOKEN "M0_IO_TOKEN"
#else
#define M0_SCHEMA "m0-daemon-sync-v1"
#define M0_ENV_FD "M0_SYNC_FD"
#define M0_ENV_TRIAL "M0_SYNC_TRIAL"
#define M0_ENV_TOKEN "M0_SYNC_TOKEN"
extern int __real_fsync(int fd);
extern int __real_fdatasync(int fd);
int __wrap_fsync(int fd);
int __wrap_fdatasync(int fd);
#endif

struct sync_event {
    uint64_t sequence, tid, caller, entry_ns, exit_ns;
#ifdef M0_DIAGNOSTIC_BACKING_IO
    uint64_t requested, buffer_size, affinity_ns;
    int64_t result, offset;
    cpu_set_t affinity;
    int fd, operation, saved_errno, clock_error, affinity_status, affinity_errno;
#else
    int fd, operation, result, saved_errno, clock_error;
#endif
};

static struct sync_event events[M0_SYNC_MAX_EVENTS];
#ifdef M0_DIAGNOSTIC_BACKING_IO
_Static_assert(CPU_SETSIZE == 1024 && sizeof(cpu_set_t) == 128, "review affinity encoding on this ABI");
_Static_assert(sizeof(ssize_t) <= 8 && sizeof(off_t) <= 8 && sizeof(size_t) <= 8 &&
               sizeof(int) == 4 && sizeof(pid_t) == 4, "review event integer bounds on this ABI");
_Static_assert(M0_SYNC_MAX_EVENTS * M0_IO_EVENT_LINE_MAX + M0_IO_HEADER_MAX +
               M0_IO_FOOTER_MAX <= M0_SYNC_MAX_BYTES, "worst case trace exceeds cap");
_Static_assert(sizeof(events) <= 2U * 1024U * 1024U, "review bounded recorder memory");
static cpu_set_t expected_affinity;
static char expected_affinity_hex[257];
static uint64_t canceled, affinity_errors;

static void affinity_hex(const cpu_set_t *mask, char text[257]) {
    static const char digits[] = "0123456789abcdef";
    for (int nibble = 0; nibble < 256; ++nibble) {
        unsigned value = 0;
        for (int bit = 0; bit < 4; ++bit)
            if (CPU_ISSET(nibble * 4 + bit, mask)) value |= 1U << bit;
        text[255 - nibble] = digits[value];
    }
    text[256] = 0;
}

static int parse_affinity(const char *text) {
    if (!text || strlen(text) != 256) return -1;
    CPU_ZERO(&expected_affinity);
    for (int i = 0; i < 256; ++i) {
        int digit = text[255-i];
        int value = digit >= '0' && digit <= '9' ? digit-'0' :
                    digit >= 'a' && digit <= 'f' ? digit-'a'+10 : -1;
        if (value < 0) return -1;
        for (int bit = 0; bit < 4; ++bit)
            if (value & (1 << bit)) CPU_SET(i*4+bit, &expected_affinity);
    }
    if (!CPU_COUNT(&expected_affinity)) return -1;
    memcpy(expected_affinity_hex, text, 257);
    return 0;
}
#endif
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t observed, recorded, dropped, inflight, clock_errors;
static uint64_t recording_errors, enabled_ns, module_base;
static pid_t process_id;
static int artifact_fd = -1, enabled, finalizing, overflow;
static char trial_id[33], token[33];
static size_t emitted;

/* Only async-signal-safe operations after fork; do not touch the inherited mutex. */
static void sync_diagnostic_fork_child(void) {
    int previous_errno = errno;
    enabled = 0;
    if (artifact_fd >= 0)
        (void)close(artifact_fd);
    artifact_fd = -1;
    errno = previous_errno;
}

static int monotonic_ns(uint64_t *value) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0)
        return -1;
    *value = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return 0;
}

static int hex_identity(const char *value) {
    if (!value || strlen(value) != 32)
        return 0;
    for (size_t i = 0; i < 32; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return 0;
    return 1;
}

#ifdef M0_DIAGNOSTIC_BACKING_IO
__attribute__((constructor(101)))
#else
__attribute__((constructor))
#endif
static void sync_diagnostic_init(void) {
    int previous_errno = errno;
    const char *fd_text = getenv(M0_ENV_FD);
    const char *trial = getenv(M0_ENV_TRIAL);
    const char *secret = getenv(M0_ENV_TOKEN);
    char *end = NULL;
    struct stat info;
    Dl_info module;
    if (!fd_text && !trial && !secret)
        goto done;
    if (!fd_text)
        goto clear;
    errno = 0;
    long descriptor = strtol(fd_text, &end, 10);
    if (errno || !*fd_text || !end || *end || descriptor < 3 || descriptor > INT32_MAX)
        goto clear;
    int fd = (int)descriptor;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || ((flags & O_ACCMODE) != O_WRONLY &&
                      (flags & O_ACCMODE) != O_RDWR) ||
        (flags & (O_APPEND | O_NONBLOCK)) || fstat(fd, &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 07777) != 0600 || info.st_nlink != 1 || info.st_size != 0 ||
        lseek(fd, 0, SEEK_CUR) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0)
        goto clear;
    artifact_fd = fd;
    if (pthread_atfork(NULL, NULL, sync_diagnostic_fork_child) != 0)
        ++recording_errors;
    if (!hex_identity(trial) || !hex_identity(secret))
        goto clear;
#ifdef M0_DIAGNOSTIC_BACKING_IO
    if (parse_affinity(getenv("M0_IO_AFFINITY")) != 0)
        goto clear;
#endif
    memcpy(trial_id, trial, 33);
    memcpy(token, secret, 33);
    process_id = getpid();
    if (monotonic_ns(&enabled_ns) != 0)
        ++clock_errors;
    memset(&module, 0, sizeof(module));
#ifdef M0_DIAGNOSTIC_BACKING_IO
    if (dladdr((void *)(uintptr_t)&__wrap_pread, &module) && module.dli_fbase)
#else
    if (dladdr((void *)(uintptr_t)&__wrap_fsync, &module) && module.dli_fbase)
#endif
        module_base = (uint64_t)(uintptr_t)module.dli_fbase;
    else
        ++recording_errors;
    enabled = 1;
clear:
    unsetenv(M0_ENV_FD);
    unsetenv(M0_ENV_TRIAL);
    unsetenv(M0_ENV_TOKEN);
#ifdef M0_DIAGNOSTIC_BACKING_IO
    unsetenv("M0_IO_AFFINITY");
#endif
done:
    errno = previous_errno;
}

#ifndef M0_DIAGNOSTIC_BACKING_IO
/* Both public wrappers capture their own caller before entering this helper. */
static int wrapped_sync(int fd, int operation, uint64_t caller) {
    if (!enabled || getpid() != process_id)
        return operation ? __real_fdatasync(fd) : __real_fsync(fd);
    int incoming_errno = errno;
    struct sync_event event = {0};
    event.fd = fd;
    event.operation = operation;
    event.caller = caller;
    event.tid = (uint64_t)syscall(SYS_gettid);
    pthread_mutex_lock(&event_mutex);
    ++observed;
    ++inflight;
    pthread_mutex_unlock(&event_mutex);
    event.clock_error = monotonic_ns(&event.entry_ns) != 0;
    errno = incoming_errno;
    int result = operation ? __real_fdatasync(fd) : __real_fsync(fd);
    int real_errno = errno; /* Preserve immediately, before clock/recording calls. */
    event.clock_error |= monotonic_ns(&event.exit_ns) != 0;
    event.result = result;
    event.saved_errno = real_errno;
    pthread_mutex_lock(&event_mutex);
    --inflight;
    if (event.clock_error || event.exit_ns < event.entry_ns)
        ++clock_errors;
    if (finalizing || getpid() != process_id) {
        ++recording_errors;
        ++dropped;
    } else if (recorded == M0_SYNC_MAX_EVENTS) {
        overflow = 1;
        ++dropped;
    } else {
        event.sequence = recorded + 1;
        events[recorded++] = event;
    }
    pthread_mutex_unlock(&event_mutex);
    errno = real_errno;
    return result;
}

int __wrap_fsync(int fd) {
    return wrapped_sync(fd, 0, (uint64_t)(uintptr_t)__builtin_extract_return_addr(
        __builtin_return_address(0)));
}

int __wrap_fdatasync(int fd) {
    return wrapped_sync(fd, 1, (uint64_t)(uintptr_t)__builtin_extract_return_addr(
        __builtin_return_address(0)));
}

#else
/* Cancellation handlers run with cancellation disabled and no recorder mutex
 * held by the caller. An interrupted libc attempt has no fabricated result. */
static void canceled_io(void *unused) {
    (void)unused;
    int previous_errno = errno, previous_state;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_state);
    pthread_mutex_lock(&event_mutex);
    --inflight;
    ++dropped;
    ++canceled;
    ++recording_errors;
    pthread_mutex_unlock(&event_mutex);
    errno = previous_errno;
}

static ssize_t real_io(int operation, int fd, void *buffer, size_t requested,
                       off_t offset, size_t buffer_size) {
    switch (operation) {
    case 0: return __real_pread(fd, buffer, requested, offset);
    case 1: return __real_pwrite(fd, buffer, requested, offset);
    case 2: return __real_read(fd, buffer, requested);
    case 3: return __real_write(fd, buffer, requested);
    default: return __real___pread_chk(fd, buffer, requested, offset, buffer_size);
    }
}

static ssize_t wrapped_io(int operation, int fd, void *buffer, size_t requested,
                          off_t offset, size_t buffer_size, uint64_t caller) {
    if (!enabled || getpid() != process_id)
        return real_io(operation, fd, buffer, requested, offset, buffer_size);
    int incoming_errno = errno, previous_state;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_state);
    struct sync_event event = {0};
    event.fd = fd;
    event.operation = operation;
    event.requested = requested;
    event.offset = offset;
    event.buffer_size = buffer_size;
    event.caller = caller;
    event.tid = (uint64_t)syscall(SYS_gettid);
    CPU_ZERO(&event.affinity);
    if (sched_getaffinity(0, sizeof(event.affinity), &event.affinity) != 0) {
        event.affinity_status = 2;
        event.affinity_errno = errno;
    } else {
        event.affinity_status = CPU_EQUAL(&event.affinity, &expected_affinity) ? 0 : 1;
    }
    event.clock_error = monotonic_ns(&event.affinity_ns) != 0;
    pthread_mutex_lock(&event_mutex);
    ++observed;
    ++inflight;
    if (event.affinity_status) ++affinity_errors;
    pthread_mutex_unlock(&event_mutex);
    /* Install cleanup before restoring cancellation; a pending cancellation
       may take effect immediately on restoration, before the real call. */
    pthread_cleanup_push(canceled_io, NULL);
    (void)pthread_setcancelstate(previous_state, NULL);
    event.clock_error |= monotonic_ns(&event.entry_ns) != 0;
    errno = incoming_errno;
    event.result = real_io(operation, fd, buffer, requested, offset, buffer_size);
    event.saved_errno = errno;
    event.clock_error |= monotonic_ns(&event.exit_ns) != 0;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_lock(&event_mutex);
    --inflight;
    if (event.clock_error || event.exit_ns < event.entry_ns || event.entry_ns < event.affinity_ns)
        ++clock_errors;
    if (finalizing || getpid() != process_id) {
        ++recording_errors;
        ++dropped;
    } else if (recorded == M0_SYNC_MAX_EVENTS) {
        overflow = 1;
        ++dropped;
    } else {
        event.sequence = recorded + 1;
        events[recorded++] = event;
    }
    pthread_mutex_unlock(&event_mutex);
    pthread_cleanup_pop(0);
    (void)pthread_setcancelstate(previous_state, NULL);
    errno = event.saved_errno;
    return (ssize_t)event.result;
}

#define CALLER ((uint64_t)(uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))
ssize_t __wrap_pread(int fd, void *buffer, size_t count, off_t offset) {
    return wrapped_io(0, fd, buffer, count, offset, 0, CALLER);
}
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t count, off_t offset) {
    return wrapped_io(1, fd, (void *)buffer, count, offset, 0, CALLER);
}
ssize_t __wrap_read(int fd, void *buffer, size_t count) {
    return wrapped_io(2, fd, buffer, count, 0, 0, CALLER);
}
ssize_t __wrap_write(int fd, const void *buffer, size_t count) {
    return wrapped_io(3, fd, (void *)buffer, count, 0, 0, CALLER);
}
ssize_t __wrap___pread_chk(int fd, void *buffer, size_t count, off_t offset, size_t buffer_size) {
    return wrapped_io(4, fd, buffer, count, offset, buffer_size, CALLER);
}
#undef CALLER
#endif

static int emit_line(const char *line, size_t length, int footer) {
    size_t limit = footer ? M0_SYNC_MAX_BYTES : M0_SYNC_MAX_BYTES - M0_SYNC_FOOTER_RESERVE;
    if (length > limit || emitted > limit - length) {
        overflow = 1;
        ++recording_errors;
        return -1;
    }
    size_t offset = 0;
    unsigned interrupted = 0;
    while (offset < length) {
        /* GNU --wrap only resolves __real_write in the backing-I/O build.
           Sync-only linkage deliberately uses ordinary write, not __real_write. */
#ifdef M0_DIAGNOSTIC_BACKING_IO
        ssize_t count = __real_write(artifact_fd, line + offset, length - offset);
#else
        ssize_t count = write(artifact_fd, line + offset, length - offset);
#endif
        if (count < 0 && errno == EINTR && ++interrupted <= 16)
            continue;
        if (count <= 0 || (size_t)count > length - offset) {
            ++recording_errors;
            return -1;
        }
        offset += (size_t)count;
        emitted += (size_t)count;
    }
    return 0;
}

static int remaining_threads(void) {
    DIR *directory = opendir("/proc/self/task");
    if (!directory)
        return -1;
    int count = 0;
    struct dirent *item;
    int scan_error;
    for (;;) {
        errno = 0;
        item = readdir(directory);
        if (!item)
            break;
        if (item->d_name[0] >= '0' && item->d_name[0] <= '9')
            ++count;
    }
    scan_error = errno;
    if (closedir(directory) != 0 || scan_error)
        return -1;
    return count;
}

#ifdef M0_DIAGNOSTIC_BACKING_IO
__attribute__((destructor(101)))
#else
__attribute__((destructor))
#endif
static void sync_diagnostic_finish(void) {
    if (!enabled || getpid() != process_id)
        return;
    int previous_errno = errno, previous_cancel_state;
    (void)pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_cancel_state);
    int threads = remaining_threads();
    pthread_mutex_lock(&event_mutex);
    finalizing = 1;
    if (threads != 1)
        ++recording_errors;
    uint64_t finished_ns = 0, written = 0;
    if (monotonic_ns(&finished_ns) != 0)
        ++clock_errors;
    char line[4096];
#ifndef M0_DIAGNOSTIC_BACKING_IO
    int length = snprintf(line, sizeof(line),
        "{\"kind\":\"header\",\"schema\":\"m0-daemon-sync-v1\",\"trial_id\":\"%s\","
        "\"token\":\"%s\",\"pid\":%ld,\"clock\":\"CLOCK_MONOTONIC\","
        "\"unit\":\"nanoseconds\",\"module_base\":%" PRIu64 ","
        "\"caller_source\":\"__builtin_return_address\",\"max_bytes\":%u,"
        "\"max_events\":%u,\"event_memory_bytes\":%zu,\"enabled_monotonic_ns\":%" PRIu64 "}\n",
        trial_id, token, (long)process_id, module_base, M0_SYNC_MAX_BYTES,
        M0_SYNC_MAX_EVENTS, sizeof(events), enabled_ns);
#else
    int length = snprintf(line, sizeof(line),
        "{\"kind\":\"header\",\"schema\":\"m0-daemon-backing-io-v1\",\"mode\":\"backing-io\","
        "\"trial_id\":\"%s\",\"token\":\"%s\",\"pid\":%ld,\"clock\":\"CLOCK_MONOTONIC\","
        "\"unit\":\"nanoseconds\",\"module_base\":%" PRIu64 ",\"caller_source\":\"__builtin_return_address\","
        "\"max_bytes\":%u,\"max_events\":%u,\"event_memory_bytes\":%zu,\"event_size_bytes\":%zu,"
        "\"max_event_line_bytes\":%u,\"max_serialized_bytes\":%u,\"affinity_bits\":1024,"
        "\"expected_affinity\":\"%s\",\"symbols\":[\"pread\",\"pwrite\",\"read\",\"write\",\"__pread_chk\"],"
        "\"event_fields\":[\"sequence\",\"tid\",\"symbol\",\"fd\",\"caller\",\"entry_ns\",\"exit_ns\","
        "\"return_value\",\"errno\",\"requested_bytes\",\"offset\",\"buffer_size\",\"affinity_ns\","
        "\"affinity_status\",\"affinity_errno\",\"clock_error\",\"affinity\"],"
        "\"enabled_monotonic_ns\":%" PRIu64 "}\n",
        trial_id, token, (long)process_id, module_base, M0_SYNC_MAX_BYTES, M0_SYNC_MAX_EVENTS,
        sizeof(events), sizeof(struct sync_event), M0_IO_EVENT_LINE_MAX,
        M0_SYNC_MAX_EVENTS*M0_IO_EVENT_LINE_MAX+M0_IO_HEADER_MAX+M0_IO_FOOTER_MAX,
        expected_affinity_hex, enabled_ns);
#endif
    if (length < 0 || (size_t)length >= sizeof(line) || emit_line(line, (size_t)length, 0))
        goto done;
    for (uint64_t i = 0; i < recorded; ++i) {
        const struct sync_event *event = &events[i];
        #ifndef M0_DIAGNOSTIC_BACKING_IO
        length = snprintf(line, sizeof(line),
            "{\"kind\":\"event\",\"sequence\":%" PRIu64 ",\"pid\":%ld,"
            "\"tid\":%" PRIu64 ",\"operation\":\"%s\",\"fd\":%d,"
            "\"caller\":%" PRIu64 ",\"entry_ns\":%" PRIu64 ",\"exit_ns\":%" PRIu64 ","
            "\"return_value\":%d,\"errno\":%d,\"clock_error\":%s}\n",
            event->sequence, (long)process_id, event->tid,
            event->operation ? "fdatasync" : "fsync", event->fd, event->caller,
            event->entry_ns, event->exit_ns, event->result, event->saved_errno,
            event->clock_error ? "true" : "false");
#else
        char affinity[257], affinity_json[260], offset[32], buffer_size[32];
        if (event->affinity_status == 2) {
            memcpy(affinity_json, "null", 5);
        } else {
            affinity_hex(&event->affinity, affinity);
            snprintf(affinity_json, sizeof(affinity_json), "\"%s\"", affinity);
        }
        if (event->operation == 2 || event->operation == 3) memcpy(offset, "null", 5);
        else snprintf(offset, sizeof(offset), "%" PRId64, event->offset);
        if (event->operation == 4) snprintf(buffer_size, sizeof(buffer_size), "%" PRIu64, event->buffer_size);
        else memcpy(buffer_size, "null", 5);
        length = snprintf(line, sizeof(line),
            "{\"e\":[%" PRIu64 ",%" PRIu64 ",%d,%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%" PRId64 ",%d,%" PRIu64 ",%s,%s,%" PRIu64 ",%d,%d,%d,%s]}\n",
            event->sequence, event->tid, event->operation, event->fd, event->caller,
            event->entry_ns, event->exit_ns, event->result, event->saved_errno, event->requested,
            offset, buffer_size, event->affinity_ns, event->affinity_status,
            event->affinity_errno, !!event->clock_error, affinity_json);
        if (length > (int)M0_IO_EVENT_LINE_MAX) { ++recording_errors; break; }
#endif
        if (length < 0 || (size_t)length >= sizeof(line)) {
            ++recording_errors;
            break;
        }
        if (emit_line(line, (size_t)length, 0))
            break;
        ++written;
    }
#ifdef M0_DIAGNOSTIC_BACKING_IO
    if (affinity_errors) ++recording_errors;
#endif
    length = snprintf(line, sizeof(line),
        "{\"kind\":\"footer\",\"trial_id\":\"%s\",\"token\":\"%s\",\"pid\":%ld,"
        "\"observed\":%" PRIu64 ",\"written\":%" PRIu64 ",\"dropped\":%" PRIu64 ","
        "\"inflight\":%" PRIu64 ",\"clock_errors\":%" PRIu64 ","
        "\"recording_errors\":%" PRIu64 ",\"overflow\":%s,\"live_threads\":%d,"
#ifdef M0_DIAGNOSTIC_BACKING_IO
        "\"canceled\":%" PRIu64 ",\"affinity_errors\":%" PRIu64 ","
#endif
        "\"finalized_monotonic_ns\":%" PRIu64 ",\"complete\":%s}\n",
        trial_id, token, (long)process_id, observed, written, dropped + recorded - written,
        inflight, clock_errors, recording_errors, overflow ? "true" : "false", threads,
#ifdef M0_DIAGNOSTIC_BACKING_IO
        canceled, affinity_errors,
#endif
        finished_ns, (!inflight && !dropped && observed == written &&
                     !clock_errors && !recording_errors && !overflow) ? "true" : "false");
    if (length > 0 && (size_t)length < M0_SYNC_FOOTER_RESERVE)
        (void)emit_line(line, (size_t)length, 1);
done:
    (void)close(artifact_fd);
    pthread_mutex_unlock(&event_mutex);
    (void)pthread_setcancelstate(previous_cancel_state, NULL);
    errno = previous_errno;
}
