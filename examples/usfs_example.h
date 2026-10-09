/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 * Private command-line and lifecycle helpers for the examples.
 */
#ifndef USFS_EXAMPLE_H
#define USFS_EXAMPLE_H

#include <usfs/usfs.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct example_arguments
{
    const char * mountpoint;           // Borrowed explicit or automatic mountpoint.
    struct usfs_client_options client; // Native access and lifetime policies.
    unsigned int workers;              // Number of concurrent request workers.
    int singlethread;                  // Explicit single-thread override.
    int help;                          // Print usage without contacting the kernel.
    int version;                       // Print the native API version.
};

enum
{
    EXAMPLE_DECIMAL_RADIX = 10
};

static inline int parse_example_number (const char * text, const unsigned long maximum, unsigned long * result)
{
    if (text == NULL)
        return -EINVAL;

    if (text[0] < '0' || text[0] > '9')
        return -EINVAL;

    char * end = NULL;
    errno = 0;
    const unsigned long value = strtoul (text, &end, EXAMPLE_DECIMAL_RADIX);
    if (errno != 0)
        return -EINVAL;

    if (*end != '\0')
        return -EINVAL;

    if (value == 0 || value > maximum)
        return -EINVAL;

    *result = value;
    return 0;
}

static inline int apply_example_mount_option (struct example_arguments * arguments, const char * option)
{
    static const char worker_option[] = "max_threads=";

    if (strcmp (option, "debug") == 0)
        return 0;

    if (strcmp (option, "hard_remove") == 0)
    {
        arguments->client.behavior.remove_policy = USFS_REMOVE_IMMEDIATE;
        return 0;
    }

    if (strcmp (option, "nullpath_ok") == 0)
    {
        arguments->client.behavior.handle_paths = USFS_PATH_OMIT_FOR_HANDLE;
        return 0;
    }

    if (strncmp (option, worker_option, sizeof (worker_option) - 1) == 0)
    {
        unsigned long workers;
        const int rc = parse_example_number (option + sizeof (worker_option) - 1, USFS_CLIENT_MAX_WORKERS, &workers);
        if (rc != 0)
            return rc;

        arguments->workers = (unsigned int)workers;
        return 0;
    }

    enum usfs_mount_access access;
    if (strcmp (option, "ro") == 0)
        access = USFS_MOUNT_READ_ONLY;
    else if (strcmp (option, "rw") == 0)
        access = USFS_MOUNT_READ_WRITE;
    else
        return -EINVAL;

    if (arguments->client.mount_access != USFS_MOUNT_AUTOMATIC && arguments->client.mount_access != access)
        return -EINVAL;

    arguments->client.mount_access = access;
    return 0;
}

static inline int parse_example_mount_options (struct example_arguments * arguments, const char * text)
{
    char * options = strdup (text);
    if (options == NULL)
        return -ENOMEM;

    int rc = 0;
    char * option = options;
    for (;;)
    {
        char * separator = strchr (option, ',');
        if (separator != NULL)
            *separator = '\0';

        rc = apply_example_mount_option (arguments, option);
        if (rc != 0 || separator == NULL)
            break;

        option = separator + 1;
    }

    free (options);
    return rc;
}

static inline int parse_example_argument (struct example_arguments * arguments, int argc, char ** argv, int * index)
{
    const char * argument = argv[*index];
    if (strcmp (argument, "-h") == 0 || strcmp (argument, "--help") == 0)
        arguments->help = 1;
    else if (strcmp (argument, "-V") == 0 || strcmp (argument, "--version") == 0)
        arguments->version = 1;
    else if (strcmp (argument, "-s") == 0)
        arguments->singlethread = 1;
    else if (strcmp (argument, "-f") == 0 || strcmp (argument, "-d") == 0)
        return 1;
    else if (strncmp (argument, "-o", 2) == 0)
    {
        const char * text = argument + 2;
        if (*text == '\0')
        {
            if (++*index >= argc)
                return -EINVAL;
            text = argv[*index];
        }
        const int rc = parse_example_mount_options (arguments, text);
        return rc == 0 ? 1 : rc;
    }
    else if (argument[0] != '-')
    {
        if (arguments->mountpoint != NULL)
            return -EINVAL;
        arguments->mountpoint = argument;
    }
    else
        return 0;

    return 1;
}

static inline int parse_example_arguments (struct example_arguments * arguments, int argc, char ** argv)
{
    arguments->workers = USFS_CLIENT_DEFAULT_WORKERS;
    for (int index = 1; index < argc; ++index)
    {
        const int rc = parse_example_argument (arguments, argc, argv, &index);
        if (rc <= 0)
        {
            fprintf (stderr, "Failed to parse argument %s: unsupported or invalid value\n", argv[index < argc ? index : argc - 1]);
            return -EINVAL;
        }
    }
    return 0;
}

static inline void print_example_options (void)
{
    puts ("    -h, --help          display usage\n"
          "    -V, --version       display native API version\n"
          "    -f, -d             foreground operation (always enabled)\n"
          "    -s                 use one worker\n"
          "    -o max_threads=N   use 1..64 workers (default 10)\n"
          "    -o ro,rw           select one access mode\n"
          "    -o hard_remove     immediately remove open files\n"
          "    -o nullpath_ok     omit paths for handle operations");
}

static inline int run_example_client (const struct example_arguments * arguments, const struct usfs_operations * operations)
{
    if (arguments->version)
    {
        printf ("USFS native client API %u\n", USFS_CLIENT_API_VERSION);
        return EXIT_SUCCESS;
    }
    if (arguments->help)
    {
        print_example_options ();
        return EXIT_SUCCESS;
    }
    if (arguments->mountpoint == NULL)
    {
        fprintf (stderr, "Failed to mount filesystem: a mountpoint is required\n");
        return EXIT_FAILURE;
    }

    struct usfs_client * client = NULL;
    int rc = usfs_client_create (USFS_CLIENT_API_VERSION, operations, sizeof (*operations), &arguments->client, NULL, &client);
    if (rc != 0)
    {
        fprintf (stderr, "Failed to create filesystem client: %s\n", strerror (-rc));
        return EXIT_FAILURE;
    }

    rc = usfs_client_mount (client, arguments->mountpoint);
    if (rc == 0)
    {
        rc = usfs_client_install_signals (client);
        if (rc == 0)
        {
            rc = usfs_client_run (client, arguments->singlethread ? 1 : arguments->workers);
            usfs_client_remove_signals (client);
        }
        const int unmount_result = usfs_client_unmount (client);
        if (rc == 0)
            rc = unmount_result;
    }

    const int destroy_result = usfs_client_destroy (&client);
    if (rc == 0)
        rc = destroy_result;
    if (rc != 0)
        fprintf (stderr, "Failed to complete filesystem session: %s\n", strerror (-rc));

    return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif
