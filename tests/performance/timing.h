// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

/* Callback inventory follows every member of struct usfs_operations. */
enum callback_kind
{
    CALLBACK_INITIALIZE = 0,
    CALLBACK_GETATTR = 1,
    CALLBACK_STATFS = 2,
    CALLBACK_READLINK = 3,
    CALLBACK_CHMOD = 4,
    CALLBACK_CHOWN = 5,
    CALLBACK_TRUNCATE = 6,
    CALLBACK_UTIMENS = 7,
    CALLBACK_MKDIR = 8,
    CALLBACK_RMDIR = 9,
    CALLBACK_UNLINK = 10,
    CALLBACK_RENAME = 11,
    CALLBACK_LINK = 12,
    CALLBACK_SYMLINK = 13,
    CALLBACK_CREATE = 14,
    CALLBACK_CREATE_ATTR = 15,
    CALLBACK_OPEN = 16,
    CALLBACK_READ = 17,
    CALLBACK_WRITE = 18,
    CALLBACK_FLUSH = 19,
    CALLBACK_FSYNC = 20,
    CALLBACK_RELEASE = 21,
    CALLBACK_SYNCFS = 22,
    CALLBACK_OPENDIR = 23,
    CALLBACK_READDIR = 24,
    CALLBACK_FSYNCDIR = 25,
    CALLBACK_RELEASEDIR = 26,
    CALLBACK_EXPORT_ID = 27,
    CALLBACK_RESOLVE_ID = 28,
    CALLBACK_SHUTDOWN,
    CALLBACK_COUNT
};


enum overhead_limits
{
    OVERHEAD_CLIENTS = 4,
    OVERHEAD_FILE_BYTES = 1024 * 1024,
    OVERHEAD_PAGE_BYTES = 4096,
    OVERHEAD_NANOSECONDS = 1000000000,
    OVERHEAD_MICROSECONDS = 1000000,
    OVERHEAD_NANOSECONDS_PER_MICROSECOND = 1000,
    OVERHEAD_METADATA_SAMPLES = 200,
    OVERHEAD_PATH_CAPACITY = 4096,
    OVERHEAD_CLOCK_BATCH = 64,
    OVERHEAD_NAME_CAPACITY = 64,
    OVERHEAD_CONTROL_TIMEOUT_MS = 30000,
    OVERHEAD_BACKEND_MIB = 8,
    OVERHEAD_BACKEND_INODES = 10000,
    OVERHEAD_MAX_ROUNDS = 10,
    OVERHEAD_READ_BYTE = 0x5a,
    OVERHEAD_WRITE_BYTE = 0xa5,
    OVERHEAD_FILE_MODE = 0600,
    OVERHEAD_DIRECTORY_MODE = 0700,
    OVERHEAD_CHANGED_MODE = 0640,
    OVERHEAD_FIXED_TIME = 1700000000
};

struct callback_metric
{
    uint64_t calls;   // Completed backend invocations.
    uint64_t wall_ns; // Callback bracket, including injected service delay.
    uint64_t cpu_ns;  // Calling worker CPU, quantized to getrusage microseconds.
    uint64_t bytes;   // Successful read/write payload bytes.
    uint64_t errors;  // Negative callback results.
};

struct overhead_shared
{
    unsigned enabled;                               // Atomic permission to collect callback metrics.
    unsigned instrumented;                          // Whether this campaign uses timing adapters.
    unsigned delay_us;                              // Requested service delay for each backend callback.
    unsigned workers;                               // Production client worker pool size.
    uid_t application_uid;                          // Optional non-root data caller and file owner.
    struct callback_metric metrics[CALLBACK_COUNT]; // Atomic per-callback totals.
};

struct callback_scope
{
    bool enabled;        // Whether this invocation contributes metrics.
    uint64_t started_ns; // Monotonic time before callback service.
    struct rusage usage; // Worker CPU before callback service.
};

static struct overhead_shared * overhead_shared;
static volatile sig_atomic_t overhead_interrupted;

static uint64_t monotonic_ns (void)
{
    struct timespec value = { 0 };
    if (clock_gettime (CLOCK_MONOTONIC, &value) != 0)
    {
        fprintf (stderr, "Failed to read monotonic clock: %s\n", strerror (errno));
        exit (EXIT_FAILURE);
    }
    return (uint64_t)value.tv_sec * OVERHEAD_NANOSECONDS + (uint64_t)value.tv_nsec;
}

static uint64_t resource_cpu_ns (const struct rusage * usage)
{
    return ((uint64_t)usage->ru_utime.tv_sec + (uint64_t)usage->ru_stime.tv_sec) * OVERHEAD_NANOSECONDS +
           ((uint64_t)usage->ru_utime.tv_usec + (uint64_t)usage->ru_stime.tv_usec) * OVERHEAD_NANOSECONDS_PER_MICROSECOND;
}

static void delay_callback (void)
{
    if (overhead_shared->delay_us == 0)
        return;

    struct timespec remaining = { .tv_sec = overhead_shared->delay_us / OVERHEAD_MICROSECONDS,
                                  .tv_nsec = (overhead_shared->delay_us % OVERHEAD_MICROSECONDS) * OVERHEAD_NANOSECONDS_PER_MICROSECOND };
    while (nanosleep (&remaining, &remaining) != 0)
    {
        if (errno == EINTR)
            continue;

        fprintf (stderr, "Failed to delay calibration callback: %s\n", strerror (errno));
        exit (EXIT_FAILURE);
    }
}

static struct callback_scope begin_callback (const enum callback_kind kind)
{
    struct callback_scope scope = { 0 };
    scope.enabled = overhead_shared->instrumented &&
                    (__atomic_load_n (&overhead_shared->enabled, __ATOMIC_ACQUIRE) != 0 || kind == CALLBACK_INITIALIZE || kind == CALLBACK_SHUTDOWN);
    if (scope.enabled)
    {
        scope.started_ns = monotonic_ns ();
        if (getrusage (RUSAGE_THREAD, &scope.usage) != 0)
            exit (EXIT_FAILURE);
    }
    delay_callback ();
    return scope;
}

static void finish_callback (const struct callback_scope * scope, const enum callback_kind kind, const ssize_t result, const uint64_t bytes)
{
    if (!scope->enabled)
        return;

    struct rusage usage = { 0 };
    if (getrusage (RUSAGE_THREAD, &usage) != 0)
        exit (EXIT_FAILURE);

    const uint64_t elapsed_ns = monotonic_ns () - scope->started_ns;
    struct callback_metric * metric = &overhead_shared->metrics[kind];
    __atomic_fetch_add (&metric->wall_ns, elapsed_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add (&metric->cpu_ns, resource_cpu_ns (&usage) - resource_cpu_ns (&scope->usage), __ATOMIC_RELAXED);
    __atomic_fetch_add (&metric->bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add (&metric->errors, result < 0 ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_fetch_add (&metric->calls, 1, __ATOMIC_RELEASE);
}

static void reset_callback_metrics (void)
{
    __atomic_store_n (&overhead_shared->enabled, 0, __ATOMIC_RELEASE);
    for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
    {
        struct callback_metric * metric = &overhead_shared->metrics[kind];
        __atomic_store_n (&metric->calls, 0, __ATOMIC_RELAXED);
        __atomic_store_n (&metric->wall_ns, 0, __ATOMIC_RELAXED);
        __atomic_store_n (&metric->cpu_ns, 0, __ATOMIC_RELAXED);
        __atomic_store_n (&metric->bytes, 0, __ATOMIC_RELAXED);
        __atomic_store_n (&metric->errors, 0, __ATOMIC_RELAXED);
    }
}

static struct callback_metric read_callback_metric (const unsigned kind)
{
    const struct callback_metric * metric = &overhead_shared->metrics[kind];
    struct callback_metric result = { .calls = __atomic_load_n (&metric->calls, __ATOMIC_ACQUIRE),
                                      .wall_ns = __atomic_load_n (&metric->wall_ns, __ATOMIC_RELAXED),
                                      .cpu_ns = __atomic_load_n (&metric->cpu_ns, __ATOMIC_RELAXED),
                                      .bytes = __atomic_load_n (&metric->bytes, __ATOMIC_RELAXED),
                                      .errors = __atomic_load_n (&metric->errors, __ATOMIC_RELAXED) };
    return result;
}

/* Read/write calibration uses preallocated, stable objects. Namespace callbacks
 * retain memfs's ordinary synchronization. Timed data paths do no allocation,
 * metadata update, backing syscall or global backend locking. Each write worker
 * owns a different inode; shared-inode calibration is read-only. */
static ssize_t calibration_read (
    const struct usfs_client_request * request,
    const char * path,
    char * buffer,
    const size_t count,
    const off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)request;
    struct mem_inode * inode = find_operation_inode (NULL, path, file_info);
    if (inode == NULL)
        return -EBADF;
    if (offset < 0 || (uint64_t)offset > inode->size)
        return -EINVAL;
    const size_t available = inode->size - (size_t)offset;
    const size_t received = count < available ? count : available;
    memcpy (buffer, inode->data + offset, received);
    return (ssize_t)received;
}

static ssize_t calibration_write (
    const struct usfs_client_request * request,
    const char * path,
    const char * buffer,
    const size_t count,
    const off_t offset,
    struct usfs_open_file * file_info
)
{
    (void)request;
    struct mem_inode * inode = find_operation_inode (NULL, path, file_info);
    if (inode == NULL)
        return -EBADF;
    if (offset < 0 || (uint64_t)offset > inode->size)
        return -EINVAL;
    if (count > inode->size - (size_t)offset)
        return memfs_write (request, path, buffer, count, offset, file_info);
    memcpy (inode->data + offset, buffer, count);
    return (ssize_t)count;
}
