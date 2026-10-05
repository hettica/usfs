// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_VFS_TEXT_H
#define USFS_VFS_TEXT_H

/* Return a complete bounded text line, EOF, or an I/O/format failure. NUL
 * bytes and oversized lines must never turn into silently truncated entries. */
static int usfs_vfs_read_line (FILE * stream, char * line, size_t capacity)
{
    size_t length = 0;
    int byte;
    if (capacity == 0)
        return -1;
    while ((byte = fgetc (stream)) != EOF)
    {
        if (byte == 0 || length + 1 >= capacity)
            return -1;
        line[length++] = (char)byte;
        if (byte == '\n')
            break;
    }
    if (ferror (stream))
        return -1;
    line[length] = '\0';
    return length != 0;
}
#endif
