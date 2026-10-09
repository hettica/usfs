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
 *     gcc -maix64 -Isrc/client/include mirror.c libusfs.a -o usfs_mirror
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

static struct options
{
    const char * source; // Source directory supplied through --source.
} options;


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
    return 0;
}

static void destroy_filesystem (const struct usfs_client_request * request)
{
    (void)request;

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
    (void)request;
    char source_path[PATH_MAX];

    if (has_file_handle (file_info))
        return fstat ((int)file_info->value, file_attributes) == 0 ? 0 : -errno;

    if (path == NULL)
        return -ESTALE;

    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    /* lstat, not stat: a symbolic link must appear as a symbolic link,
       otherwise the mirror silently flattens the tree's structure. */
    if (lstat (source_path, file_attributes) == -1)
        return -errno;

    return 0;
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
    struct usfs_open_file * file_info,
    struct usfs_directory_sink * sink
)
{
    char source_path[PATH_MAX];

    (void)request;
    (void)file_info;

    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    /* A separate stream owns this listing's cursor. The persistent directory
       descriptor remains available for handle-based metadata operations. */
    DIR * directory_stream = opendir (source_path);
    if (directory_stream == NULL)
        return -errno;

    const int read_result = read_directory_entries (directory_stream, sink);
    const int close_result = closedir (directory_stream);
    const int close_error = close_result == 0 ? 0 : errno;

    if (read_result != 0)
        return read_result;

    return close_error == 0 ? 0 : -close_error;
}

static int open_file (const struct usfs_client_request * request, const char * path, struct usfs_open_file * file_info)
{
    (void)request;
    char source_path[PATH_MAX];
    const int operation_result = build_source_path (path, source_path, sizeof (source_path));
    if (operation_result != 0)
        return operation_result;

    if ((file_info->open_flags & O_ACCMODE) != O_RDONLY)
        return -EROFS;

    const int file_descriptor = open (source_path, O_RDONLY);
    if (file_descriptor == -1)
        return -errno;

    /* A present native handle carries the descriptor directly, including zero.
       Ownership transfers to release_file (). */
    file_info->value = (uint64_t)file_descriptor;

    /* cppcheck-suppress resourceLeak */
    return 0;
}

static int get_operation_file_descriptor (const char * path, const struct usfs_open_file * file_info, int * file_descriptor)
{
    if (has_file_handle (file_info))
    {
        *file_descriptor = (int)file_info->value;
        return 0;
    }

    char source_path[PATH_MAX];
    const int path_result = build_source_path (path, source_path, sizeof (source_path));
    if (path_result != 0)
        return path_result;

    *file_descriptor = open (source_path, O_RDONLY);
    if (*file_descriptor == -1)
        return -errno;

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
    (void)request;
    int file_descriptor;
    const int descriptor_result = get_operation_file_descriptor (path, file_info, &file_descriptor);
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
        close ((int)file_info->value);
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
    (void)request;
    int file_descriptor;
    const int descriptor_result = get_operation_file_descriptor (path, file_info, &file_descriptor);
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
