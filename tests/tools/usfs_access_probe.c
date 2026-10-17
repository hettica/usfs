/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include <sys/access.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_access_argument (const char * text, long * parsed_value)
{
    char * parse_end = NULL;

    errno = 0;
    const long numeric_value = strtol (text, &parse_end, 0);
    if (errno != 0)
        return 2;

    if (parse_end == text)
        return 2;

    if (*parse_end != '\0')
        return 2;

    *parsed_value = numeric_value;
    return 0;
}

int main (const int argc, char ** argv)
{
    if (argc != 4)
    {
        fprintf (stderr, "usage: %s path mode who\n", argv[0]);
        return 2;
    }

    long access_mode;
    if (parse_access_argument (argv[2], &access_mode) != 0)
        return 2;

    long access_identity;
    if (parse_access_argument (argv[3], &access_identity) != 0)
        return 2;

    if (accessx (argv[1], (int)access_mode, (int)access_identity) == 0)
        return 0;

    perror ("accessx");
    return 1;
}
