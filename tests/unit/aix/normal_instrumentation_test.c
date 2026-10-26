// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#include "usfs_config.h"
#include "usfs_config_instrumentation.h"
#include <string.h>

int __fd_getdtablesize (void);
int __fd_select (void);

int main (void)
{
    struct tap_state tap;
    struct usfs_dev_cfg configuration = { 0 };

    tap_plan (&tap, 5);
    configuration.abi_version = USFS_CONFIG_ABI_VERSION;
    const struct usfs_dev_cfg original = configuration;

    usfs_config_instrumentation_prepare (&configuration, USFS_CONFIG_OPERATION_INITIALIZE);
    tap_ok (&tap, memcmp (&configuration, &original, sizeof (configuration)) == 0, "release instrumentation preserves initialization settings");
    usfs_config_instrumentation_prepare (&configuration, USFS_CONFIG_OPERATION_TERMINATE);
    tap_ok (&tap, memcmp (&configuration, &original, sizeof (configuration)) == 0, "release instrumentation preserves termination settings");
    tap_ok (&tap, usfs_config_instrumentation_checkpoint (USFS_CONFIG_METHOD_LOCKED) == 0, "release checkpoint never injects a failure");
    tap_ok (&tap, __fd_getdtablesize () == 2000, "diagnostic descriptor capacity retains its compatibility contract");
    tap_ok (&tap, __fd_select () == -1, "diagnostic select is explicitly unsupported");

    return tap_finish (&tap);
}
