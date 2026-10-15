/*
 * Isolated M0 sync-call diagnostic, schema m0-daemon-sync-v1.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define M0_SYNC_MAX_EVENTS 8192U
#define M0_SYNC_MAX_BYTES (4U * 1024U * 1024U)
#define M0_SYNC_FOOTER_RESERVE 1024U

extern int __real_fsync(int fd);
extern int __real_fdatasync(int fd);
int __wrap_fsync(int fd);
int __wrap_fdatasync(int fd);

struct sync_event {
    uint64_t sequence, tid, caller, entry_ns, exit_ns;
    int fd, operation, result, saved_errno, clock_error;
};

static struct sync_event events[M0_SYNC_MAX_EVENTS];
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

__attribute__((constructor)) static void sync_diagnostic_init(void) {
    int previous_errno = errno;
    const char *fd_text = getenv("M0_SYNC_FD");
    const char *trial = getenv("M0_SYNC_TRIAL");
    const char *secret = getenv("M0_SYNC_TOKEN");
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
    memcpy(trial_id, trial, 33);
    memcpy(token, secret, 33);
    process_id = getpid();
    if (monotonic_ns(&enabled_ns) != 0)
        ++clock_errors;
    memset(&module, 0, sizeof(module));
    if (dladdr((void *)(uintptr_t)&__wrap_fsync, &module) && module.dli_fbase)
        module_base = (uint64_t)(uintptr_t)module.dli_fbase;
    else
        ++recording_errors;
    enabled = 1;
clear:
    unsetenv("M0_SYNC_FD");
    unsetenv("M0_SYNC_TRIAL");
    unsetenv("M0_SYNC_TOKEN");
done:
    errno = previous_errno;
}

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
        ssize_t count = write(artifact_fd, line + offset, length - offset);
        if (count < 0 && errno == EINTR && ++interrupted <= 16)
            continue;
        if (count <= 0) {
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

__attribute__((destructor)) static void sync_diagnostic_finish(void) {
    if (!enabled || getpid() != process_id)
        return;
    int previous_errno = errno;
    int threads = remaining_threads();
    pthread_mutex_lock(&event_mutex);
    finalizing = 1;
    if (threads != 1)
        ++recording_errors;
    uint64_t finished_ns = 0, written = 0;
    if (monotonic_ns(&finished_ns) != 0)
        ++clock_errors;
    char line[1024];
    int length = snprintf(line, sizeof(line),
        "{\"kind\":\"header\",\"schema\":\"m0-daemon-sync-v1\",\"trial_id\":\"%s\","
        "\"token\":\"%s\",\"pid\":%ld,\"clock\":\"CLOCK_MONOTONIC\","
        "\"unit\":\"nanoseconds\",\"module_base\":%" PRIu64 ","
        "\"caller_source\":\"__builtin_return_address\",\"max_bytes\":%u,"
        "\"max_events\":%u,\"event_memory_bytes\":%zu,\"enabled_monotonic_ns\":%" PRIu64 "}\n",
        trial_id, token, (long)process_id, module_base, M0_SYNC_MAX_BYTES,
        M0_SYNC_MAX_EVENTS, sizeof(events), enabled_ns);
    if (length < 0 || (size_t)length >= sizeof(line) || emit_line(line, (size_t)length, 0))
        goto done;
    for (uint64_t i = 0; i < recorded; ++i) {
        const struct sync_event *event = &events[i];
        length = snprintf(line, sizeof(line),
            "{\"kind\":\"event\",\"sequence\":%" PRIu64 ",\"pid\":%ld,"
            "\"tid\":%" PRIu64 ",\"operation\":\"%s\",\"fd\":%d,"
            "\"caller\":%" PRIu64 ",\"entry_ns\":%" PRIu64 ",\"exit_ns\":%" PRIu64 ","
            "\"return_value\":%d,\"errno\":%d,\"clock_error\":%s}\n",
            event->sequence, (long)process_id, event->tid,
            event->operation ? "fdatasync" : "fsync", event->fd, event->caller,
            event->entry_ns, event->exit_ns, event->result, event->saved_errno,
            event->clock_error ? "true" : "false");
        if (length < 0 || (size_t)length >= sizeof(line)) {
            ++recording_errors;
            break;
        }
        if (emit_line(line, (size_t)length, 0))
            break;
        ++written;
    }
    length = snprintf(line, sizeof(line),
        "{\"kind\":\"footer\",\"trial_id\":\"%s\",\"token\":\"%s\",\"pid\":%ld,"
        "\"observed\":%" PRIu64 ",\"written\":%" PRIu64 ",\"dropped\":%" PRIu64 ","
        "\"inflight\":%" PRIu64 ",\"clock_errors\":%" PRIu64 ","
        "\"recording_errors\":%" PRIu64 ",\"overflow\":%s,\"live_threads\":%d,"
        "\"finalized_monotonic_ns\":%" PRIu64 ",\"complete\":%s}\n",
        trial_id, token, (long)process_id, observed, written, dropped + recorded - written,
        inflight, clock_errors, recording_errors, overflow ? "true" : "false", threads,
        finished_ns, (!inflight && !dropped && observed == written &&
                     !clock_errors && !recording_errors && !overflow) ? "true" : "false");
    if (length > 0 && (size_t)length < sizeof(line))
        (void)emit_line(line, (size_t)length, 1);
done:
    (void)close(artifact_fd);
    pthread_mutex_unlock(&event_mutex);
    errno = previous_errno;
}
