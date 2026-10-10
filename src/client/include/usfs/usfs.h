/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Native userspace filesystem interface for IBM AIX USFS.
 */
#ifndef USFS_CLIENT_PUBLIC_H
#define USFS_CLIENT_PUBLIC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USFS_CLIENT_API_VERSION     2u
#define USFS_CLIENT_DEFAULT_WORKERS 10u
#define USFS_CLIENT_MAX_WORKERS     64u
#define USFS_TIME_NOW               (-2L)
#define USFS_TIME_OMIT              (-3L)

struct usfs_client;
struct usfs_client_request;
struct usfs_directory_sink;

enum usfs_mount_access
{
    USFS_MOUNT_AUTOMATIC = 0,
    USFS_MOUNT_READ_ONLY = 1,
    USFS_MOUNT_READ_WRITE = 2
};

enum usfs_remove_policy
{
    USFS_REMOVE_DEFERRED = 0,
    USFS_REMOVE_IMMEDIATE = 1
};

enum usfs_path_policy
{
    USFS_PATH_DEFAULT = 0,
    USFS_PATH_OMIT_FOR_HANDLE = 1
};

enum usfs_create_action
{
    USFS_CREATE_WITHOUT_OPEN = 0,
    USFS_CREATE_FOR_LOOKUP = 1,
    USFS_CREATE_WITH_OPEN = 2
};

enum usfs_initial_attribute
{
    USFS_INITIAL_MODE = 1u << 0,
    USFS_INITIAL_UID = 1u << 1,
    USFS_INITIAL_GID = 1u << 2,
    USFS_INITIAL_SIZE = 1u << 3,
    USFS_INITIAL_ATIME = 1u << 4,
    USFS_INITIAL_MTIME = 1u << 5,
    USFS_INITIAL_CTIME = 1u << 6
};

struct usfs_open_file
{
    uint64_t value; // Backend-owned handle value; zero is valid.
    int open_flags; // POSIX open flags supplied to the backend.
};

/* Borrowed mount-scoped identity for an object request. The callback path is
 * supplied separately and can be NULL after the last name is removed. Cached
 * backend fields are optional; nodeid remains valid for the mount lifetime. */
struct usfs_object_identity
{
    uint64_t nodeid;          // Stable node identifier within this mount.
    uint64_t backend_dev;     // Cached backend device number when known.
    uint64_t backend_ino;     // Cached backend inode number when known.
    mode_t backend_type;      // Cached backend file type when known.
    int has_backend_identity; // Whether the cached backend fields are valid.
};

struct usfs_behavior
{
    enum usfs_remove_policy remove_policy; // Defer removal of open objects, or remove immediately.
    enum usfs_path_policy handle_paths;    // Prefer a known path, or omit it for handle operations.
};

struct usfs_limits
{
    uint32_t max_read;  // Maximum bytes requested in one read callback.
    uint32_t max_write; // Maximum bytes requested in one write callback.
};

/* Messages are borrowed, callbacks may overlap, and logging preserves errno. */
typedef void (*usfs_diagnostic_fn) (void * user_data, const char * message);

struct usfs_client_options
{
    enum usfs_mount_access mount_access; // Automatic access is inferred from mutating callbacks.
    struct usfs_behavior behavior;       // Initial policies, adjustable during initialization.
    usfs_diagnostic_fn diagnostic;       // Optional message recipient; NULL writes to stderr.
    void * diagnostic_data;              // Borrowed argument passed to the diagnostic recipient.
};

/*
 * Request objects, paths, and buffers are borrowed for the callback duration.
 * Callbacks return zero or negative errno; read/write return a byte count or
 * negative errno. Missing open/opendir/flush callbacks succeed; missing
 * release/releasedir callbacks require no backend cleanup. Other absent
 * operations report the engine's unsupported or read-only error. Automatic
 * mounting is read-only when no mutating callbacks are supplied.
 * Backends must synchronize their state when using more than one worker.
 * A callback must not synchronously access its own mount through the kernel.
 *
 * Application data remains the pointer supplied at creation. Initialization
 * runs once before dispatch; shutdown follows successful initialization and
 * release of remaining open handles. Lifecycle requests have zero credentials.
 * Failed initialization cleans its own partial state and receives no shutdown.
 * Behavior values changed by initialization must remain valid enum values.
 * readlink must terminate its successful result within the supplied capacity.
 * release/releasedir results are ignored, matching the existing engine policy.
 * create_attr receives an open-file pointer only for USFS_CREATE_WITH_OPEN.
 * A non-NULL open-file pointer represents an open instance even when value is
 * zero. A NULL pointer represents an operation without an open instance.
 * Handle operations may receive a NULL path for detached objects; choosing
 * USFS_PATH_OMIT_FOR_HANDLE also omits known paths for those operations.
 * utimens accepts USFS_TIME_NOW/USFS_TIME_OMIT in each tv_nsec field.
 */
struct usfs_operations
{
    int (*initialize
    ) (const struct usfs_client_request *,
       const struct usfs_limits *,
       struct usfs_behavior *);                            // Initialize backend state before the first dispatched request.
    void (*shutdown) (const struct usfs_client_request *); // Dispose backend state after remaining handles have been released.

    int (*getattr
    ) (const struct usfs_client_request *, const char *, struct stat *, struct usfs_open_file *);     // Read authoritative object attributes.
    int (*statfs) (const struct usfs_client_request *, const char *, struct statvfs *);               // Read capacity and free-space statistics.
    int (*readlink) (const struct usfs_client_request *, const char *, char *, size_t);               // Read a terminated symbolic-link target.
    int (*chmod) (const struct usfs_client_request *, const char *, mode_t, struct usfs_open_file *); // Change object permissions.
    int (*chown) (const struct usfs_client_request *, const char *, uid_t, gid_t, struct usfs_open_file *); // Change object owner and group.
    int (*truncate) (const struct usfs_client_request *, const char *, off_t, struct usfs_open_file *);     // Change the file length.
    int (*utimens
    ) (const struct usfs_client_request *,
       const char *,
       const struct timespec[2],
       struct usfs_open_file *); // Update access and modification timestamps.

    int (*mkdir) (const struct usfs_client_request *, const char *, mode_t);                           // Create a directory.
    int (*rmdir) (const struct usfs_client_request *, const char *);                                   // Remove an empty directory.
    int (*unlink) (const struct usfs_client_request *, const char *);                                  // Remove a file name.
    int (*rename) (const struct usfs_client_request *, const char *, const char *);                    // Move or replace a name.
    int (*link) (const struct usfs_client_request *, const char *, const char *);                      // Add a hard-link name.
    int (*symlink) (const struct usfs_client_request *, const char *, const char *);                   // Create a symbolic link.
    int (*create) (const struct usfs_client_request *, const char *, mode_t, struct usfs_open_file *); // Create and open a regular file.
    // Atomic creation: failure must leave neither a new name nor an open handle.
    int (*create_attr
    ) (const struct usfs_client_request *,
       const char *,
       const struct stat *,
       unsigned int,
       enum usfs_create_action,
       struct stat *,
       struct usfs_open_file *); // Create atomically with requested attributes and activation.

    int (*open) (const struct usfs_client_request *, const char *, struct usfs_open_file *); // Open a regular file and supply its backend handle.
    ssize_t (*read
    ) (const struct usfs_client_request *, const char *, char *, size_t, off_t, struct usfs_open_file *); // Read bytes at an explicit offset.
    ssize_t (*write
    ) (const struct usfs_client_request *, const char *, const char *, size_t, off_t, struct usfs_open_file *); // Write bytes at an explicit offset.
    int (*flush) (const struct usfs_client_request *, const char *, struct usfs_open_file *);                   // Flush per-open state.
    int (*fsync) (const struct usfs_client_request *, const char *, int, struct usfs_open_file *); // Synchronize file contents or metadata.
    int (*release) (const struct usfs_client_request *, const char *, struct usfs_open_file *);    // Release one regular-file handle.
    int (*syncfs) (const struct usfs_client_request *, const char *);                              // Synchronize the filesystem.

    int (*opendir) (const struct usfs_client_request *, const char *, struct usfs_open_file *); // Open a directory and supply its backend handle.
    // Add the complete listing to the sink; the client owns cursor management.
    int (*readdir
    ) (const struct usfs_client_request *,
       const char *,
       const struct usfs_object_identity *,
       struct usfs_directory_sink *);                                                                 // Produce a complete directory snapshot.
    int (*fsyncdir) (const struct usfs_client_request *, const char *, int, struct usfs_open_file *); // Synchronize directory state.
    int (*releasedir) (const struct usfs_client_request *, const char *, struct usfs_open_file *);    // Release one directory handle.

    // Provide both identity callbacks. Tokens are nonzero and never reused in a mount.
    int (*export_id
    ) (const struct usfs_client_request *,
       const char *,
       uint64_t,
       uint64_t,
       mode_t,
       uint64_t *);                                                                   // Issue a stable token for the supplied backend identity.
    int (*resolve_id) (const struct usfs_client_request *, uint64_t, char *, size_t); // Resolve an issued token to its current path.
};

struct usfs_client * usfs_request_client (const struct usfs_client_request * request);
void * usfs_request_user_data (const struct usfs_client_request * request);
uid_t usfs_request_uid (const struct usfs_client_request * request);
gid_t usfs_request_gid (const struct usfs_client_request * request);
pid_t usfs_request_pid (const struct usfs_client_request * request);
/* Returns a borrowed identity for vnode-only callbacks, open, and release;
 * NULL for requests without a single existing object. */
const struct usfs_object_identity * usfs_request_object_identity (const struct usfs_client_request * request);

/* A sink remembers its first error even if the backend continues adding entries. */
int usfs_directory_add (struct usfs_directory_sink * sink, const char * name, const struct stat * metadata);

/*
 * Creation copies the supplied options and complete operation-table members.
 * Members beyond operations_size are absent; a partial known member is rejected.
 * Larger tables use the known prefix. NULL operations requires size zero.
 * NULL options select zero-initialized defaults. On failure, *result is NULL.
 * Lifecycle status functions return zero or negative errno.
 */
int usfs_client_create (
    unsigned int api_version,
    const struct usfs_operations * operations,
    size_t operations_size,
    const struct usfs_client_options * options,
    void * application_data,
    struct usfs_client ** result
);
int usfs_client_mount (struct usfs_client * client, const char * mountpoint);
/* Run accepts 1..64 workers. A normal stop succeeds; fatal errors remain sticky.
 * Serialize lifecycle calls. request_stop may run concurrently and from a signal
 * handler while the caller ensures that the client remains alive. */
int usfs_client_run (struct usfs_client * client, unsigned int worker_count);
void usfs_client_request_stop (struct usfs_client * client);
int usfs_client_unmount (struct usfs_client * client);

/*
 * Destruction preserves *client when cleanup cannot start, including a live
 * mount. It sets *client to NULL after consuming storage, even when reporting
 * a deferred cleanup error. Callers may retry only while *client remains set.
 */
int usfs_client_destroy (struct usfs_client ** client);
int usfs_client_install_signals (struct usfs_client * client);
void usfs_client_remove_signals (struct usfs_client * client);

#ifdef __cplusplus
}
#endif
#endif
