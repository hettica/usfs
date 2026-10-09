/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#include "tap.h"

#include <usfs/usfs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define main memfs_program_main
#include "../../../examples/memfs.c"
#undef main

static int export_path_token (const char * path, uint64_t * token)
{
    struct stat metadata;
    const int rc = filesystem_operations.getattr (NULL, path, &metadata, NULL);
    if (rc != 0)
        return rc;

    return filesystem_operations.export_id (NULL, path, (uint64_t)metadata.st_dev, (uint64_t)metadata.st_ino, metadata.st_mode & S_IFMT, token);
}

static int token_resolves_to (const uint64_t token, const char * expected_path)
{
    char path[PATH_MAX] = { 0 };

    return filesystem_operations.resolve_id (NULL, token, path, sizeof (path)) == 0 && strcmp (path, expected_path) == 0;
}

struct fid_test_fixture
{
    struct usfs_open_file original_handle;    // Open handle retained across unlink and replacement checks.
    struct usfs_open_file replacement_handle; // Open handle for the new object at the reused pathname.
    uint64_t root_token;                      // Root inode identity exported by the backend.
    uint64_t directory_token;                 // Directory inode identity retained across rename.
    uint64_t file_token;                      // Original file identity retained across namespace changes.
    uint64_t symlink_token;                   // Symbolic link identity distinct from its target.
    uint64_t replacement_token;               // Identity of the new object at the original alias.
};

static int create_initial_objects (struct fid_test_fixture * fixture)
{
    if (filesystem_operations.mkdir (NULL, "/before", 0755) != 0)
        return false;

    if (filesystem_operations.create (NULL, "/before/file", 0644, &fixture->original_handle) != 0)
        return false;

    return filesystem_operations.symlink (NULL, "/before/file", "/before/sym") == 0;
}

static int export_initial_tokens (struct fid_test_fixture * fixture)
{
    if (export_path_token ("/", &fixture->root_token) != 0)
        return false;

    if (export_path_token ("/before", &fixture->directory_token) != 0)
        return false;

    if (export_path_token ("/before/file", &fixture->file_token) != 0)
        return false;

    if (export_path_token ("/before/sym", &fixture->symlink_token) != 0)
        return false;

    if (fixture->root_token != g_root->ino || fixture->directory_token <= UINT32_MAX || fixture->file_token <= UINT32_MAX ||
        fixture->symlink_token <= UINT32_MAX)
        return false;

    return fixture->directory_token != fixture->file_token && fixture->file_token != fixture->symlink_token &&
           fixture->directory_token != fixture->symlink_token;
}

static void test_initial_identities (struct tap_state * tap, struct fid_test_fixture * fixture)
{
    uint64_t detached_token = 0;

    tap_ok (tap, create_initial_objects (fixture), "real memfs callbacks create a directory, file, and symbolic link");

    tap_ok (tap, export_initial_tokens (fixture), "file, directory, and symlink tokens retain distinct high 32-bit identities");

    tap_ok (
        tap,
        token_resolves_to (fixture->root_token, "/") && token_resolves_to (fixture->directory_token, "/before") &&
            token_resolves_to (fixture->file_token, "/before/file") && token_resolves_to (fixture->symlink_token, "/before/sym"),
        "each exported token resolves to its original object path"
    );

    tap_ok (
        tap,
        filesystem_operations.export_id (NULL, "/before/file", 0, fixture->file_token, S_IFDIR, &detached_token) == -ESTALE,
        "export rejects a backend object type that conflicts with the path"
    );
}

static void test_rename_and_hardlink (struct tap_state * tap, const struct fid_test_fixture * fixture)
{
    uint64_t detached_token = 0;
    char short_path[2] = { 0 };

    tap_ok (tap, filesystem_operations.rename (NULL, "/before", "/after") == 0, "parent directory rename succeeds");

    tap_ok (
        tap,
        token_resolves_to (fixture->directory_token, "/after") && token_resolves_to (fixture->file_token, "/after/file") &&
            token_resolves_to (fixture->symlink_token, "/after/sym"),
        "parent rename preserves descendant tokens and updates their paths"
    );

    tap_ok (
        tap,
        filesystem_operations.link (NULL, "/after/file", "/after/alias") == 0 && filesystem_operations.unlink (NULL, "/after/file") == 0,
        "removing the original name leaves a surviving hard link"
    );

    tap_ok (
        tap,
        token_resolves_to (fixture->file_token, "/after/alias") &&
            filesystem_operations.export_id (NULL, NULL, 0, fixture->file_token, S_IFREG, &detached_token) == 0 &&
            detached_token == fixture->file_token,
        "surviving hard link and detached identity retain the same token"
    );

    tap_ok (
        tap,
        filesystem_operations.resolve_id (NULL, fixture->file_token, short_path, sizeof (short_path)) == -ENAMETOOLONG,
        "identifier resolution rejects insufficient pathname capacity"
    );
}

static void test_retirement_and_replacement (struct tap_state * tap, struct fid_test_fixture * fixture)
{
    uint64_t detached_token = 0;
    char short_path[2] = { 0 };

    tap_ok (
        tap,
        filesystem_operations.unlink (NULL, "/after/alias") == 0 && find_live_inode_by_id (fixture->file_token) != NULL &&
            filesystem_operations.resolve_id (NULL, fixture->file_token, short_path, sizeof (short_path)) == -ESTALE &&
            filesystem_operations.export_id (NULL, NULL, 0, fixture->file_token, S_IFREG, &detached_token) == -ESTALE,
        "final unlink makes a still-open inode's token stale"
    );

    tap_ok (
        tap,
        filesystem_operations.create (NULL, "/after/alias", 0644, &fixture->replacement_handle) == 0 &&
            export_path_token ("/after/alias", &fixture->replacement_token) == 0 && fixture->replacement_token != fixture->file_token &&
            filesystem_operations.resolve_id (NULL, fixture->file_token, short_path, sizeof (short_path)) == -ESTALE,
        "replacement at the same pathname receives a distinct token"
    );

    tap_ok (
        tap,
        filesystem_operations.release (NULL, "/after/file", &fixture->original_handle) == 0 && find_live_inode_by_id (fixture->file_token) == NULL,
        "closing the final old handle removes its retired inode from the index"
    );

    tap_ok (
        tap,
        token_resolves_to (fixture->replacement_token, "/after/alias") &&
            filesystem_operations.release (NULL, "/after/alias", &fixture->replacement_handle) == 0,
        "the replacement remains independently resolvable"
    );
}

static void test_token_exhaustion (struct tap_state * tap, const struct fid_test_fixture * fixture)
{
    struct usfs_open_file wrap_handle = { .open_flags = O_RDWR };
    struct usfs_open_file overflow_handle = { .open_flags = O_RDWR };
    uint64_t wrap_token = 0;

    g_next_ino = UINT64_MAX;
    tap_ok (
        tap,
        filesystem_operations.create (NULL, "/after/wrap", 0644, &wrap_handle) == 0 && export_path_token ("/after/wrap", &wrap_token) == 0 &&
            wrap_token == UINT64_MAX,
        "the final 64-bit inode token can be allocated"
    );

    tap_ok (
        tap,
        filesystem_operations.create (NULL, "/after/overflow", 0644, &overflow_handle) == -ENOSPC && find_live_inode_by_id (wrap_token) != NULL &&
            token_resolves_to (wrap_token, "/after/wrap") && token_resolves_to (fixture->root_token, "/"),
        "token wrap refuses reuse without invalidating existing identities"
    );

    (void)filesystem_operations.release (NULL, "/after/wrap", &wrap_handle);
}

int main (void)
{
    struct tap_state tap;
    struct fid_test_fixture fixture = { 0 };

    tap_plan (&tap, 15);

    g_root = inode_new (S_IFDIR | 0755, getuid (), getgid ());
    if (g_root == NULL)
    {
        tap_ok (&tap, 1, "memfs root initializes for identifier tests");
        return tap_finish (&tap);
    }

    g_root->nlink = 2;
    g_next_ino = UINT64_C (0x100000001);
    fixture.original_handle.open_flags = O_RDWR;
    fixture.replacement_handle.open_flags = O_RDWR;

    test_initial_identities (&tap, &fixture);
    test_rename_and_hardlink (&tap, &fixture);
    test_retirement_and_replacement (&tap, &fixture);
    test_token_exhaustion (&tap, &fixture);

    return tap_finish (&tap);
}
