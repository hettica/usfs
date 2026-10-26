/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main (int argc, char ** argv)
{
    char existing[PATH_MAX];
    char absent[PATH_MAX];
    int descriptor;

    if (argc != 2 || snprintf (existing, sizeof (existing), "%s/rshare-existing", argv[1]) >= (int)sizeof (existing) ||
        snprintf (absent, sizeof (absent), "%s/rshare-absent", argv[1]) >= (int)sizeof (absent))
        return 2;

    descriptor = open (existing, O_CREAT | O_RDWR, 0600);
    if (descriptor < 0 || close (descriptor) != 0)
    {
        perror ("ordinary create");
        return 1;
    }

    errno = 0;
    descriptor = open (existing, O_RDONLY | O_RSHARE);
    if (descriptor >= 0)
    {
        close (descriptor);
        fprintf (stderr, "read-share open unexpectedly succeeded\n");
        return 1;
    }
    if (errno != EOPNOTSUPP)
    {
        perror ("read-share open");
        return 1;
    }

    errno = 0;
    descriptor = open (absent, O_CREAT | O_RDWR | O_RSHARE, 0600);
    if (descriptor >= 0)
    {
        close (descriptor);
        fprintf (stderr, "read-share create unexpectedly succeeded\n");
        return 1;
    }
    if (errno != EOPNOTSUPP)
    {
        perror ("read-share create");
        return 1;
    }

    struct stat attributes;
    if (lstat (absent, &attributes) == 0 || errno != ENOENT)
    {
        fprintf (stderr, "failed read-share create left a file\n");
        return 1;
    }

    puts ("RSHARE_REJECT_OK");
    return 0;
}
