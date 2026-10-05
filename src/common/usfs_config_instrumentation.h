// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_CONFIG_INSTRUMENTATION_H
#define USFS_CONFIG_INSTRUMENTATION_H

struct usfs_dev_cfg;

enum usfs_config_operation
{
    USFS_CONFIG_OPERATION_INITIALIZE = 1,
    USFS_CONFIG_OPERATION_TERMINATE = 2
};

enum usfs_config_method_checkpoint
{
    USFS_CONFIG_METHOD_LOCKED = 1,
    USFS_CONFIG_METHOD_AFTER_LOAD = 2,
    USFS_CONFIG_METHOD_AFTER_DEVICE_CONFIG = 3,
    USFS_CONFIG_METHOD_AFTER_CFG_TERM = 4,
    USFS_CONFIG_METHOD_AFTER_DEVICE_CLEANUP = 5,
    USFS_CONFIG_METHOD_AFTER_UNLOAD = 6,
    USFS_CONFIG_METHOD_AFTER_VFS_UNREGISTER = 7,
    USFS_CONFIG_METHOD_MARK_AVAILABLE = 8,
    USFS_CONFIG_METHOD_LIFECYCLE_QUERY = 9
};

/* Build-selected configuration backend; normal builds provide a no-op. */
void usfs_config_instrumentation_prepare (
    struct usfs_dev_cfg * dds,
    enum usfs_config_operation operation
);

/* Test builds can delay or fail a userspace configuration transaction. */
int usfs_config_instrumentation_checkpoint (
    enum usfs_config_method_checkpoint checkpoint
);

#endif
