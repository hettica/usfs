/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * USFS: native userspace file systems for AIX
 *
 * Read-only mirror of an existing directory tree.
 *
 * Pointed at a directory with --source, this daemon mounts a file system that
 * presents exactly that tree: same structure, same names, same attributes, same
 * symbolic links. Every mutation is refused. Unless a mount point is given on
 * the command line the file system is mounted at /mnt/<pid>/<basename of
 * source>, so pointing it at /usr mounts at /mnt/<pid>/usr.
 *
 * Compile with:
 *
 *     gcc -maix64 -pthread -Isrc/client/include mirror.c libusfs.a -lpthreads -o usfs_mirror
 *
 * Usage:
 *
 *     usfs_mirror --source=/usr [mountpoint]
 *
 * Only the read-side callbacks are implemented. The absence of the mutating
 * ones is the read-only statement: the kernel extension refuses those
 * operations with EROFS before they could reach a callback.
 */

#include "usfs_example.h"

#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* Root of the mirrored tree: absolute, resolved by realpath, never with a
   trailing slash and never "/" (see resolve_source_directory). */
static char * source_directory_path;
static size_t source_directory_path_length;
static struct stat source_root_attributes;
static int source_root_identity_known;

static struct options
{
    const char * source; // Source directory supplied through --source.
} options;

struct mirror_anchor
{
    struct mirror_anchor * next; // Next object retained by this mount.
    uint64_t nodeid;             // Stable mount-scoped object identity.
    unsigned open_refs;          // Open instances retaining this anchor.
    int descriptor;              // Owned descriptor for attributes and sync.
    DIR * directory_stream;      // Owned stream for a directory, or NULL for a file.
};

struct mirror_open
{
    struct mirror_anchor * anchor; // Shared object state retained by this open.
    int descriptor;                // Descriptor owned by this open instance.
};

static pthread_mutex_t anchor_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mirror_anchor * anchors;

static struct mirror_anchor * find_anchor (const uint64_t nodeid)
{
    for (struct mirror_anchor * anchor = anchors; anchor != NULL; anchor = anchor->next)
        if (anchor->nodeid == nodeid)
            return anchor;

    return NULL;
}

static int object_matches_identity (
    const struct stat * attributes,
    const struct usfs_object_identity * identity,
    const char * path
)
{
    if (identity == NULL)
        return true;

    if (identity->has_backend_identity)
        return (uint64_t)attributes->st_dev == identity->backend_dev &&
            (uint64_t)attributes->st_ino == identity->backend_ino &&
            (attributes->st_mode & S_IFMT) == identity->backend_type;

    if (path == NULL || strcmp (path, "/") != 0 || !source_root_identity_known)
        return false;

    return attributes->st_dev == source_root_attributes.st_dev &&
        attributes->st_ino == source_root_attributes.st_ino &&
        (attributes->st_mode & S_IFMT) == (source_root_attributes.st_mode & S_IFMT);
}


/*
 * Maps a path inside the mounted file system onto the real path under the
 * mirrored source. Returns 0, or -ENAMETOOLONG when the result would not fit.
 */
static int build_source_path (const char * path, char * source_path, const size_t source_path_capacity)
{
    int path_length;

    if (strcmp (path, "/") == 0)
        path_length = snprintf (source_path, source_path_capacity, "%s", source_directory_path);
    else
        path_length = snprintf (source_path, source_path_capacity, "%s%s", source_directory_path, path);

    if (path_length < 0)
        return -ENAMETOOLONG;

    if ((size_t)path_length >= source_path_capacity)
        return -ENAMETOOLONG;

    return 0;
}

static int initialize_filesystem (const struct usfs_client_request * request, const struct usfs_limits * limits, struct usfs_behavior * behavior)
{
    (void)request;
    (void)limits;
    (void)behavior;

    if (stat (source_directory_path, &source_root_attributes) != 0)
        return -errno;

    source_root_identity_known = true;
    return 0;
}

static void destroy_filesystem (const struct usfs_client_request * request)
{
    (void)request;

    pthread_mutex_lock (&anchor_lock);

    while (anchors != NULL)
    {
        struct mirror_anchor * anchor = anchors;
        anchors = anchor->next;

        if (anchor->directory_stream != NULL)
            closedir (anchor->directory_stream);

        close (anchor->descriptor);
        free (anchor);
    }

    pthread_mutex_unlock (&anchor_lock);

    free (source_directory_path);
    source_directory_path = NULL;
}

static int has_file_handle (const struct usfs_open_file * file_info)
{
    return file_info != NULL;
}

static int get_file_attributes (
    const struct usfs_client_request * request,
    const char * path,
    struct stat * file_attributes,
    struct usfs_open_file * file_info
)
{
    const struct usfs_object_identity * identity = usfs_request_object_identity (request);
    char source_path[PATH_MAX];

    if (has_file_handle (file_info))
    {
        const struct mirror_open * opened = (const struct mirror_open *)(uintptr_t)file_info->value;

        return fstat (opened->descriptor, file_attributes) == 0 ? 0 : -errno;
    }

    if (identity != NULL)
    {
        pthread_mutex_lock (&anchor_lock);
        const struct mirror_anchor * anchor = find_anchor (identity->nodeid);

        if (anchor != NULL)
        {
            const int rc = fstat (anchor->descriptor, file_attributes) == 0 ? 0 : -errno;

            pthread_mutex_unlock (&anchor_lock);
            return rc;
        }

        pthread_mutex_unlock (&anchor_lock);
    }

    if (path == NULL)
        return -ESTALE;

    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    /* lstat, not stat: a symbolic link must appear as a symbolic link,
       otherwise the mirror silently flattens the tree's structure. */
    if (lstat (source_path, file_attributes) == -1)
        return -errno;

    return object_matches_identity (file_attributes, identity, path) ? 0 : -ESTALE;
}

static int read_symbolic_link (const struct usfs_client_request * request, const char * path, char * buffer, size_t buffer_capacity)
{
    (void)request;
    char source_path[PATH_MAX];

    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    if (buffer_capacity == 0)
        return -EINVAL;

    const ssize_t target_length = readlink (source_path, buffer, buffer_capacity - 1);
    if (target_length == -1)
        return -errno;

    /* The high-level API expects a NUL-terminated target and a 0 return,
       not the length. */
    buffer[target_length] = '\0';

    return 0;
}

static int read_directory_entries (DIR * directory_stream, struct usfs_directory_sink * sink)
{
    for (;;)
    {
        errno = 0;
        /* cppcheck-suppress readdirCalled */
        const struct dirent * directory_entry = readdir (directory_stream);
        if (directory_entry == NULL)
            return errno == 0 ? 0 : -errno;

        struct stat file_attributes = { 0 };

        file_attributes.st_ino = directory_entry->d_ino;

        if (usfs_directory_add (sink, directory_entry->d_name, &file_attributes) != 0)
            return 0;
    }
}

static int read_directory (
    const struct usfs_client_request * request,
    const char * path,
    const struct usfs_object_identity * identity,
    struct usfs_directory_sink * sink
)
{
    char source_path[PATH_MAX];

    (void)request;

    if (identity == NULL)
        return -ESTALE;

    pthread_mutex_lock (&anchor_lock);
    struct mirror_anchor * anchor = find_anchor (identity->nodeid);

    if (anchor != NULL)
    {
        if (anchor->directory_stream == NULL)
        {
            pthread_mutex_unlock (&anchor_lock);
            return -ENOTDIR;
        }

        rewinddir (anchor->directory_stream);
        const int read_result = read_directory_entries (anchor->directory_stream, sink);

        pthread_mutex_unlock (&anchor_lock);
        return read_result;
    }

    pthread_mutex_unlock (&anchor_lock);

    if (path == NULL)
        return -ESTALE;

    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    DIR * directory_stream = opendir (source_path);
    if (directory_stream == NULL)
        return -errno;

    struct stat attributes;
    if (fstat (dirfd (directory_stream), &attributes) != 0)
    {
        const int error = errno;
        closedir (directory_stream);
        return -error;
    }

    if (!object_matches_identity (&attributes, identity, path))
    {
        closedir (directory_stream);
        return -ESTALE;
    }

    const int read_result = read_directory_entries (directory_stream, sink);
    const int close_result = closedir (directory_stream);
    const int close_error = close_result == 0 ? 0 : errno;

    if (read_result != 0)
        return read_result;

    return close_error == 0 ? 0 : -close_error;
}

static int retain_object_anchor (
    const struct usfs_object_identity * identity,
    const int file_descriptor,
    const int is_directory,
    struct mirror_anchor ** retained_anchor
)
{
    pthread_mutex_lock (&anchor_lock);

    struct mirror_anchor * anchor = find_anchor (identity->nodeid);

    if (anchor != NULL)
    {
        anchor->open_refs++;
        *retained_anchor = anchor;
        pthread_mutex_unlock (&anchor_lock);
        return 0;
    }

    anchor = calloc (1, sizeof (*anchor));
    if (anchor == NULL)
    {
        pthread_mutex_unlock (&anchor_lock);
        return -ENOMEM;
    }

    anchor->descriptor = dup (file_descriptor);
    if (anchor->descriptor == -1)
    {
        const int error = errno;
        free (anchor);
        pthread_mutex_unlock (&anchor_lock);
        return -error;
    }

    if (is_directory)
    {
        const int stream_descriptor = dup (file_descriptor);
        if (stream_descriptor == -1)
        {
            const int error = errno;
            close (anchor->descriptor);
            free (anchor);
            pthread_mutex_unlock (&anchor_lock);
            return -error;
        }

        anchor->directory_stream = fdopendir (stream_descriptor);
        if (anchor->directory_stream == NULL)
        {
            const int error = errno;
            close (stream_descriptor);
            close (anchor->descriptor);
            free (anchor);
            pthread_mutex_unlock (&anchor_lock);
            return -error;
        }
    }

    anchor->nodeid = identity->nodeid;
    anchor->open_refs = 1;
    anchor->next = anchors;
    anchors = anchor;
    *retained_anchor = anchor;

    pthread_mutex_unlock (&anchor_lock);
    return 0;
}

static int open_file (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    const struct usfs_object_identity * identity = usfs_request_object_identity (request);

    if (identity == NULL || path == NULL)
        return -ESTALE;

    char source_path[PATH_MAX];
    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    if ((file_info->open_flags & O_ACCMODE) != O_RDONLY)
        return -EROFS;

    const int file_descriptor = open (source_path, O_RDONLY);
    if (file_descriptor == -1)
        return -errno;

    struct stat attributes;
    if (fstat (file_descriptor, &attributes) != 0)
    {
        const int error = errno;
        close (file_descriptor);
        return -error;
    }

    if (!object_matches_identity (&attributes, identity, path))
    {
        close (file_descriptor);
        return -ESTALE;
    }

    struct mirror_open * opened = calloc (1, sizeof (*opened));
    if (opened == NULL)
    {
        close (file_descriptor);
        return -ENOMEM;
    }

    const int anchor_result = retain_object_anchor (identity, file_descriptor, S_ISDIR (attributes.st_mode), &opened->anchor);
    if (anchor_result != 0)
    {
        free (opened);
        close (file_descriptor);
        return anchor_result;
    }

    opened->descriptor = file_descriptor;
    file_info->value = (uint64_t)(uintptr_t)opened;

    return 0;
}

static int get_operation_file_descriptor (
    const struct usfs_client_request * request,
    const char * path,
    const struct usfs_open_file * file_info,
    int * file_descriptor
)
{
    if (has_file_handle (file_info))
    {
        const struct mirror_open * opened = (const struct mirror_open *)(uintptr_t)file_info->value;
        *file_descriptor = opened->descriptor;
        return 0;
    }

    const struct usfs_object_identity * identity = usfs_request_object_identity (request);
    if (identity != NULL)
    {
        pthread_mutex_lock (&anchor_lock);
        const struct mirror_anchor * anchor = find_anchor (identity->nodeid);

        if (anchor != NULL)
        {
            *file_descriptor = dup (anchor->descriptor);
            const int result = *file_descriptor == -1 ? -errno : 0;

            pthread_mutex_unlock (&anchor_lock);
            return result;
        }

        pthread_mutex_unlock (&anchor_lock);
    }

    if (path == NULL)
        return -ESTALE;

    char source_path[PATH_MAX];
    const int path_result = build_source_path (path, source_path, sizeof (source_path));
    if (path_result != 0)
        return path_result;

    *file_descriptor = open (source_path, O_RDONLY);
    if (*file_descriptor == -1)
        return -errno;

    struct stat attributes;
    if (fstat (*file_descriptor, &attributes) != 0)
    {
        const int error = errno;
        close (*file_descriptor);
        return -error;
    }

    if (!object_matches_identity (&attributes, identity, path))
    {
        close (*file_descriptor);
        return -ESTALE;
    }

    return 0;
}

static ssize_t read_file (
    const struct usfs_client_request * request,
    const char * path,
    char * buffer,
    const size_t requested_bytes,
    const off_t offset,
    struct usfs_open_file * file_info
)
{
    int file_descriptor;
    const int descriptor_result = get_operation_file_descriptor (request, path, file_info, &file_descriptor);
    if (descriptor_result != 0)
        return descriptor_result;

    /* Explicit offsets keep concurrent reads independent of the descriptor position. */
    const ssize_t bytes_read = pread (file_descriptor, buffer, requested_bytes, offset);
    const int read_result = bytes_read == -1 ? -errno : (int)bytes_read;

    if (!has_file_handle (file_info))
        (void)close (file_descriptor);

    return read_result;
}

static int release_file (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    (void)request;
    (void)path;

    if (has_file_handle (file_info))
    {
        struct mirror_open * opened = (struct mirror_open *)(uintptr_t)file_info->value;

        pthread_mutex_lock (&anchor_lock);

        struct mirror_anchor * anchor = opened->anchor;
        anchor->open_refs--;

        if (anchor->open_refs == 0)
        {
            struct mirror_anchor ** link = &anchors;
            while (*link != anchor)
                link = &(*link)->next;

            *link = anchor->next;

            if (anchor->directory_stream != NULL)
                closedir (anchor->directory_stream);

            close (anchor->descriptor);
            free (anchor);
        }

        pthread_mutex_unlock (&anchor_lock);

        close (opened->descriptor);
        free (opened);
        file_info->value = 0;
    }

    return 0;
}

static int synchronize_file (
    const struct usfs_client_request * request,
    const char * path,
    const int synchronize_data_only,
    struct usfs_open_file * file_info
)
{
    int file_descriptor;
    const int descriptor_result = get_operation_file_descriptor (request, path, file_info, &file_descriptor);
    if (descriptor_result != 0)
        return descriptor_result;

    const int operation_result = synchronize_data_only ? fdatasync (file_descriptor) : fsync (file_descriptor);
    const int synchronization_result = operation_result == 0 ? 0 : -errno;

    if (!has_file_handle (file_info))
        (void)close (file_descriptor);

    return synchronization_result;
}

static int synchronize_filesystem (const struct usfs_client_request * request, const char * path)
{
    (void)request;
    (void)path;
    /* This example exposes its backing tree read-only, so USFS cannot have
       introduced changes that require filesystem-wide persistence. */
    return 0;
}

static int get_filesystem_statistics (const struct usfs_client_request * request, const char * path, struct statvfs * filesystem_statistics)
{
    (void)request;
    char source_path[PATH_MAX];

    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    if (statvfs (source_path, filesystem_statistics) == -1)
        return -errno;

    return 0;
}

static const struct usfs_operations filesystem_operations = {
    .initialize = initialize_filesystem,
    .shutdown = destroy_filesystem,
    .getattr = get_file_attributes,
    .readlink = read_symbolic_link,
    .opendir = open_file,
    .readdir = read_directory,
    .releasedir = release_file,
    .open = open_file,
    .read = read_file,
    .fsync = synchronize_file,
    .fsyncdir = synchronize_file,
    .release = release_file,
    .statfs = get_filesystem_statistics,
    .syncfs = synchronize_filesystem,
};

static void print_help (void)
{
    printf ("Supported arguments: --source=<dir> [options] [mountpoint]\n\n");
    printf ("File-system specific options:\n"
            "    --source=<dir>      directory tree to mirror read-only\n"
            "                        (required)\n"
            "\n"
            "Without a mountpoint argument the file system is mounted at\n"
            "/mnt/<pid>/<basename of source> (created automatically).\n"
            "\n");
}

/* The value following -o is an option list, not a mountpoint. */


/*
 * Resolves --source into source_directory_path and validates it. Returns 0 on success.
 */
static int resolve_source_directory (void)
{
    char resolved_path[PATH_MAX];
    struct stat file_attributes;

    if (options.source == NULL)
    {
        fprintf (stderr, "Failed to select source directory: --source=<dir> is required\n");
        return -1;
    }

    /* realpath also collapses "//", ".", ".." and trailing slashes, and
       resolves a symlinked source, which is what the prefix should be. */
    if (realpath (options.source, resolved_path) == NULL)
    {
        fprintf (stderr, "Failed to resolve source directory %s: %s\n", options.source, strerror (errno));
        return -1;
    }

    if (stat (resolved_path, &file_attributes) == -1)
    {
        fprintf (stderr, "Failed to inspect source directory %s: %s\n", resolved_path, strerror (errno));
        return -1;
    }

    if (!S_ISDIR (file_attributes.st_mode))
    {
        fprintf (stderr, "Failed to select source directory %s: path is not a directory\n", resolved_path);
        return -1;
    }

    /* Mirroring "/" would necessarily contain the mount point below /mnt,
       which deadlocks this daemon (see is_mountpoint_outside_source). */
    if (strcmp (resolved_path, "/") == 0)
    {
        fprintf (
            stderr,
            "Failed to select source directory \"/\": it would "
            "contain this daemon's own mount point\n"
        );
        return -1;
    }

    source_directory_path = strdup (resolved_path);
    if (source_directory_path == NULL)
    {
        fprintf (stderr, "Failed to allocate source directory path: out of memory\n");
        return -1;
    }

    source_directory_path_length = strlen (source_directory_path);

    return 0;
}

/*
 * Refuses a mount point that lies inside the mirrored tree.
 *
 * This daemon serves requests one at a time, so if the tree it mirrors
 * contained its own mount point, a reader descending into that path would make
 * the kernel send a request to a daemon already blocked reading the very same
 * path. That wedges the mount permanently.
 */
static int is_mountpoint_outside_source (const char * mountpoint_path)
{
    char resolved_path[PATH_MAX];

    if (realpath (mountpoint_path, resolved_path) == NULL)
        return true; // The path may not have been created yet.

    if (strncmp (resolved_path, source_directory_path, source_directory_path_length) != 0)
        return true;

    const char path_separator = resolved_path[source_directory_path_length];
    if (path_separator != '/')
    {
        if (path_separator != '\0')
            return true;
    }

    fprintf (
        stderr,
        "Failed to select mountpoint %s: it lies inside source directory %s and would deadlock the filesystem\n",
        resolved_path,
        source_directory_path
    );
    return false;
}

struct automatic_mount_paths
{
    char parent_directory_path[PATH_MAX]; // Process-specific directory containing the mountpoint.
    char mountpoint_path[PATH_MAX];       // Automatically selected mountpoint; empty for an explicit one.
};

static int create_mount_directory (const char * directory_path)
{
    if (mkdir (directory_path, 0755) == 0)
        return 0;

    if (errno == EEXIST)
        return 0;

    fprintf (stderr, "Failed to create mount directory %s: %s\n", directory_path, strerror (errno));
    return -1;
}

static void remove_automatic_mount_directories (const struct automatic_mount_paths * mount_paths)
{
    if (mount_paths->mountpoint_path[0] == '\0')
        return;
    /* Best effort: a killed daemon can leave directories behind. */
    (void)rmdir (mount_paths->mountpoint_path);
    (void)rmdir (mount_paths->parent_directory_path);
}

static int run_filesystem (struct example_arguments * arguments, const struct automatic_mount_paths * mount_paths)
{
    const int command_result = run_example_client (arguments, &filesystem_operations);
    remove_automatic_mount_directories (mount_paths);
    return command_result;
}


static const char * get_source_directory_name (void)
{
    const char * final_separator = strrchr (source_directory_path, '/');
    if (final_separator == NULL)
        return "root";

    if (final_separator[1] == '\0')
        return "root";

    return final_separator + 1;
}

static int prepare_mount_arguments (struct example_arguments * arguments, struct automatic_mount_paths * mount_paths)
{
    const char * explicit_mountpoint = arguments->mountpoint;
    if (explicit_mountpoint != NULL)
    {
        if (!is_mountpoint_outside_source (explicit_mountpoint))
            return -1;

        return 0;
    }

    snprintf (mount_paths->parent_directory_path, sizeof (mount_paths->parent_directory_path), "/mnt/%d", (int)getpid ());
    const int mount_path_length = snprintf (
        mount_paths->mountpoint_path,
        sizeof (mount_paths->mountpoint_path),
        "%s/%s",
        mount_paths->parent_directory_path,
        get_source_directory_name ()
    );
    if (mount_path_length < 0)
    {
        fprintf (stderr, "Failed to format automatic mountpoint path: %s\n", strerror (errno));
        return -1;
    }

    if ((size_t)mount_path_length >= sizeof (mount_paths->mountpoint_path))
    {
        fprintf (
            stderr,
            "Failed to format automatic mountpoint path: path exceeds %lu bytes\n",
            (unsigned long)sizeof (mount_paths->mountpoint_path) - 1
        );
        return -1;
    }

    if (create_mount_directory (mount_paths->parent_directory_path) != 0)
        return -1;

    if (create_mount_directory (mount_paths->mountpoint_path) != 0)
        return -1;

    if (!is_mountpoint_outside_source (mount_paths->mountpoint_path))
    {
        remove_automatic_mount_directories (mount_paths);
        return -1;
    }

    arguments->mountpoint = mount_paths->mountpoint_path;
    printf ("Mirroring directory %s at %s\n", source_directory_path, mount_paths->mountpoint_path);
    fflush (stdout);
    return 0;
}

int main (const int argc, char * argv[])
{
    struct example_arguments arguments = { 0 };
    struct automatic_mount_paths mount_paths = { 0 };
    arguments.workers = USFS_CLIENT_DEFAULT_WORKERS;
    for (int index = 1; index < argc; ++index)
    {
        if (strncmp (argv[index], "--source=", 9) == 0)
        {
            options.source = argv[index] + 9;
            continue;
        }
        if (parse_example_argument (&arguments, argc, argv, &index) <= 0)
        {
            fprintf (stderr, "Failed to parse arguments: unsupported option or invalid value\n");
            return EXIT_FAILURE;
        }
    }
    if (arguments.help)
        print_help ();
    if (arguments.help || arguments.version)
        return run_example_client (&arguments, &filesystem_operations);

    if (resolve_source_directory () != 0)
        return EXIT_FAILURE;
    if (prepare_mount_arguments (&arguments, &mount_paths) != 0)
        return EXIT_FAILURE;

    return run_filesystem (&arguments, &mount_paths);
}
