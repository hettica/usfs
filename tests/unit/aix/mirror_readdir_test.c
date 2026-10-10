/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#include "tap.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum
{
    TEST_DIRECTORY_INODE = 42
};

static int directory_marker;
static int open_error;
static int read_error;
static int close_error;
static int filler_result;
static unsigned entries_remaining;
static unsigned read_calls;
static unsigned close_calls;
static unsigned filled_entries;
static struct dirent directory_entry;
static unsigned descriptor_open_calls;
static unsigned descriptor_stat_calls;
static unsigned descriptor_close_calls;

static int open_test_file (const char * path, int flags, ...)
{
    if (strcmp (path, "/source") != 0 || flags != O_RDONLY)
        abort ();

    ++descriptor_open_calls;
    return 0;
}

static int stat_test_file (int descriptor, struct stat * attributes)
{
    if (descriptor != 0)
        abort ();

    ++descriptor_stat_calls;
    memset (attributes, 0, sizeof (*attributes));
    attributes->st_mode = S_IFDIR;
    attributes->st_ino = TEST_DIRECTORY_INODE;
    return 0;
}

static int close_test_file (int descriptor)
{
    if (descriptor != 0)
        abort ();

    ++descriptor_close_calls;
    return 0;
}

static DIR * open_test_directory (const char * path)
{
    if (strcmp (path, "/source") != 0)
        abort ();

    if (open_error != 0)
    {
        errno = open_error;
        return NULL;
    }

    return (DIR *)&directory_marker;
}

static struct dirent * read_test_directory (DIR * directory_stream)
{
    if (directory_stream != (DIR *)&directory_marker)
        abort ();

    ++read_calls;

    if (entries_remaining != 0)
    {
        --entries_remaining;
        directory_entry.d_ino = TEST_DIRECTORY_INODE;
        strcpy (directory_entry.d_name, "entry");
        return &directory_entry;
    }

    if (read_error != 0)
        errno = read_error;

    return NULL;
}

static int close_test_directory (DIR * directory_stream)
{
    if (directory_stream != (DIR *)&directory_marker)
        abort ();

    ++close_calls;

    if (close_error != 0)
    {
        errno = close_error;
        return -1;
    }

    return 0;
}

#define open(...)          open_test_file (__VA_ARGS__)
#define fstat(...)         stat_test_file (__VA_ARGS__)
#define close(...)         close_test_file (__VA_ARGS__)
#define opendir(...)       open_test_directory (__VA_ARGS__)
#define dirfd(...)         0
#define readdir            read_test_directory
#define closedir           close_test_directory
#define usfs_directory_add fill_test_directory
#define main               mirror_program_main
#include "../../../examples/mirror.c"
#undef main
#undef usfs_directory_add
#undef closedir
#undef readdir
#undef opendir
#undef dirfd
#undef close
#undef fstat
#undef open

int fill_test_directory (struct usfs_directory_sink * buffer, const char * name, const struct stat * attributes)
{
    (void)buffer;

    if (strcmp (name, "entry") != 0 || attributes->st_ino != TEST_DIRECTORY_INODE)
        abort ();

    ++filled_entries;
    /* A callback's errno must not turn the next clean EOF into an error. */
    errno = EACCES;
    return filler_result;
}

static void reset_directory_case (void)
{
    open_error = 0;
    read_error = 0;
    close_error = 0;
    filler_result = 0;
    entries_remaining = 1;
    read_calls = 0;
    close_calls = 0;
    filled_entries = 0;
    source_directory_path = "/source";
    memset (&source_root_attributes, 0, sizeof (source_root_attributes));
    source_root_attributes.st_ino = TEST_DIRECTORY_INODE;
    source_root_attributes.st_mode = S_IFDIR;
    source_root_identity_known = 1;
}

static int run_directory_case (void)
{
    const struct usfs_object_identity identity = { .nodeid = 1 };

    return read_directory (NULL, "/", &identity, NULL);
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, 9);

    reset_directory_case ();
    const struct usfs_object_identity unknown_identity = { .nodeid = 9 };
    struct stat attributes = { .st_ino = TEST_DIRECTORY_INODE, .st_mode = S_IFDIR };
    tap_ok (&tap, !object_matches_identity (&attributes, &unknown_identity, "/child"),
            "a path fallback without a verifiable object identity fails closed");

    reset_directory_case ();
    tap_ok (
        &tap,
        run_directory_case () == 0 && filled_entries == 1 && read_calls == 2 && close_calls == 1,
        "clean directory EOF ignores errno left by the filler"
    );

    reset_directory_case ();
    entries_remaining = 0;
    errno = EIO;
    tap_ok (&tap, run_directory_case () == 0 && filled_entries == 0 && close_calls == 1, "empty directory EOF ignores stale incoming errno");

    reset_directory_case ();
    read_error = EIO;
    tap_ok (
        &tap,
        run_directory_case () == -EIO && filled_entries == 1 && close_calls == 1,
        "read failure after an entry rejects the partial listing and closes the stream"
    );

    reset_directory_case ();
    close_error = EBADF;
    tap_ok (&tap, run_directory_case () == -EBADF && read_calls == 2 && close_calls == 1, "close failure rejects an otherwise complete listing");

    reset_directory_case ();
    read_error = EIO;
    close_error = EBADF;
    tap_ok (&tap, run_directory_case () == -EIO && close_calls == 1, "read failure retains precedence over a close failure");

    reset_directory_case ();
    filler_result = 1;
    read_error = EIO;
    tap_ok (
        &tap,
        run_directory_case () == 0 && read_calls == 1 && close_calls == 1,
        "filler saturation stops successfully without reading another entry"
    );

    reset_directory_case ();
    filler_result = 1;
    close_error = EBADF;
    tap_ok (&tap, run_directory_case () == -EBADF && read_calls == 1 && close_calls == 1, "filler saturation still reports close failure");

    reset_directory_case ();
    open_error = EACCES;
    tap_ok (
        &tap,
        run_directory_case () == -EACCES && read_calls == 0 && close_calls == 0,
        "open failure returns its error without touching a stream"
    );

    return tap_finish (&tap);
}
