// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT


enum metadata_operation
{
    META_STAT,
    META_FSTAT,
    META_STATVFS,
    META_MISSING,
    META_OPEN,
    META_CLOSE,
    META_CREATE,
    META_MKDIR,
    META_RMDIR,
    META_UNLINK,
    META_RENAME,
    META_LINK,
    META_SYMLINK,
    META_READLINK,
    META_CHMOD,
    META_CHOWN,
    META_TRUNCATE,
    META_FTRUNCATE,
    META_UTIME,
    META_FSYNC,
    META_FSYNC_RANGE,
    META_FSYNCDIR,
    META_SYNCHRONIZE,
    META_OPENDIR,
    META_CLOSEDIR,
    META_READDIR,
    META_READDIR_CACHED,
    META_ACCESS,
    META_SEEK,
    META_READ_SINGLE,
    META_WRITE_SINGLE,
    META_READ_VECTOR,
    META_WRITE_VECTOR,
    META_MAP,
    META_UNMAP,
    META_MAPPED_FIRST,
    META_MAPPED_HIT,
    META_MAPPED_FLUSH,
    META_APPEND,
    META_IOCTL_UNSUPPORTED,
    META_LOCK,
    META_FCLEAR_UNSUPPORTED,
    META_FIFO_UNSUPPORTED,
    META_FID_CAPTURE,
    META_FID_RESOLVE,
    META_DIRECTORY_SYNC_NATIVE,
    META_COUNT
};

static const char * const metadata_names[META_COUNT] = { "stat-path",
                                                         "fstat-handle",
                                                         "statvfs",
                                                         "lookup-missing",
                                                         "open-file",
                                                         "close-file",
                                                         "create-file",
                                                         "mkdir",
                                                         "rmdir",
                                                         "unlink",
                                                         "rename",
                                                         "hard-link",
                                                         "symlink",
                                                         "readlink",
                                                         "chmod",
                                                         "chown",
                                                         "truncate-path",
                                                         "ftruncate-handle",
                                                         "utime",
                                                         "fsync-file",
                                                         "fsync-range",
                                                         "fsync-range-directory-einval",
                                                         "sync-filesystem",
                                                         "opendir",
                                                         "closedir",
                                                         "readdir-snapshot",
                                                         "readdir-cached-entry",
                                                         "access",
                                                         "lseek",
                                                         "pread-4k",
                                                         "pwrite-4k",
                                                         "readv-4k",
                                                         "writev-4k",
                                                         "mmap",
                                                         "munmap",
                                                         "mapped-first-read",
                                                         "mapped-hit-read",
                                                         "mapped-flush",
                                                         "append-write",
                                                         "ioctl-unsupported",
                                                         "record-lock",
                                                         "fclear-unsupported",
                                                         "mkfifo-unsupported",
                                                         "fid-capture-composite",
                                                         "fid-resolve-composite",
                                                         "fsyncdir-vnode-composite" };

struct metadata_context
{
    const struct overhead_server * server;   // Calibration mount owner.
    char path[OVERHEAD_PATH_CAPACITY];       // Stable existing data file.
    char temporary[OVERHEAD_PATH_CAPACITY];  // Per-iteration mutation target.
    char moved[OVERHEAD_PATH_CAPACITY];      // Rename destination.
    char symbol[OVERHEAD_PATH_CAPACITY];     // Stable symbolic link.
    int descriptor;                          // Existing data file, retained for this case.
    int control_descriptor;                  // Test-only identifier diagnostic control device.
    struct usfs_test_fid_probe identifier;   // Captured native identifier, retained across resolution.
    int extra_descriptor;                    // Per-iteration open instance.
    DIR * directory;                         // Per-iteration directory stream.
    void * mapping;                          // Per-iteration retained mapping.
    unsigned char data[OVERHEAD_PAGE_BYTES]; // Prefaulted mapped/append data.
};

static int construct_overhead_path (char * destination, const size_t capacity, const char * root, const char * name)
{
    const int length = snprintf (destination, capacity, "%s/%s", root, name);
    if (length < 0 || (size_t)length >= capacity)
    {
        fprintf (stderr, "Failed to construct overhead path: root or name is too long\n");
        return -1;
    }
    return 0;
}

static int make_metadata_file (const char * path)
{
    const int descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, OVERHEAD_FILE_MODE);
    if (descriptor < 0)
        return -1;
    return close (descriptor);
}

static bool metadata_uses_mapping (const enum metadata_operation operation)
{
    return operation == META_MAP || operation == META_UNMAP || operation == META_MAPPED_FIRST || operation == META_MAPPED_HIT ||
           operation == META_MAPPED_FLUSH;
}

static bool metadata_reads_data (const enum metadata_operation operation)
{
    return operation == META_READ_SINGLE || operation == META_READ_VECTOR;
}

static bool metadata_writes_data (const enum metadata_operation operation)
{
    return operation == META_WRITE_SINGLE || operation == META_WRITE_VECTOR;
}

static bool metadata_uses_native_probe (const enum metadata_operation operation)
{
    return operation == META_FID_CAPTURE || operation == META_FID_RESOLVE || operation == META_DIRECTORY_SYNC_NATIVE;
}

static int copy_identifier_path (char * destination, const size_t capacity, const char * path)
{
    const size_t length = strlen (path);
    if (length >= capacity)
        return -1;

    memcpy (destination, path, length + 1);
    return 0;
}

static int prepare_metadata_iteration (struct metadata_context * context, const enum metadata_operation operation)
{
    context->extra_descriptor = -1;
    context->directory = NULL;
    context->mapping = MAP_FAILED;
    memset (context->data, OVERHEAD_READ_BYTE, sizeof (context->data));

    if (metadata_reads_data (operation) || metadata_writes_data (operation))
    {
        if (pwrite (context->descriptor, context->data, sizeof (context->data), 0) != (ssize_t)sizeof (context->data))
            return -1;
        memset (context->data, metadata_writes_data (operation) ? OVERHEAD_WRITE_BYTE : 0, sizeof (context->data));
        return lseek (context->descriptor, 0, SEEK_SET) == 0 ? 0 : -1;
    }

    if (operation == META_DIRECTORY_SYNC_NATIVE)
    {
        memset (&context->identifier, 0, sizeof (context->identifier));
        context->identifier.abi_version = USFS_TEST_ABI_VERSION;
        context->identifier.action = USFS_TEST_VNODE_SYNC_DIRECTORY;
        return copy_identifier_path (context->identifier.path, sizeof (context->identifier.path), context->server->mountpoint);
    }

    if (operation == META_FID_CAPTURE || operation == META_FID_RESOLVE)
    {
        memset (&context->identifier, 0, sizeof (context->identifier));
        context->identifier.abi_version = USFS_TEST_ABI_VERSION;
        context->identifier.action = USFS_TEST_FID_CAPTURE;
        if (copy_identifier_path (context->identifier.path, sizeof (context->identifier.path), context->path) != 0)
            return -1;
        if (operation == META_FID_CAPTURE)
            return 0;
        if (ioctl (context->control_descriptor, USFS_TEST_IOC_FID_PROBE, &context->identifier) != 0)
            return -1;
        context->identifier.action = USFS_TEST_FID_RESOLVE;
        if (copy_identifier_path (context->identifier.path, sizeof (context->identifier.path), context->server->mountpoint) != 0)
            return -1;
        return copy_identifier_path (context->identifier.expected_path, sizeof (context->identifier.expected_path), context->path);
    }

    if (operation == META_CLOSE)
        context->extra_descriptor = open (context->path, O_RDWR);
    else if (operation == META_RMDIR)
        return mkdir (context->temporary, OVERHEAD_DIRECTORY_MODE);
    else if (operation == META_UNLINK || operation == META_RENAME)
        return make_metadata_file (context->temporary);
    else if (operation == META_FTRUNCATE || operation == META_TRUNCATE)
        return ftruncate (context->descriptor, OVERHEAD_FILE_BYTES);
    else if (operation == META_FSYNCDIR)
        context->extra_descriptor = open (context->server->mountpoint, O_RDONLY);
    else if (operation == META_READDIR || operation == META_READDIR_CACHED || operation == META_CLOSEDIR)
    {
        context->directory = opendir (context->server->mountpoint);
        if (context->directory == NULL)
            return -1;
        if (operation == META_READDIR_CACHED && readdir (context->directory) == NULL)
            return -1;
    }
    else if (operation == META_APPEND)
        context->extra_descriptor = open (context->temporary, O_CREAT | O_EXCL | O_RDWR | O_APPEND, OVERHEAD_FILE_MODE);
    else if (metadata_uses_mapping (operation))
    {
        context->extra_descriptor = open (context->temporary, O_CREAT | O_EXCL | O_RDWR, OVERHEAD_FILE_MODE);
        if (context->extra_descriptor < 0)
            return -1;
        if (pwrite (context->extra_descriptor, context->data, sizeof (context->data), 0) != (ssize_t)sizeof (context->data))
            return -1;
        if (operation == META_MAP)
            return 0;
        context->mapping = mmap (NULL, sizeof (context->data), PROT_READ | PROT_WRITE, MAP_SHARED, context->extra_descriptor, 0);
        if (context->mapping == MAP_FAILED)
            return -1;
        if (operation == META_MAPPED_HIT)
        {
            if (pread (context->extra_descriptor, context->data, sizeof (context->data), 0) != (ssize_t)sizeof (context->data))
                return -1;
        }
        if (operation == META_MAPPED_FLUSH)
            memset (context->mapping, OVERHEAD_WRITE_BYTE, sizeof (context->data));
    }

    if ((operation == META_CLOSE || operation == META_FSYNCDIR || operation == META_APPEND) && context->extra_descriptor < 0)
        return -1;
    return 0;
}

static int execute_metadata_iteration (struct metadata_context * context, const enum metadata_operation operation)
{
    struct stat attributes = { 0 };
    struct statvfs filesystem_statistics = { 0 };
    struct flock file_lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1 };
    struct utimbuf requested_times = { .actime = OVERHEAD_FIXED_TIME, .modtime = OVERHEAD_FIXED_TIME + 1 };
    struct iovec vectors[] = { { .iov_base = context->data, .iov_len = sizeof (context->data) / 2 },
                               { .iov_base = context->data + sizeof (context->data) / 2, .iov_len = sizeof (context->data) / 2 } };
    char target[OVERHEAD_NAME_CAPACITY];
    ssize_t count;
    int rc;

    switch (operation)
    {
        case META_STAT:
            return stat (context->path, &attributes);
        case META_FSTAT:
            return fstat (context->descriptor, &attributes);
        case META_STATVFS:
            return statvfs (context->server->mountpoint, &filesystem_statistics);
        case META_MISSING:
            errno = 0;
            rc = stat (context->temporary, &attributes);
            return rc == -1 && errno == ENOENT ? 0 : -1;
        case META_OPEN:
            context->extra_descriptor = open (context->path, O_RDWR);
            return context->extra_descriptor < 0 ? -1 : 0;
        case META_CLOSE:
            rc = close (context->extra_descriptor);
            context->extra_descriptor = -1;
            return rc;
        case META_CREATE:
            context->extra_descriptor = open (context->temporary, O_CREAT | O_EXCL | O_RDWR, OVERHEAD_FILE_MODE);
            return context->extra_descriptor < 0 ? -1 : 0;
        case META_MKDIR:
            return mkdir (context->temporary, OVERHEAD_DIRECTORY_MODE);
        case META_RMDIR:
            return rmdir (context->temporary);
        case META_UNLINK:
            return unlink (context->temporary);
        case META_RENAME:
            return rename (context->temporary, context->moved);
        case META_LINK:
            return link (context->path, context->temporary);
        case META_SYMLINK:
            return symlink ("data-0", context->temporary);
        case META_READLINK:
            count = readlink (context->symbol, target, sizeof (target));
            return count == (ssize_t)strlen ("data-0") && memcmp (target, "data-0", (size_t)count) == 0 ? 0 : -1;
        case META_CHMOD:
            return chmod (context->path, OVERHEAD_CHANGED_MODE);
        case META_CHOWN:
            return chown (context->path, getuid (), getgid ());
        case META_TRUNCATE:
            return truncate (context->path, OVERHEAD_FILE_BYTES - OVERHEAD_PAGE_BYTES);
        case META_FTRUNCATE:
            return ftruncate (context->descriptor, OVERHEAD_FILE_BYTES - OVERHEAD_PAGE_BYTES);
        case META_UTIME:
            return utime (context->path, &requested_times);
        case META_FSYNC:
            return fsync (context->descriptor);
        case META_FSYNC_RANGE:
            return fsync_range (context->descriptor, 0, 0, OVERHEAD_PAGE_BYTES);
        case META_FSYNCDIR:
            /* AIX fsync requires a writable descriptor; fsync_range does not. */
            rc = fsync_range (context->extra_descriptor, O_SYNC, 0, 0);
            return rc == -1 && errno == EINVAL ? 0 : -1;
        case META_SYNCHRONIZE:
            return syncvfs ((char *)context->server->mountpoint, FS_SYNCVFS_FS | FS_SYNCVFS_FORCE);
        case META_OPENDIR:
            context->directory = opendir (context->server->mountpoint);
            return context->directory == NULL ? -1 : 0;
        case META_CLOSEDIR:
            rc = closedir (context->directory);
            context->directory = NULL;
            return rc;
        case META_READDIR: {
            unsigned entries = 0;
            errno = 0;
            while (readdir (context->directory) != NULL)
                entries++;
            return entries == OVERHEAD_CLIENTS + 3 && errno == 0 ? 0 : -1;
        }
        case META_READDIR_CACHED:
            return readdir (context->directory) != NULL ? 0 : -1;
        case META_ACCESS:
            return access (context->path, R_OK | W_OK);
        case META_SEEK:
            return lseek (context->descriptor, OVERHEAD_PAGE_BYTES, SEEK_SET) == OVERHEAD_PAGE_BYTES ? 0 : -1;
        case META_MAP:
            context->mapping = mmap (NULL, OVERHEAD_PAGE_BYTES, PROT_READ, MAP_SHARED, context->extra_descriptor, 0);
            return context->mapping == MAP_FAILED ? -1 : 0;
        case META_UNMAP:
            rc = munmap (context->mapping, OVERHEAD_PAGE_BYTES);
            context->mapping = MAP_FAILED;
            return rc;
        case META_MAPPED_FIRST:
        case META_MAPPED_HIT:
            count = pread (context->extra_descriptor, context->data, sizeof (context->data), 0);
            return count == (ssize_t)sizeof (context->data) ? 0 : -1;
        case META_MAPPED_FLUSH:
            rc = msync (context->mapping, sizeof (context->data), MS_SYNC);
            return rc == 0 ? fsync (context->extra_descriptor) : rc;
        case META_APPEND:
            count = write (context->extra_descriptor, context->data, sizeof (context->data));
            return count == (ssize_t)sizeof (context->data) ? 0 : -1;
        case META_READ_SINGLE:
            return pread (context->descriptor, context->data, sizeof (context->data), 0) == (ssize_t)sizeof (context->data) ? 0 : -1;
        case META_WRITE_SINGLE:
            return pwrite (context->descriptor, context->data, sizeof (context->data), 0) == (ssize_t)sizeof (context->data) ? 0 : -1;
        case META_READ_VECTOR:
            return readv (context->descriptor, vectors, sizeof (vectors) / sizeof (vectors[0])) == (ssize_t)sizeof (context->data) ? 0 : -1;
        case META_WRITE_VECTOR:
            return writev (context->descriptor, vectors, sizeof (vectors) / sizeof (vectors[0])) == (ssize_t)sizeof (context->data) ? 0 : -1;
        case META_IOCTL_UNSUPPORTED:
            errno = 0;
            rc = ioctl (context->descriptor, 0, NULL);
            return rc == -1 && (errno == ENOSYS || errno == ENOTTY) ? 0 : -1;
        case META_LOCK:
            rc = fcntl (context->descriptor, F_SETLK, &file_lock);
            if (rc != 0)
                return -1;
            file_lock.l_type = F_UNLCK;
            return fcntl (context->descriptor, F_SETLK, &file_lock) == 0 ? 0 : -1;
        case META_FCLEAR_UNSUPPORTED:
            errno = 0;
            rc = fclear (context->descriptor, OVERHEAD_PAGE_BYTES);
            return rc == -1 && errno == ENOSYS ? 0 : -1;
        case META_FIFO_UNSUPPORTED:
            errno = 0;
            rc = mkfifo (context->temporary, OVERHEAD_FILE_MODE);
            return rc == -1 && (errno == ENOSYS || errno == EINVAL) ? 0 : -1;
        case META_FID_CAPTURE:
        case META_FID_RESOLVE:
        case META_DIRECTORY_SYNC_NATIVE:
            return ioctl (context->control_descriptor, USFS_TEST_IOC_FID_PROBE, &context->identifier);
        default:
            return -1;
    }
}

static int finish_metadata_iteration (struct metadata_context * context, const enum metadata_operation operation)
{
    int rc = 0;
    if (metadata_writes_data (operation))
        if (pread (context->descriptor, context->data, sizeof (context->data), 0) != (ssize_t)sizeof (context->data))
            rc = -1;
    if (metadata_reads_data (operation) || metadata_writes_data (operation))
    {
        const unsigned char expected = metadata_writes_data (operation) ? OVERHEAD_WRITE_BYTE : OVERHEAD_READ_BYTE;
        for (unsigned index = 0; index < sizeof (context->data); ++index)
            if (context->data[index] != expected)
                rc = -1;
    }
    if (operation == META_MAPPED_FIRST || operation == META_MAPPED_HIT)
    {
        for (unsigned index = 0; index < sizeof (context->data); ++index)
            if (context->data[index] != OVERHEAD_READ_BYTE)
                rc = -1;
    }
    if (context->mapping != MAP_FAILED)
        if (munmap (context->mapping, OVERHEAD_PAGE_BYTES) != 0)
            rc = -1;
    if ((operation == META_MAPPED_FLUSH || operation == META_APPEND) && context->extra_descriptor >= 0)
    {
        if (pread (context->extra_descriptor, context->data, sizeof (context->data), 0) != (ssize_t)sizeof (context->data))
            rc = -1;
        for (unsigned index = 0; index < sizeof (context->data); ++index)
            if (context->data[index] != (operation == META_APPEND ? OVERHEAD_READ_BYTE : OVERHEAD_WRITE_BYTE))
                rc = -1;
    }
    if (context->extra_descriptor >= 0)
        if (close (context->extra_descriptor) != 0)
            rc = -1;
    if (context->directory != NULL)
        if (closedir (context->directory) != 0)
            rc = -1;
    if (operation == META_MKDIR)
        if (rmdir (context->temporary) != 0)
            rc = -1;
    if (operation == META_RENAME)
        if (unlink (context->moved) != 0)
            rc = -1;
    if (operation == META_CREATE || operation == META_LINK || operation == META_SYMLINK || operation == META_APPEND ||
        metadata_uses_mapping (operation))
        if (unlink (context->temporary) != 0)
            rc = -1;
    return rc;
}

static struct callback_metric total_callback_metrics (void)
{
    struct callback_metric total = { 0 };
    for (unsigned kind = 0; kind < CALLBACK_COUNT; ++kind)
    {
        const struct callback_metric metric = read_callback_metric (kind);
        total.calls += metric.calls;
        total.wall_ns += metric.wall_ns;
        total.cpu_ns += metric.cpu_ns;
        total.bytes += metric.bytes;
        total.errors += metric.errors;
    }
    return total;
}

static int measure_metadata_case (
    struct metadata_context * context,
    FILE * output,
    FILE * callbacks,
    const enum metadata_operation operation,
    const unsigned round,
    const unsigned samples
)
{
    reset_callback_metrics ();
    for (unsigned iteration = 0; iteration < samples && !overhead_interrupted; ++iteration)
    {
        int rc = prepare_metadata_iteration (context, operation);
        if (rc != 0)
        {
            (void)finish_metadata_iteration (context, operation);
            return rc;
        }
        struct kext_state before = { 0 }, after = { 0 };
        if (capture_kernel_status (&before) != 0)
            return -1;
        const struct callback_metric callback_before = total_callback_metrics ();
        __atomic_store_n (&overhead_shared->enabled, 1, __ATOMIC_RELEASE);
        errno = 0;
        const uint64_t started_ns = monotonic_ns ();
        rc = execute_metadata_iteration (context, operation);
        const uint64_t elapsed_ns = monotonic_ns () - started_ns;
        __atomic_store_n (&overhead_shared->enabled, 0, __ATOMIC_RELEASE);
        const int saved_errno = errno;
        const struct callback_metric callback_after = total_callback_metrics ();
        const int status_result = capture_kernel_status (&after);
        const int cleanup_result = finish_metadata_iteration (context, operation);
        if (rc != 0 || cleanup_result != 0 || status_result != 0)
        {
            fprintf (
                stderr,
                "Failed overhead operation %s iteration %u: rc=%d cleanup=%d errno=%d\n",
                metadata_names[operation],
                iteration,
                rc,
                cleanup_result,
                saved_errno
            );
            return -1;
        }
        fprintf (
            output,
            "%s,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%d\n",
            metadata_names[operation],
            round,
            iteration,
            (unsigned long long)elapsed_ns,
            (unsigned long long)(callback_after.wall_ns - callback_before.wall_ns),
            (unsigned long long)(callback_after.cpu_ns - callback_before.cpu_ns),
            (unsigned long long)(callback_after.calls - callback_before.calls),
            (unsigned long long)(after.accepted_requests - before.accepted_requests),
            (unsigned long long)(callback_after.errors - callback_before.errors),
            saved_errno
        );
    }
    write_callback_metrics (callbacks, metadata_names[operation], round);
    fflush (output);
    return overhead_interrupted ? -1 : 0;
}

static int run_metadata_suite (
    const struct overhead_server * server,
    FILE * output,
    FILE * callbacks,
    const unsigned rounds,
    const bool smoke,
    const bool fallback_creation,
    const bool identifiers
)
{
    struct metadata_context context = { .server = server, .descriptor = -1, .extra_descriptor = -1, .control_descriptor = -1, .mapping = MAP_FAILED };
    if (construct_overhead_path (context.path, sizeof (context.path), server->mountpoint, "data-0") != 0 ||
        construct_overhead_path (context.temporary, sizeof (context.temporary), server->mountpoint, "probe") != 0 ||
        construct_overhead_path (context.moved, sizeof (context.moved), server->mountpoint, "probe-moved") != 0 ||
        construct_overhead_path (context.symbol, sizeof (context.symbol), server->mountpoint, "symbol") != 0)
        return -1;
    memset (context.data, OVERHEAD_READ_BYTE, sizeof (context.data));
    context.descriptor = open (context.path, O_RDWR);
    if (context.descriptor < 0)
        return -1;
    if (identifiers)
    {
        context.control_descriptor = open ("/dev/usfs0", O_RDWR);
        if (context.control_descriptor < 0)
        {
            close (context.descriptor);
            return -1;
        }
    }
    int rc = symlink ("data-0", context.symbol);
    if (rc == 0)
    {
        for (unsigned round = 1; round <= rounds && rc == 0; ++round)
        {
            for (unsigned operation = 0; operation < META_COUNT && rc == 0; ++operation)
            {
                if (fallback_creation && operation != META_CREATE)
                    continue;
                if (identifiers != metadata_uses_native_probe (operation))
                    continue;
                fprintf (stderr, "metadata round=%u operation=%s\n", round, metadata_names[operation]);
                rc = measure_metadata_case (
                    &context,
                    output,
                    callbacks,
                    (enum metadata_operation)operation,
                    round,
                    smoke ? 2 : OVERHEAD_METADATA_SAMPLES
                );
            }
        }
    }
    (void)unlink (context.symbol);
    if (context.control_descriptor >= 0)
        if (close (context.control_descriptor) != 0)
            rc = -1;
    const int close_result = close (context.descriptor);
    return rc == 0 ? close_result : rc;
}

#include "data_transfers.h"

static FILE * open_overhead_output (const char * prefix, const char * suffix)
{
    char path[OVERHEAD_PATH_CAPACITY];
    const int length = snprintf (path, sizeof (path), "%s%s", prefix, suffix);
    if (length < 0 || (size_t)length >= sizeof (path))
        return NULL;
    const int descriptor = open (path, O_WRONLY | O_CREAT | O_EXCL, OVERHEAD_FILE_MODE);
    if (descriptor < 0)
        return NULL;
    FILE * output = fdopen (descriptor, "w");
    if (output == NULL)
        close (descriptor);
    return output;
}

static int run_overhead_workloads (
    const struct overhead_server * server,
    const char * prefix,
    const unsigned rounds,
    const char * suite,
    const bool fallback_creation
)
{
    FILE * metadata = open_overhead_output (prefix, "-metadata.csv");
    FILE * data = open_overhead_output (prefix, "-data.csv");
    FILE * callbacks = open_overhead_output (prefix, "-callbacks.csv");
    if (metadata == NULL || data == NULL || callbacks == NULL)
    {
        if (metadata != NULL)
            fclose (metadata);
        if (data != NULL)
            fclose (data);
        if (callbacks != NULL)
            fclose (callbacks);
        return -1;
    }
    fprintf (
        metadata,
        "operation,round,iteration,elapsed_ns,callback_wall_ns,callback_cpu_ns,callback_calls,rpc_requests,callback_errors,observed_errno\n"
    );
    fprintf (
        data,
        "operation,path,read_bytes,clients,files,round,delay_us,instrumented,operations,bytes,elapsed_ns,client_cpu_ns,daemon_cpu_ns,callback_wall_ns,callback_cpu_ns,callback_calls,rpc_requests\n"
    );
    fprintf (callbacks, "operation,round,callback,calls,wall_ns,cpu_ns,bytes,errors\n");
    int rc = 0;
    if (strcmp (suite, "data") != 0 && strcmp (suite, "data-smoke") != 0)
        rc = run_metadata_suite (
            server,
            metadata,
            callbacks,
            rounds,
            strcmp (suite, "smoke") == 0,
            fallback_creation,
            strcmp (suite, "identifiers") == 0
        );
    if (rc == 0 && (strcmp (suite, "data") == 0 || strcmp (suite, "smoke") == 0 || strcmp (suite, "data-smoke") == 0))
    {
        rc = select_application_identity ();
        if (rc == 0)
            rc = run_data_suite (server, data, callbacks, rounds, strcmp (suite, "smoke") == 0 || strcmp (suite, "data-smoke") == 0);
    }
    if (fclose (metadata) != 0)
        rc = -1;
    if (fclose (data) != 0)
        rc = -1;
    if (fclose (callbacks) != 0)
        rc = -1;
    return rc;
}
