// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "usfs_config.h"
#include "usfs_config_instrumentation.h"

void usfs_config_instrumentation_prepare (struct usfs_dev_cfg * device_configuration, const enum usfs_config_operation operation)
{
    (void)device_configuration;
    (void)operation;
}

int usfs_config_instrumentation_checkpoint (const enum usfs_config_method_checkpoint checkpoint)
{
    (void)checkpoint;

    return 0;
}
