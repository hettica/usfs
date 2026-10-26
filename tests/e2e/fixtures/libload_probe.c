/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef int (*answer_function) (void);

int main (int argc, char ** argv)
{
    if (argc != 3)
    {
        fprintf (stderr, "usage: %s direct|member PATH\n", argv[0]);
        return 2;
    }

    int flags = RTLD_NOW;
    if (strcmp (argv[1], "member") == 0)
        flags |= RTLD_MEMBER;
    else if (strcmp (argv[1], "direct") != 0)
        return 2;

    errno = 0;
    void * handle = dlopen (argv[2], flags);
    if (handle == NULL)
    {
        fprintf (stderr, "dlopen failed: errno=%d (%s), detail=%s\n", errno, strerror (errno), dlerror ());
        return 1;
    }

    answer_function answer = (answer_function)dlsym (handle, "usfs_probe_answer");
    if (answer == NULL)
    {
        fprintf (stderr, "dlsym failed: %s\n", dlerror ());
        return 1;
    }

    const int value = answer ();
    printf ("LOAD_OK answer=%d\n", value);
    if (dlclose (handle) != 0)
    {
        fprintf (stderr, "dlclose failed: %s\n", dlerror ());
        return 1;
    }

    return value == 4242 ? 0 : 1;
}
