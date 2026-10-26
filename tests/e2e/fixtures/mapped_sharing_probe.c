/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#define _ALL_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

enum { MAPPING_BYTES = 8192 };

/* Run against an existing file of at least MAPPING_BYTES, on both JFS2 and
 * USFS. The scenario daemon admits every OPEN, so the USFS checks below must
 * be enforced by the kernel rather than accidentally by a backing-file fd. */
static int check_mapping (const char * path, const int flags, const char * label)
{
    int result = 1;
    int owner = open (path, flags);
    int reader = -1;
    void * mapping = MAP_FAILED;
    if (owner < 0)
    {
        perror ("open mapping owner");
        goto done;
    }

    mapping = mmap (NULL, MAPPING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, owner, 0);
    if (mapping == MAP_FAILED)
    {
        perror ("writable shared mmap");
        goto done;
    }
    ((volatile unsigned char *)mapping)[0] = 42;
    if (close (owner) != 0)
    {
        owner = -1;
        perror ("close mapping owner");
        goto done;
    }
    owner = -1;

    errno = 0;
    reader = open (path, O_RDONLY | O_RSHARE);
    if (reader >= 0 || errno != ETXTBSY)
    {
        fprintf (stderr, "sharing with surviving writable mapping: fd=%d errno=%d, expected ETXTBSY\n", reader, errno);
        goto done;
    }
    /* The mapping must still be writable after its descriptor closes. */
    ((volatile unsigned char *)mapping)[1] = 43;
    if (munmap (mapping, MAPPING_BYTES) != 0)
    {
        perror ("last writable unmap");
        goto done;
    }
    mapping = MAP_FAILED;

    reader = open (path, O_RDONLY | O_RSHARE);
    if (reader < 0)
    {
        perror ("sharing after last writable unmap");
        goto done;
    }
    result = 0;

done:
    if (reader >= 0 && close (reader) != 0)
        result = 1;
    if (mapping != MAP_FAILED && munmap (mapping, MAPPING_BYTES) != 0)
        result = 1;
    if (owner >= 0 && close (owner) != 0)
        result = 1;
    printf ("%s=%s\n", label, result == 0 ? "valid" : "failed");
    return result;
}

int main (int argc, char ** argv)
{
    if (argc != 2)
        return 2;
    alarm (30);
    if (check_mapping (argv[1], O_RDWR, "ordinary-writer") != 0)
        return 1;
    return check_mapping (argv[1], O_RDWR | O_RSHARE, "shared-writer");
}
