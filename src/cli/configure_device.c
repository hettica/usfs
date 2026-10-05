/*
    Copyright (c) 2026 Raman Dzehtsiar
    SPDX-License-Identifier: MIT

    cfgusfs executable - USFS device Configure method.

    Invoked by the ODM configuration framework when a USFS device is brought
    online, e.g. `mkdev -l usfs0` runs `cfgusfs -l usfs0`. The optional -1/-2
    boot-time flags of a generic Configure method are not used by this
    pseudo-device.

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

    if (usfs_configure (logical_device_name) != USFS_SUCCESS)
    {
        fprintf (stderr, "Failed to configure device %s: see %s for transaction details\n", logical_device_name, USFS_LOG_PATH);
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}
