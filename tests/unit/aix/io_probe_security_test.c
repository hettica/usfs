/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#define _LARGE_FILES
#include "tap.h"
#define main io_probe_program_main
#include "../../../src/tools/usfs_io_probe.c"
#undef main

int main (void)
{
    struct tap_state tap;
    char directory[] = "/tmp/usfs-alias-security-XXXXXX";
    char path[PATH_MAX], alias[PATH_MAX];

    tap_plan (&tap, 4);
    if (mkdtemp (directory) == NULL)
        abort ();

    snprintf (path, sizeof (path), "%s/file", directory);
    snprintf (alias, sizeof (alias), "%s/alias", directory);
    const int owned = open (path, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (owned < 0 || link (path, alias) != 0)
        abort ();

    int reopened = open_probe_alias (owned, alias, O_RDWR);
    tap_ok (&tap, reopened >= 0, "probe accepts a hard-link alias of its owned inode");
    if (reopened >= 0)
        close (reopened);

    if (unlink (alias) != 0)
        abort ();

    const int replacement = open (alias, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (replacement < 0)
        abort ();

    reopened = open_probe_alias (owned, alias, O_RDWR);
    tap_ok (&tap, reopened < 0 && errno == ESTALE, "probe rejects a replacement inode before mapping or writing");
    if (reopened >= 0)
        close (reopened);

    close (replacement);
    if (unlink (alias) != 0 || symlink (path, alias) != 0)
        abort ();

    reopened = open_probe_alias (owned, alias, O_RDONLY);
    tap_ok (&tap, reopened < 0, "probe refuses a substituted symbolic link");
    if (reopened >= 0)
        close (reopened);

    if (unlink (alias) != 0 || mkfifo (alias, S_IRUSR | S_IWUSR) != 0)
        abort ();

    reopened = open_probe_alias (owned, alias, O_RDONLY);
    tap_ok (&tap, reopened < 0 && errno == ESTALE, "probe rejects a substituted FIFO without blocking");
    if (reopened >= 0)
        close (reopened);

    close (owned);
    unlink (alias);
    unlink (path);
    rmdir (directory);
    return tap_finish (&tap);
}
