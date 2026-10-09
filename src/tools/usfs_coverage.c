/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "usfs_gcov.h"

#define PATH_BUFFER    1024
#define GCDA_MAGIC     0x67636461u
#define GCOV_MAX_UNITS 32

struct container_unit
{
    const char * name;          // Filename borrowed from the container.
    const unsigned char * gcda; // Serialized data borrowed from the container.
    unsigned gcda_length;       // Serialized data length in bytes.
};

static int is_filename_character (const char character)
{
    if (character >= 'a' && character <= 'z')
        return true;
    if (character >= 'A' && character <= 'Z')
        return true;
    if (character >= '0' && character <= '9')
        return true;
    if (character == '_' || character == '-' || character == '.')
        return true;

    return false;
}

static int safe_filename (const char * name, const unsigned length)
{
    if (length < 6)
        return false;
    if (length >= USFS_GCOV_FILENAME_MAX)
        return false;
    if (name[length - 1] != '\0')
        return false;
    if (strcmp (name + length - 6, ".gcda") != 0)
        return false;

    for (unsigned character_index = 0; character_index + 1 < length; character_index++)
    {
        if (!is_filename_character (name[character_index]))
            return false;
    }

    return true;
}

static int load_info (int file_descriptor, struct usfs_gcov_info * info)
{
    memset (info, 0, sizeof (*info));
    if (ioctl (file_descriptor, USFS_GCOV_IOC_INFO, info) != 0)
        return -1;
    if (info->abi_version != USFS_GCOV_ABI_VERSION ||
        info->gcov_version != USFS_GCOV_VERSION ||
        info->unit_count == 0 ||
        !(info->flags & USFS_GCOV_FLAG_ARCS) ||
        info->snapshot_size < sizeof (struct usfs_gcov_container_header) ||
        info->snapshot_size > USFS_GCOV_MAX_SNAPSHOT)
    {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static int write_all (int file_descriptor, const void * buffer, unsigned length)
{
    const unsigned char * cursor = buffer;
    unsigned remaining = length;

    while (remaining != 0)
    {
        ssize_t written = write (file_descriptor, cursor, remaining);
        if (written <= 0)
            return -1;
        cursor += written;
        remaining -= (unsigned)written;
    }
    return 0;
}

static int parse_container_unit (const unsigned char ** cursor, const unsigned char * end, char names[][USFS_GCOV_FILENAME_MAX], unsigned unit, struct container_unit * parsed_unit)
{
    const struct usfs_gcov_unit_header * unit_header;
    const char * name; // Filename borrowed from the container.
    const uint32_t * gcda_header;
    unsigned prior_unit;

    if ((unsigned)(end - *cursor) < sizeof (*unit_header))
    {
        fprintf (stderr, "truncated unit header\n");
        return 1;
    }
    unit_header = (const struct usfs_gcov_unit_header *)*cursor;
    *cursor += sizeof (*unit_header);
    if (unit_header->reserved != 0 ||
        unit_header->filename_length > (unsigned)(end - *cursor) ||
        !safe_filename ((const char *)*cursor, unit_header->filename_length))
    {
        fprintf (stderr, "unsafe or malformed unit filename\n");
        return 1;
    }

    name = (const char *)*cursor;
    for (prior_unit = 0; prior_unit < unit; prior_unit++)
        if (strcmp (name, names[prior_unit]) == 0)
        {
            fprintf (stderr, "duplicate unit filename: %s\n", name);
            return 1;
        }
    strcpy (names[unit], name);

    *cursor += USFS_GCOV_ALIGN8 (unit_header->filename_length);
    if (*cursor > end ||
        unit_header->gcda_length < 16 ||
        unit_header->gcda_length > (unsigned)(end - *cursor))
    {
        fprintf (stderr, "malformed gcda length for %s\n", name);
        return 1;
    }
    parsed_unit->name = name;
    parsed_unit->gcda = *cursor;
    parsed_unit->gcda_length = unit_header->gcda_length;
    gcda_header = (const uint32_t *)parsed_unit->gcda;
    if (gcda_header[0] != GCDA_MAGIC ||
        gcda_header[1] != unit_header->version ||
        gcda_header[2] != unit_header->stamp ||
        gcda_header[3] != unit_header->checksum ||
        unit_header->version != USFS_GCOV_VERSION)
    {
        fprintf (stderr, "gcda metadata mismatch for %s\n", name);
        return 1;
    }

    *cursor += USFS_GCOV_ALIGN8 (unit_header->gcda_length);
    if (*cursor > end)
    {
        fprintf (stderr, "unit padding exceeds container\n");
        return 1;
    }
    return 0;
}

static int write_container_unit (const char * temporary_directory, const struct container_unit * unit)
{
    char output_path[PATH_BUFFER];
    int output_descriptor;

    if (snprintf (output_path, sizeof (output_path), "%s/%s", temporary_directory, unit->name) >=
        (int)sizeof (output_path))
    {
        fprintf (stderr, "output path too long\n");
        return 1;
    }
    output_descriptor = open (output_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output_descriptor < 0)
    {
        perror ("open gcda output");
        return 1;
    }
    if (write_all (output_descriptor, unit->gcda, unit->gcda_length) != 0)
    {
        perror ("write gcda output");
        close (output_descriptor);
        return 1;
    }
    if (close (output_descriptor) != 0)
    {
        perror ("close gcda output");
        return 1;
    }

    return 0;
}

static void remove_container_outputs (
    const char * temporary_directory,
    char names[][USFS_GCOV_FILENAME_MAX],
    unsigned unit_count
)
{
    char output_path[PATH_BUFFER];
    unsigned unit;

    for (unit = 0; unit < unit_count; unit++)
    {
        if (names[unit][0] != '\0' &&
            snprintf (output_path, sizeof (output_path), "%s/%s", temporary_directory, names[unit]) < (int)sizeof (output_path))
            unlink (output_path);
    }
    rmdir (temporary_directory);
}

static int publish_container_units (const unsigned char * buffer, const unsigned buffer_size, const struct usfs_gcov_container_header * header, const char * temporary_directory, const char * directory, char names[][USFS_GCOV_FILENAME_MAX])
{
    const unsigned char * cursor;
    const unsigned char * end;
    unsigned unit;

    cursor = buffer + sizeof (*header);
    end = buffer + buffer_size;
    for (unit = 0; unit < header->unit_count; unit++)
    {
        struct container_unit parsed_unit;

        if (parse_container_unit (&cursor, end, names, unit, &parsed_unit) != 0 ||
            write_container_unit (temporary_directory, &parsed_unit) != 0)
            return 1;
    }
    if (cursor != end)
    {
        fprintf (stderr, "trailing bytes in coverage container\n");
        return 1;
    }
    if (rename (temporary_directory, directory) != 0)
    {
        perror ("publish snapshot directory");
        return 1;
    }
    printf ("wrote %u kernel gcda files to %s\n", header->unit_count, directory);
    return 0;
}

static int container_to_directory (const unsigned char * buffer, unsigned buffer_size, const char * directory)
{
    const struct usfs_gcov_container_header * header;
    char temporary_directory[PATH_BUFFER];
    char names[GCOV_MAX_UNITS][USFS_GCOV_FILENAME_MAX];
    int rc = 1;

    if (buffer_size < sizeof (*header) ||
        snprintf (temporary_directory, sizeof (temporary_directory), "%s.tmp.%ld", directory, (long)getpid ()) >= (int)sizeof (temporary_directory))
    {
        fprintf (stderr, "invalid coverage container or output directory\n");
        return 1;
    }

    header = (struct usfs_gcov_container_header *)buffer;
    if (header->magic != USFS_GCOV_MAGIC ||
        header->abi_version != USFS_GCOV_ABI_VERSION ||
        header->gcov_version != USFS_GCOV_VERSION ||
        header->unit_count == 0 || header->unit_count > GCOV_MAX_UNITS ||
        header->total_size != buffer_size || header->reserved != 0)
    {
        fprintf (stderr, "malformed coverage container header\n");
        return 1;
    }
    if (access (directory, F_OK) == 0)
    {
        fprintf (stderr, "destination already exists: %s\n", directory);
        return 1;
    }
    if (mkdir (temporary_directory, 0700) != 0)
    {
        perror ("mkdir snapshot temporary directory");
        return 1;
    }
    memset (names, 0, sizeof (names));

    rc = publish_container_units (buffer, buffer_size, header, temporary_directory, directory, names);

    if (rc != 0)
        remove_container_outputs (temporary_directory, names, header->unit_count);
    return rc;
}

static int snapshot_to_directory (int file_descriptor, const char * directory)
{
    struct usfs_gcov_info info;
    struct usfs_gcov_snapshot_request request;
    unsigned char * buffer;
    int rc;

    if (load_info (file_descriptor, &info) != 0)
    {
        perror ("coverage info");
        return 1;
    }
    buffer = malloc (info.snapshot_size);
    if (buffer == NULL)
    {
        perror ("malloc snapshot");
        return 1;
    }
    memset (&request, 0, sizeof (request));
    request.abi_version = USFS_GCOV_ABI_VERSION;
    request.capacity = info.snapshot_size;
    request.user_buffer = (uint64_t)(unsigned long)buffer;
    if (ioctl (file_descriptor, USFS_GCOV_IOC_SNAPSHOT, &request) != 0)
    {
        perror ("snapshot ioctl");
        free (buffer);
        return 1;
    }
    if (request.size != info.snapshot_size)
    {
        fprintf (stderr, "snapshot size changed (%u != %u)\n", request.size, info.snapshot_size);
        free (buffer);
        return 1;
    }
    rc = container_to_directory (buffer, request.size, directory);
    free (buffer);
    return rc;
}

static int unpack_container_file (const char * path, const char * directory)
{
    struct stat status;
    unsigned char * buffer;
    unsigned remaining;
    unsigned char * cursor;
    int file_descriptor;
    int rc;

    if (stat (path, &status) != 0 || status.st_size <= 0 ||
        (uint64_t)status.st_size > USFS_GCOV_MAX_SNAPSHOT)
    {
        fprintf (stderr, "invalid coverage container file: %s\n", path);
        return 1;
    }
    file_descriptor = open (path, O_RDONLY);
    if (file_descriptor < 0)
    {
        perror ("open coverage container");
        return 1;
    }
    remaining = (unsigned)status.st_size;
    buffer = malloc (remaining);
    if (buffer == NULL)
    {
        perror ("malloc coverage container");
        close (file_descriptor);
        return 1;
    }
    cursor = buffer;
    while (remaining != 0)
    {
        ssize_t count = read (file_descriptor, cursor, remaining);
        if (count <= 0)
        {
            perror ("read coverage container");
            free (buffer);
            close (file_descriptor);
            return 1;
        }
        cursor += count;
        remaining -= (unsigned)count;
    }
    if (close (file_descriptor) != 0)
    {
        perror ("close coverage container");
        free (buffer);
        return 1;
    }
    rc = container_to_directory (buffer, (unsigned)status.st_size, directory);
    free (buffer);
    return rc;
}

static int process_device_command (const int file_descriptor, const int argc, char ** argv)
{
    struct usfs_gcov_info info;
    uint32_t abi_version = USFS_GCOV_ABI_VERSION;
    int rc = 1;

    if (strcmp (argv[1], "info") == 0 && argc == 2)
    {
        if (load_info (file_descriptor, &info) != 0)
            perror ("coverage info");
        else
        {
            printf ("ABI=%u gcov=%#x units=%u flags=%#x snapshot=%u\n", info.abi_version, info.gcov_version, info.unit_count, info.flags, info.snapshot_size);
            rc = 0;
        }
    }
    else if (strcmp (argv[1], "reset") == 0 && argc == 2)
    {
        if (ioctl (file_descriptor, USFS_GCOV_IOC_RESET, &abi_version) != 0)
            perror ("coverage reset");
        else
        {
            puts ("kernel coverage counters reset");
            rc = 0;
        }
    }
    else if (strcmp (argv[1], "snapshot") == 0 && argc == 3)
    {
        rc = snapshot_to_directory (file_descriptor, argv[2]);
    }
    else
    {
        fprintf (stderr, "usage: %s info | reset | snapshot DIRECTORY | "
                         "unpack CONTAINER DIRECTORY\n",
                 argv[0]);
        rc = 2;
    }

    return rc;
}

int main (const int argc, char ** argv)
{
    int file_descriptor;
    int rc = 1;

    if (argc == 4 && strcmp (argv[1], "unpack") == 0)
        return unpack_container_file (argv[2], argv[3]);
    if (argc < 2 || argc > 3)
    {
        fprintf (stderr, "usage: %s info | reset | snapshot DIRECTORY | "
                         "unpack CONTAINER DIRECTORY\n",
                 argv[0]);
        return 2;
    }
    file_descriptor = open ("/dev/usfs0", O_RDWR | O_NONBLOCK);
    if (file_descriptor < 0)
    {
        perror ("open /dev/usfs0");
        return 1;
    }

    rc = process_device_command (file_descriptor, argc, argv);
    close (file_descriptor);
    return rc;
}
