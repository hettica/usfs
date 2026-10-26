/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static struct flock whole_file_lock (short type)
{
    struct flock lock = { 0 };
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    return lock;
}

static int child_can_wait_for_lock (const char * path, const pid_t owner, int notification)
{
    int descriptor = open (path, O_RDWR);
    struct flock lock = whole_file_lock (F_WRLCK);
    if (descriptor < 0)
        return 1;

    if (fcntl (descriptor, F_GETLK, &lock) != 0 || lock.l_type != F_WRLCK || lock.l_pid != owner)
        return 2;

    lock = whole_file_lock (F_WRLCK);
    errno = 0;
    if (fcntl (descriptor, F_SETLK, &lock) != -1 || (errno != EACCES && errno != EAGAIN))
        return 3;

    if (write (notification, "R", 1) != 1)
        return 4;
    if (fcntl (descriptor, F_SETLKW, &lock) != 0)
        return 5;
    if (write (notification, "D", 1) != 1)
        return 6;

    lock.l_type = F_UNLCK;
    if (fcntl (descriptor, F_SETLK, &lock) != 0 || close (descriptor) != 0)
        return 7;
    return 0;
}

static int wait_for_child (pid_t child)
{
    int status = 0;
    return waitpid (child, &status, 0) == child && WIFEXITED (status) && WEXITSTATUS (status) == 0 ? 0 : 1;
}

int main (int argc, char ** argv)
{
    int descriptor, duplicate, signal_pipe[2];
    char signal_byte = 0;
    pid_t child;
    struct flock lock;

    if (argc != 2)
        return 2;
    alarm (20);

    descriptor = open (argv[1], O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (descriptor < 0 || write (descriptor, "lock", 4) != 4)
        return 1;

    lock = whole_file_lock (F_WRLCK);
    if (fcntl (descriptor, F_SETLK, &lock) != 0 || pipe (signal_pipe) != 0)
        return 1;

    child = fork ();
    if (child < 0)
        return 1;
    if (child == 0)
    {
        close (descriptor);
        close (signal_pipe[0]);
        _exit (child_can_wait_for_lock (argv[1], getppid (), signal_pipe[1]));
    }

    close (signal_pipe[1]);
    if (read (signal_pipe[0], &signal_byte, 1) != 1 || signal_byte != 'R')
        return 1;

    lock.l_type = F_UNLCK;
    if (fcntl (descriptor, F_SETLK, &lock) != 0 || read (signal_pipe[0], &signal_byte, 1) != 1 || signal_byte != 'D' ||
        wait_for_child (child) != 0)
        return 1;
    close (signal_pipe[0]);

    /* AIX releases this process's locks when any descriptor for the file closes. */
    lock = whole_file_lock (F_WRLCK);
    duplicate = open (argv[1], O_RDWR);
    if (duplicate < 0 || fcntl (descriptor, F_SETLK, &lock) != 0 || close (duplicate) != 0)
        return 1;
    child = fork ();
    if (child < 0)
        return 1;
    if (child == 0)
    {
        int other = open (argv[1], O_RDWR);
        struct flock query = whole_file_lock (F_WRLCK);
        _exit (other < 0 || fcntl (other, F_GETLK, &query) != 0 || query.l_type != F_UNLCK);
    }
    if (wait_for_child (child) != 0)
        return 1;

    lock = whole_file_lock (F_WRLCK);
    lock.l_whence = SEEK_END;
    lock.l_start = -1;
    lock.l_len = 1;
    if (fcntl (descriptor, F_SETLK, &lock) != 0)
        return 1;
    child = fork ();
    if (child < 0)
        return 1;
    if (child == 0)
    {
        int other = open (argv[1], O_RDWR);
        struct flock query = whole_file_lock (F_WRLCK);
        query.l_start = 3;
        query.l_len = 1;
        _exit (other < 0 || fcntl (other, F_GETLK, &query) != 0 || query.l_type != F_WRLCK || query.l_pid != getppid ());
    }
    if (wait_for_child (child) != 0 || close (descriptor) != 0)
        return 1;

    puts ("RECORD_LOCK_OK");
    return 0;
}
