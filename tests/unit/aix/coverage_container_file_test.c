/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#include "tap.h"
#define main coverage_tool_main
#include "../../../src/tools/usfs_coverage.c"
#undef main

static int create_fixture_file (const char * path, const char * contents)
{
    const int descriptor = open (path, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (descriptor < 0)
        abort ();

    const size_t length = strlen (contents);
    if (write (descriptor, contents, length) != (ssize_t)length || lseek (descriptor, 0, SEEK_SET) != 0)
        abort ();

    return descriptor;
}

int main (void)
{
    struct tap_state tap;
    char directory[] = "/tmp/usfs-container-security-XXXXXX";
    char path[PATH_BUFFER], alias[PATH_BUFFER];

    tap_plan (&tap, 6);
    if (mkdtemp (directory) == NULL)
        abort ();

    snprintf (path, sizeof (path), "%s/container", directory);
    snprintf (alias, sizeof (alias), "%s/alias", directory);
    int descriptor = create_fixture_file (path, "original");
    unsigned char * contents = NULL;
    unsigned length = 0;
    int rc = read_container_file (descriptor, &contents, &length);

    tap_ok (
        &tap,
        rc == 0 && length == strlen ("original") && memcmp (contents, "original", length) == 0,
        "container contents are read from the opened regular file"
    );
    free (contents);
    contents = NULL;

    if (unlink (path) != 0 || lseek (descriptor, 0, SEEK_SET) != 0)
        abort ();

    const int replacement = create_fixture_file (path, "replacement");
    rc = read_container_file (descriptor, &contents, &length);
    tap_ok (
        &tap,
        rc == 0 && length == strlen ("original") && memcmp (contents, "original", length) == 0,
        "replacing the pathname cannot redirect reads from the opened container"
    );
    free (contents);
    contents = NULL;
    close (descriptor);
    close (replacement);

    if (symlink (path, alias) != 0)
        abort ();

    tap_ok (&tap, unpack_container_file (alias, directory) != 0, "container unpack refuses symbolic links");
    unlink (alias);
    if (mkfifo (alias, S_IRUSR | S_IWUSR) != 0)
        abort ();

    tap_ok (&tap, unpack_container_file (alias, directory) != 0, "container unpack rejects a FIFO without waiting for a writer");
    unlink (alias);

    descriptor = open (directory, O_RDONLY);
    if (descriptor < 0)
        abort ();

    tap_ok (
        &tap,
        read_container_file (descriptor, &contents, &length) != 0 && contents == NULL,
        "container reader rejects a directory before allocation"
    );
    close (descriptor);

    descriptor = open (path, O_RDWR);
    if (descriptor < 0 || ftruncate (descriptor, (off_t)USFS_GCOV_MAX_SNAPSHOT + 1) != 0)
        abort ();

    tap_ok (
        &tap,
        read_container_file (descriptor, &contents, &length) != 0 && contents == NULL,
        "oversized containers are rejected using the opened file metadata"
    );
    close (descriptor);
    unlink (path);
    rmdir (directory);
    return tap_finish (&tap);
}
