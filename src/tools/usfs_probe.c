/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * usfs_probe - diagnostic ladder for the USFS control device.
 *
 * Exercises the device stepwise so a crash or hang can be attributed to one
 * specific kernel path:
 *   step 1: open /dev/usfs0                    (ddmpx channel alloc + ddopen)
 *   step 2: ioctl USFS_IOC_CONNECTION_REQUEST  (ddioctl handshake)
 *   step 3: non-blocking read                  (ddread empty-queue path, expects EAGAIN)
 *   step 4: close                              (ddclose + ddmpx dealloc)
 *   step 5: blocking read + SIGALRM            (e_sleep_thread in ddread, wakeup by
 *                                       signal, expects EINTR)
 * No mount is performed. Each step prints before it runs and after it
 * succeeds, so console output pinpoints the failing step.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "usfs_file.h"
#include "usfs_proto.h"

static void alarm_handler (const int signal_number)
{
    (void)signal_number;
}

static int open_nonblocking_device (void)
{
    printf ("step 1: open /dev/usfs0 ...\n");
    fflush (stdout);
    const int device_descriptor = open ("/dev/usfs0", O_RDWR | O_NONBLOCK);
    if (device_descriptor < 0)
    {
        printf ("  open failed: %s\n", strerror (errno));
        return INVALID_FILE_DESCRIPTOR;
    }
    printf ("  open ok, fd=%d\n", device_descriptor);
    fflush (stdout);

    return device_descriptor;
}

static void probe_connection_request (const int device_descriptor)
{
    struct usfs_dev_info device_info;

    printf ("step 2: ioctl USFS_IOC_CONNECTION_REQUEST ...\n");
    fflush (stdout);
    memset (&device_info, 0, sizeof (device_info));
    if (ioctl (device_descriptor, USFS_IOC_CONNECTION_REQUEST, &device_info) != 0)
    {
        printf ("  ioctl failed: %s\n", strerror (errno));
    }
    else
    {
        printf ("  ioctl ok: proto=%u gfs_type=%d chan=%d cookie=%016llx\n", (unsigned)device_info.protocol_version, (int)device_info.fs_type, (int)device_info.channel, (unsigned long long)device_info.cookie);
    }
    fflush (stdout);
}

static void probe_nonblocking_read (const int device_descriptor)
{
    char read_buffer[64];

    printf ("step 3: non-blocking read (expect EAGAIN) ...\n");
    fflush (stdout);
    const ssize_t bytes_read = read (device_descriptor, read_buffer, sizeof (read_buffer));
    if (bytes_read < 0)
        printf ("  read failed as expected: %s\n", strerror (errno));
    else
        printf ("  read returned %ld bytes (unexpected)\n", (long)bytes_read);
    fflush (stdout);
}

static int probe_interrupted_read (void)
{
    struct sigaction alarm_action;
    char read_buffer[64];

    printf ("step 5: blocking read, interrupted by SIGALRM after 2s ...\n");
    fflush (stdout);
    const int device_descriptor = open ("/dev/usfs0", O_RDWR);
    if (device_descriptor < 0)
    {
        printf ("  reopen failed: %s\n", strerror (errno));
        return 1;
    }
    memset (&alarm_action, 0, sizeof (alarm_action));
    alarm_action.sa_handler = alarm_handler; /* no SA_RESTART: read must return EINTR */
    sigemptyset (&alarm_action.sa_mask);
    sigaction (SIGALRM, &alarm_action, NULL);
    alarm (2);
    const ssize_t bytes_read = read (device_descriptor, read_buffer, sizeof (read_buffer));
    if (bytes_read < 0 && errno == EINTR)
        printf ("  blocking read interrupted as expected (EINTR)\n");
    else
        printf ("  unexpected result: n=%ld errno=%d (%s)\n", (long)bytes_read, errno, strerror (errno));
    alarm (0);
    close (device_descriptor);
    fflush (stdout);

    return 0;
}

int main (void)
{
    const int device_descriptor = open_nonblocking_device ();
    if (device_descriptor < 0)
        return 1;

    probe_connection_request (device_descriptor);
    probe_nonblocking_read (device_descriptor);

    printf ("step 4: close ...\n");
    fflush (stdout);
    close (device_descriptor);
    printf ("  close ok\n");
    fflush (stdout);

    if (probe_interrupted_read () != 0)
        return 1;

    printf ("probe complete\n");

    return 0;
}
