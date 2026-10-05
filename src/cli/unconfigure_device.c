/*
    Copyright (c) 2026 Raman Dzehtsiar
    SPDX-License-Identifier: MIT

    ucfgusfs executable - USFS device Unconfigure method.

    Invoked by the ODM configuration framework when a USFS device is taken
    offline, e.g. `rmdev -l usfs0` runs `ucfgusfs -l usfs0`. It reverses the
    work done by cfgusfs: CFG_TERM, device-number release, ODM status update,
    /dev node removal and kernel extension unload.

    The shared implementation holds the ODM configuration lock across the
    complete transaction.
 */

#include "usfs_common.h"

int main (const int argc, char ** argv)
{
    const char * logical_device_name = NULL;

    if (usfs_parse_device_arg (argc, argv, &logical_device_name) != USFS_SUCCESS)
    {
        fprintf (stderr, "Failed to parse device arguments: expected -l %s\n", USFS_LOGICAL_DEVICE_NAME);
        return USFS_FAILURE;
    }

    if (usfs_unconfigure (logical_device_name) != USFS_SUCCESS)
    {
        fprintf (stderr, "Failed to unconfigure device %s: see %s for transaction details\n", logical_device_name, USFS_LOG_PATH);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}
