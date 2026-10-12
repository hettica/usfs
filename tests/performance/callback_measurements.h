// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

/* Benchmark-only callback adapters; the production client is linked unchanged. */

static int measured_initialize (const struct usfs_client_request * request, const struct usfs_limits * limits, struct usfs_behavior * behavior)
{
    struct callback_scope scope = begin_callback (CALLBACK_INITIALIZE);
    const int result = initialize_filesystem (request, limits, behavior);
    finish_callback (&scope, CALLBACK_INITIALIZE, result, 0);
    return result;
}

static int measured_getattr (
    const struct usfs_client_request * request,
    const char * path,
    struct stat * attributes,
    struct usfs_open_file * file_info
)
{
    struct callback_scope scope = begin_callback (CALLBACK_GETATTR);
    const int result = get_file_attributes (request, path, attributes, file_info);
    finish_callback (&scope, CALLBACK_GETATTR, result, 0);
    return result;
}

static int measured_statfs (const struct usfs_client_request * request, const char * path, struct statvfs * statistics)
{
    struct callback_scope scope = begin_callback (CALLBACK_STATFS);
    const int result = memfs_statfs (request, path, statistics);
    finish_callback (&scope, CALLBACK_STATFS, result, 0);
    return result;
}

static int measured_readlink (const struct usfs_client_request * request, const char * path, char * buffer, size_t capacity)
{
    struct callback_scope scope = begin_callback (CALLBACK_READLINK);
    const int result = read_symbolic_link (request, path, buffer, capacity);
    finish_callback (&scope, CALLBACK_READLINK, result, 0);
    return result;
}

static int measured_chmod (const struct usfs_client_request * request, const char * path, mode_t mode, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_CHMOD);
    const int result = change_file_permissions (request, path, mode, file_info);
    finish_callback (&scope, CALLBACK_CHMOD, result, 0);
    return result;
}

static int measured_chown (const struct usfs_client_request * request, const char * path, uid_t uid, gid_t gid, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_CHOWN);
    const int result = change_file_owner (request, path, uid, gid, file_info);
    finish_callback (&scope, CALLBACK_CHOWN, result, 0);
    return result;
}

static int measured_truncate (const struct usfs_client_request * request, const char * path, off_t length, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_TRUNCATE);
    const int result = memfs_truncate (request, path, length, file_info);
    finish_callback (&scope, CALLBACK_TRUNCATE, result, 0);
    return result;
}

static int measured_utimens (
    const struct usfs_client_request * request,
    const char * path,
    const struct timespec times[2],
    struct usfs_open_file * file_info
)
{
    struct callback_scope scope = begin_callback (CALLBACK_UTIMENS);
    const int result = set_file_times (request, path, times, file_info);
    finish_callback (&scope, CALLBACK_UTIMENS, result, 0);
    return result;
}

static int measured_mkdir (const struct usfs_client_request * request, const char * path, mode_t mode)
{
    struct callback_scope scope = begin_callback (CALLBACK_MKDIR);
    const int result = memfs_mkdir (request, path, mode);
    finish_callback (&scope, CALLBACK_MKDIR, result, 0);
    return result;
}

static int measured_rmdir (const struct usfs_client_request * request, const struct usfs_expected_entry * expected)
{
    struct callback_scope scope = begin_callback (CALLBACK_RMDIR);
    const int result = remove_directory (request, expected);
    finish_callback (&scope, CALLBACK_RMDIR, result, 0);
    return result;
}

static int measured_unlink (const struct usfs_client_request * request, const struct usfs_expected_entry * expected)
{
    struct callback_scope scope = begin_callback (CALLBACK_UNLINK);
    const int result = memfs_unlink (request, expected);
    finish_callback (&scope, CALLBACK_UNLINK, result, 0);
    return result;
}

static int measured_rename (
    const struct usfs_client_request * request,
    const struct usfs_expected_entry * source,
    const struct usfs_expected_entry * destination
)
{
    struct callback_scope scope = begin_callback (CALLBACK_RENAME);
    const int result = memfs_rename (request, source, destination);
    finish_callback (&scope, CALLBACK_RENAME, result, 0);
    return result;
}

static int measured_link (const struct usfs_client_request * request, const char * from, const char * to)
{
    struct callback_scope scope = begin_callback (CALLBACK_LINK);
    const int result = memfs_link (request, from, to);
    finish_callback (&scope, CALLBACK_LINK, result, 0);
    return result;
}

static int measured_symlink (const struct usfs_client_request * request, const char * target, const char * path)
{
    struct callback_scope scope = begin_callback (CALLBACK_SYMLINK);
    const int result = memfs_symlink (request, target, path);
    finish_callback (&scope, CALLBACK_SYMLINK, result, 0);
    return result;
}

static int measured_create (const struct usfs_client_request * request, const char * path, mode_t mode, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_CREATE);
    const int result = memfs_create (request, path, mode, file_info);
    finish_callback (&scope, CALLBACK_CREATE, result, 0);
    return result;
}

static int measured_create_attr (
    const struct usfs_client_request * request,
    const char * path,
    const struct stat * initial,
    unsigned int valid,
    enum usfs_create_action action,
    struct stat * attributes,
    struct usfs_open_file * file_info
)
{
    struct callback_scope scope = begin_callback (CALLBACK_CREATE_ATTR);
    const int result = memfs_create_attr (request, path, initial, valid, action, attributes, file_info);
    finish_callback (&scope, CALLBACK_CREATE_ATTR, result, 0);
    return result;
}

static int measured_open (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_OPEN);
    const int result = open_file (request, path, file_info);
    finish_callback (&scope, CALLBACK_OPEN, result, 0);
    return result;
}

static ssize_t measured_read (
    const struct usfs_client_request * request,
    const char * path,
    char * buffer,
    size_t count,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    struct callback_scope scope = begin_callback (CALLBACK_READ);
    const ssize_t result = calibration_read (request, path, buffer, count, offset, file_info);
    finish_callback (&scope, CALLBACK_READ, result, result > 0 ? (uint64_t)result : 0);
    return result;
}

static ssize_t measured_write (
    const struct usfs_client_request * request,
    const char * path,
    const char * buffer,
    size_t count,
    off_t offset,
    struct usfs_open_file * file_info
)
{
    struct callback_scope scope = begin_callback (CALLBACK_WRITE);
    const ssize_t result = calibration_write (request, path, buffer, count, offset, file_info);
    finish_callback (&scope, CALLBACK_WRITE, result, result > 0 ? (uint64_t)result : 0);
    return result;
}

static int measured_flush (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_FLUSH);
    const int result = flush_file (request, path, file_info);
    finish_callback (&scope, CALLBACK_FLUSH, result, 0);
    return result;
}

static int measured_fsync (const struct usfs_client_request * request, const char * path, int datasync, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_FSYNC);
    const int result = synchronize_file (request, path, datasync, file_info);
    finish_callback (&scope, CALLBACK_FSYNC, result, 0);
    return result;
}

static int measured_release (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_RELEASE);
    const int result = memfs_release (request, path, file_info);
    finish_callback (&scope, CALLBACK_RELEASE, result, 0);
    return result;
}

static int measured_syncfs (const struct usfs_client_request * request, const char * path)
{
    struct callback_scope scope = begin_callback (CALLBACK_SYNCFS);
    const int result = synchronize_filesystem (request, path);
    finish_callback (&scope, CALLBACK_SYNCFS, result, 0);
    return result;
}

static int measured_opendir (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_OPENDIR);
    const int result = memfs_opendir (request, path, file_info);
    finish_callback (&scope, CALLBACK_OPENDIR, result, 0);
    return result;
}

static int measured_readdir (
    const struct usfs_client_request * request,
    const char * path,
    const struct usfs_object_identity * identity,
    struct usfs_directory_sink * sink
)
{
    struct callback_scope scope = begin_callback (CALLBACK_READDIR);
    const int result = read_directory (request, path, identity, sink);
    finish_callback (&scope, CALLBACK_READDIR, result, 0);
    return result;
}

static int measured_fsyncdir (const struct usfs_client_request * request, const char * path, int datasync, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_FSYNCDIR);
    const int result = synchronize_file (request, path, datasync, file_info);
    finish_callback (&scope, CALLBACK_FSYNCDIR, result, 0);
    return result;
}

static int measured_releasedir (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    struct callback_scope scope = begin_callback (CALLBACK_RELEASEDIR);
    const int result = memfs_release (request, path, file_info);
    finish_callback (&scope, CALLBACK_RELEASEDIR, result, 0);
    return result;
}

static int measured_export_id (
    const struct usfs_client_request * request,
    const char * path,
    uint64_t dev,
    uint64_t ino,
    mode_t type,
    uint64_t * token
)
{
    struct callback_scope scope = begin_callback (CALLBACK_EXPORT_ID);
    const int result = export_object_id (request, path, dev, ino, type, token);
    finish_callback (&scope, CALLBACK_EXPORT_ID, result, 0);
    return result;
}

static int measured_resolve_id (const struct usfs_client_request * request, uint64_t token, char * path, size_t capacity)
{
    struct callback_scope scope = begin_callback (CALLBACK_RESOLVE_ID);
    const int result = resolve_object_id (request, token, path, capacity);
    finish_callback (&scope, CALLBACK_RESOLVE_ID, result, 0);
    return result;
}

static void measured_shutdown (const struct usfs_client_request * request)
{
    struct callback_scope scope = begin_callback (CALLBACK_SHUTDOWN);
    destroy_filesystem (request);
    finish_callback (&scope, CALLBACK_SHUTDOWN, 0, 0);
}

static const char * const callback_names[CALLBACK_COUNT] = { "initialize", "getattr",  "statfs",     "readlink",    "chmod",      "chown",
                                                             "truncate",   "utimens",  "mkdir",      "rmdir",       "unlink",     "rename",
                                                             "link",       "symlink",  "create",     "create_attr", "open",       "read",
                                                             "write",      "flush",    "fsync",      "release",     "syncfs",     "opendir",
                                                             "readdir",    "fsyncdir", "releasedir", "export_id",   "resolve_id", "shutdown" };

static const struct usfs_operations measured_operations = { .initialize = measured_initialize,
                                                            .getattr = measured_getattr,
                                                            .statfs = measured_statfs,
                                                            .readlink = measured_readlink,
                                                            .chmod = measured_chmod,
                                                            .chown = measured_chown,
                                                            .truncate = measured_truncate,
                                                            .utimens = measured_utimens,
                                                            .mkdir = measured_mkdir,
                                                            .rmdir = measured_rmdir,
                                                            .unlink = measured_unlink,
                                                            .rename = measured_rename,
                                                            .link = measured_link,
                                                            .symlink = measured_symlink,
                                                            .create = measured_create,
                                                            .create_attr = measured_create_attr,
                                                            .open = measured_open,
                                                            .read = measured_read,
                                                            .write = measured_write,
                                                            .flush = measured_flush,
                                                            .fsync = measured_fsync,
                                                            .release = measured_release,
                                                            .syncfs = measured_syncfs,
                                                            .opendir = measured_opendir,
                                                            .readdir = measured_readdir,
                                                            .fsyncdir = measured_fsyncdir,
                                                            .releasedir = measured_releasedir,
                                                            .export_id = measured_export_id,
                                                            .resolve_id = measured_resolve_id,
                                                            .shutdown = measured_shutdown };
