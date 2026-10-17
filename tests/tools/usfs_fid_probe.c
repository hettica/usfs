/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "usfs_test.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum
{
    FID_RECORD_MAGIC = 0x55464944u,
    FID_RECORD_VERSION = 1,
    FID_PAYLOAD_LENGTH = 16,
    FID_VERSION_OFFSET = 4,
    FID_RESERVED_OFFSET = 6,
    FID_MOUNT_SERIAL_OFFSET = 0,
    FID_RESOLVE_REPEAT_LIMIT = 1000
};

struct fid_record
{
    uint32_t magic;                                         // Identifies a probe record.
    uint32_t version;                                       // Format version of the saved record.
    uint32_t vnode_type;                                    // Native AIX vnode type captured with the FID.
    uint32_t reserved;                                      // Zero for future format changes.
    uint64_t inode;                                         // Backend inode captured with the FID.
    unsigned char file_id[USFS_TEST_FID_RAW_BYTES];         // Complete native AIX fileid.
    unsigned char file_system_id[USFS_TEST_FSID_RAW_BYTES]; // Native AIX fsid for direct fidtovp.
};

typedef char usfs_probe_fileid_size_check[(sizeof (struct fileid) == USFS_TEST_FID_RAW_BYTES) ? 1 : -1];

static int copy_probe_path (char * destination, const char * source)
{
    if (source[0] != '/')
    {
        fprintf (stderr, "Invalid probe path %s: expected an absolute path\n", source);
        return -1;
    }

    const size_t length = strlen (source);
    if (length >= USFS_TEST_FID_PATH_MAX)
    {
        fprintf (stderr, "Invalid probe path %s: exceeds %u bytes\n", source, USFS_TEST_FID_PATH_MAX - 1u);
        return -1;
    }

    memcpy (destination, source, length + 1u);
    return 0;
}

static int read_record (const char * record_path, struct fid_record * record)
{
    FILE * record_file = fopen (record_path, "rb");
    if (record_file == NULL)
    {
        fprintf (stderr, "Failed to open FID record %s: %s\n", record_path, strerror (errno));
        return -1;
    }

    const size_t read_count = fread (record, 1, sizeof (*record), record_file);
    const int trailing_byte = fgetc (record_file);
    const int read_failed = ferror (record_file);
    const int close_rc = fclose (record_file);

    if (read_count != sizeof (*record) || trailing_byte != EOF || read_failed || close_rc != 0)
    {
        fprintf (stderr, "Failed to read FID record %s: expected exactly %lu bytes\n", record_path, (unsigned long)sizeof (*record));
        return -1;
    }

    if (record->magic != FID_RECORD_MAGIC || record->version != FID_RECORD_VERSION || record->reserved != 0)
    {
        fprintf (stderr, "Invalid FID record %s: unsupported record header\n", record_path);
        return -1;
    }

    return 0;
}

static int write_record (const char * record_path, const struct fid_record * record)
{
    FILE * record_file = fopen (record_path, "wb");
    if (record_file == NULL)
    {
        fprintf (stderr, "Failed to create FID record %s: %s\n", record_path, strerror (errno));
        return -1;
    }

    const size_t written_count = fwrite (record, 1, sizeof (*record), record_file);
    const int write_failed = ferror (record_file);
    const int close_rc = fclose (record_file);

    if (written_count != sizeof (*record) || write_failed || close_rc != 0)
    {
        fprintf (stderr, "Failed to write FID record %s: incomplete record\n", record_path);
        return -1;
    }

    return 0;
}

static int issue_probe (const int device_descriptor, struct usfs_test_fid_probe * request)
{
    return ioctl (device_descriptor, USFS_TEST_IOC_FID_PROBE, request);
}

static void fill_record (struct fid_record * record, const struct usfs_test_fid_probe * request)
{
    memset (record, 0, sizeof (*record));
    record->magic = FID_RECORD_MAGIC;
    record->version = FID_RECORD_VERSION;
    record->vnode_type = request->vnode_type;
    record->inode = request->inode;
    memcpy (record->file_id, request->file_id, sizeof (record->file_id));
    memcpy (record->file_system_id, request->file_system_id, sizeof (record->file_system_id));
}

static void fill_resolve_request (struct usfs_test_fid_probe * request, const struct fid_record * record)
{
    request->abi_version = USFS_TEST_ABI_VERSION;
    request->action = USFS_TEST_FID_RESOLVE;
    request->vnode_type = record->vnode_type;
    request->inode = record->inode;
    memcpy (request->file_id, record->file_id, sizeof (request->file_id));
    memcpy (request->file_system_id, record->file_system_id, sizeof (request->file_system_id));
}

static int capture_path (const int device_descriptor, const char * object_path, const char * record_path)
{
    struct usfs_test_fid_probe request = { 0 };

    request.abi_version = USFS_TEST_ABI_VERSION;
    request.action = USFS_TEST_FID_CAPTURE;
    if (copy_probe_path (request.path, object_path) != 0)
        return 1;

    if (issue_probe (device_descriptor, &request) != 0)
    {
        fprintf (stderr, "Failed to capture native FID for %s: %s\n", object_path, strerror (errno));
        return 1;
    }

    struct fid_record record = { 0 };
    fill_record (&record, &request);
    return write_record (record_path, &record) == 0 ? 0 : 1;
}

static int capture_stale_path (const int device_descriptor, const char * object_path)
{
    struct usfs_test_fid_probe request = { 0 };

    request.abi_version = USFS_TEST_ABI_VERSION;
    request.action = USFS_TEST_FID_CAPTURE;
    if (copy_probe_path (request.path, object_path) != 0)
        return 1;

    errno = 0;
    const int rc = issue_probe (device_descriptor, &request);
    const int observed_error = errno;

    if (rc == -1 && observed_error == ESTALE)
        return 0;

    fprintf (stderr, "Failed force-unmount FID check for %s: expected ESTALE, observed result %d and errno %d\n", object_path, rc, observed_error);
    return 1;
}

static int prepare_resolve_request (
    struct usfs_test_fid_probe * request,
    const char * mount_path,
    const char * record_path,
    const char * expected_path
)
{
    struct fid_record record = { 0 };
    if (read_record (record_path, &record) != 0)
        return -1;

    memset (request, 0, sizeof (*request));
    fill_resolve_request (request, &record);

    if (copy_probe_path (request->path, mount_path) != 0)
        return -1;

    if (expected_path != NULL && copy_probe_path (request->expected_path, expected_path) != 0)
        return -1;

    return 0;
}

static int resolve_record (
    const int device_descriptor,
    const char * mount_path,
    const char * record_path,
    const char * expected_path,
    const int allow_stale
)
{
    struct usfs_test_fid_probe request = { 0 };
    if (prepare_resolve_request (&request, mount_path, record_path, expected_path) != 0)
        return 1;

    if (issue_probe (device_descriptor, &request) == 0)
        return 0;

    if (allow_stale && errno == ESTALE)
        return 0;

    fprintf (stderr, "Failed to resolve native FID from %s: %s\n", record_path, strerror (errno));
    return 1;
}

static int resolve_record_repeatedly (
    const int device_descriptor,
    const char * mount_path,
    const char * record_path,
    const char * expected_path,
    const char * repeat_text
)
{
    char * parse_end = NULL;
    errno = 0;
    const unsigned long repeat_count = strtoul (repeat_text, &parse_end, 10);
    if (errno != 0 || parse_end == repeat_text || *parse_end != '\0' || repeat_count == 0 || repeat_count > FID_RESOLVE_REPEAT_LIMIT)
    {
        fprintf (stderr, "Invalid FID resolve count %s: expected 1 through %d\n", repeat_text, FID_RESOLVE_REPEAT_LIMIT);
        return 1;
    }

    for (unsigned long iteration = 0; iteration < repeat_count; iteration++)
    {
        if (resolve_record (device_descriptor, mount_path, record_path, expected_path, false) != 0)
            return 1;
    }

    return 0;
}

static int expect_probe_error (const int device_descriptor, struct usfs_test_fid_probe * request, const int expected_error, const char * case_name)
{
    errno = 0;
    const int rc = issue_probe (device_descriptor, request);
    const int observed_error = errno;

    if (rc == -1 && observed_error == expected_error)
        return 0;

    fprintf (stderr, "Failed %s FID check: expected errno %d, observed result %d and errno %d\n", case_name, expected_error, rc, observed_error);
    return 1;
}

static int expect_stale (const int device_descriptor, const char * mount_path, const char * record_path)
{
    struct usfs_test_fid_probe request = { 0 };
    if (prepare_resolve_request (&request, mount_path, record_path, NULL) != 0)
        return 1;

    return expect_probe_error (device_descriptor, &request, ESTALE, "stale");
}

static int expect_unsupported (const int device_descriptor, const char * object_path)
{
    struct usfs_test_fid_probe request = { 0 };

    request.abi_version = USFS_TEST_ABI_VERSION;
    request.action = USFS_TEST_FID_CAPTURE;
    if (copy_probe_path (request.path, object_path) != 0)
        return 1;

    return expect_probe_error (device_descriptor, &request, EOPNOTSUPP, "unsupported backend");
}

static int expect_malformed (const int device_descriptor, const char * mount_path, const char * record_path)
{
    struct usfs_test_fid_probe original = { 0 };
    if (prepare_resolve_request (&original, mount_path, record_path, NULL) != 0)
        return 1;

    struct usfs_test_fid_probe changed = original;
    struct fileid file_id = { 0 };

    memcpy (&file_id, changed.file_id, sizeof (file_id));
    file_id.fid_len = FID_PAYLOAD_LENGTH - 1;
    memcpy (changed.file_id, &file_id, sizeof (file_id));
    if (expect_probe_error (device_descriptor, &changed, EINVAL, "bad length") != 0)
        return 1;

    changed = original;
    changed.file_id[offsetof (struct fileid, fid_x) + FID_VERSION_OFFSET] ^= 1u;
    if (expect_probe_error (device_descriptor, &changed, EINVAL, "bad version") != 0)
        return 1;

    changed = original;
    changed.file_id[offsetof (struct fileid, fid_x) + FID_RESERVED_OFFSET] = 1u;
    if (expect_probe_error (device_descriptor, &changed, EINVAL, "reserved bytes") != 0)
        return 1;

    changed = original;
    memcpy (&file_id, changed.file_id, sizeof (file_id));
    file_id.fid_ino = 0;
    file_id.fid_gen = 0;
    memcpy (changed.file_id, &file_id, sizeof (file_id));
    if (expect_probe_error (device_descriptor, &changed, EINVAL, "zero token") != 0)
        return 1;

    changed = original;
    changed.file_id[offsetof (struct fileid, fid_x) + FID_MOUNT_SERIAL_OFFSET] ^= 1u;
    return expect_probe_error (device_descriptor, &changed, ESTALE, "obsolete mount serial");
}

static void print_usage (void)
{
    fprintf (stderr, "usage: usfs_fid_probe capture ABS_OBJECT_PATH RECORD_FILE\n");
    fprintf (stderr, "       usfs_fid_probe resolve ABS_MOUNT_PATH RECORD_FILE [ABS_EXPECTED_PATH]\n");
    fprintf (stderr, "       usfs_fid_probe stale|malformed|resolve-or-stale ABS_MOUNT_PATH RECORD_FILE\n");
    fprintf (stderr, "       usfs_fid_probe capture-stale ABS_OBJECT_PATH\n");
    fprintf (stderr, "       usfs_fid_probe resolve-repeat ABS_MOUNT_PATH RECORD_FILE ABS_EXPECTED_PATH COUNT\n");
    fprintf (stderr, "       usfs_fid_probe unsupported ABS_OBJECT_PATH\n");
}

static int run_command (const int device_descriptor, const int argc, char ** argv)
{
    const char * command = argv[1];

    if (strcmp (command, "capture") == 0 && argc == 4)
        return capture_path (device_descriptor, argv[2], argv[3]);

    if (strcmp (command, "capture-stale") == 0 && argc == 3)
        return capture_stale_path (device_descriptor, argv[2]);

    if (strcmp (command, "resolve") == 0 && (argc == 4 || argc == 5))
        return resolve_record (device_descriptor, argv[2], argv[3], argc == 5 ? argv[4] : NULL, false);

    if (strcmp (command, "resolve-or-stale") == 0 && argc == 4)
        return resolve_record (device_descriptor, argv[2], argv[3], NULL, true);

    if (strcmp (command, "resolve-repeat") == 0 && argc == 6)
        return resolve_record_repeatedly (device_descriptor, argv[2], argv[3], argv[4], argv[5]);

    if (strcmp (command, "stale") == 0 && argc == 4)
        return expect_stale (device_descriptor, argv[2], argv[3]);

    if (strcmp (command, "malformed") == 0 && argc == 4)
        return expect_malformed (device_descriptor, argv[2], argv[3]);

    if (strcmp (command, "unsupported") == 0 && argc == 3)
        return expect_unsupported (device_descriptor, argv[2]);

    print_usage ();
    return 1;
}

int main (const int argc, char ** argv)
{
    if (argc < 2)
    {
        print_usage ();
        return 1;
    }

    const int device_descriptor = open ("/dev/usfs0", O_RDWR);
    if (device_descriptor < 0)
    {
        fprintf (stderr, "Failed to open USFS control device /dev/usfs0: %s\n", strerror (errno));
        return 1;
    }

    const int rc = run_command (device_descriptor, argc, argv);
    if (close (device_descriptor) != 0)
    {
        fprintf (stderr, "Failed to close USFS control device /dev/usfs0: %s\n", strerror (errno));
        return 1;
    }

    return rc;
}
