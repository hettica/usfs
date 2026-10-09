/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "usfs_config.h"
#include "usfs_config_instrumentation.h"

struct usfs_test_bootstrap
{
    uint32_t fault_identifier;   // Fault selected for initialization injection.
    int32_t injected_errno;      // Error returned when the selected fault triggers.
    uint32_t trigger_occurrence; // Matching occurrence that triggers the fault.
};

void usfs_config_instrumentation_prepare (struct usfs_dev_cfg * device_configuration, const enum usfs_config_operation operation)
{
    if (operation != USFS_CONFIG_OPERATION_INITIALIZE)
        return;

    const char * fault_text = getenv ("USFS_TEST_INIT_FAULT");
    if (fault_text == NULL)
        return;

    const char * error_text = getenv ("USFS_TEST_INIT_ERRNO");
    const char * occurrence_text = getenv ("USFS_TEST_INIT_OCCURRENCE");
    struct usfs_test_bootstrap bootstrap;

    memset (&bootstrap, 0, sizeof (bootstrap));
    bootstrap.fault_identifier = (uint32_t)strtoul (fault_text, NULL, 0);
    bootstrap.injected_errno = (int32_t)strtol (error_text != NULL ? error_text : "5", NULL, 0);
    bootstrap.trigger_occurrence = (uint32_t)strtoul (occurrence_text != NULL ? occurrence_text : "1", NULL, 0);
    device_configuration->flags |= USFS_CONFIG_FLAG_INSTRUMENTATION;
    memcpy (device_configuration->instrumentation, &bootstrap, sizeof (bootstrap));
}

static int write_lock_marker (const char * marker_path)
{
    if (marker_path == NULL)
        return 0;

    if (marker_path[0] == '\0')
        return 0;

    FILE * marker_file = fopen (marker_path, "w");
    if (marker_file == NULL)
        return 1;

    int rc = 0;
    if (fprintf (marker_file, "%ld\n", (long)getpid ()) < 0)
        rc = 1;

    if (fclose (marker_file) != 0)
        rc = 1;

    return rc;
}

static int hold_configuration_lock (const char * hold_duration_text)
{
    if (hold_duration_text == NULL)
        return 0;

    const unsigned long hold_duration_ms = strtoul (hold_duration_text, NULL, 10);
    if (hold_duration_ms > 10000)
        return 1;

    struct timespec remaining_duration;
    remaining_duration.tv_sec = (time_t)(hold_duration_ms / 1000);
    remaining_duration.tv_nsec = (long)(hold_duration_ms % 1000) * 1000000L;
    while (nanosleep (&remaining_duration, &remaining_duration) != 0)
    {
        if (errno != EINTR)
            return 1;
    }

    return 0;
}

static int process_locked_checkpoint (void)
{
    const char * marker_path = getenv ("USFS_TEST_METHOD_LOCK_MARKER");
    const char * hold_duration_text = getenv ("USFS_TEST_METHOD_HOLD_MS");

    if (write_lock_marker (marker_path) != 0)
        return 1;

    return hold_configuration_lock (hold_duration_text);
}

int usfs_config_instrumentation_checkpoint (const enum usfs_config_method_checkpoint checkpoint)
{
    const char * fault_text = getenv ("USFS_TEST_METHOD_FAULT");

    if (checkpoint == USFS_CONFIG_METHOD_LOCKED)
    {
        if (process_locked_checkpoint () != 0)
            return 1;
    }

    if (fault_text == NULL)
        return 0;

    if (strtoul (fault_text, NULL, 10) == (unsigned long)checkpoint)
        return 1;

    return 0;
}
