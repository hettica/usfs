// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_CONFIG_H
#define USFS_CONFIG_H

#ifdef _KERNEL
    #include <sys/inttypes.h>
#else
    #include <stdint.h>
#endif

#include "usfs_status.h"


#define USFS_CONFIG_DDS_MAGIC   0x55534644u
#define USFS_CONFIG_ABI_VERSION 2u

#define USFS_CONFIG_FLAG_INSTRUMENTATION 0x00000001u
#define USFS_CONFIG_FLAG_DEFERRED_EXPORT 0x00000002u
#define USFS_CONFIG_INSTRUMENTATION_SIZE 12u

/* Runtime availability policy, managed through chusfs. */
#define USFS_RUNTIME_CONFIG_MAGIC       0x55534652u
#define USFS_RUNTIME_CONFIG_ABI_VERSION 1u

#define USFS_RUNTIME_SET_REQUEST_TIMEOUT 0x00000001u
#define USFS_RUNTIME_SET_PAGER_TIMEOUT   0x00000002u
#define USFS_RUNTIME_SET_MAX_OUTSTANDING 0x00000004u
#define USFS_RUNTIME_SET_ALL             (USFS_RUNTIME_SET_REQUEST_TIMEOUT | USFS_RUNTIME_SET_PAGER_TIMEOUT | USFS_RUNTIME_SET_MAX_OUTSTANDING)

#define USFS_DEFAULT_REQUEST_TIMEOUT_MS 30000u
#define USFS_DEFAULT_PAGER_TIMEOUT_MS   10000u
#define USFS_DEFAULT_MAX_OUTSTANDING    256u

#define USFS_MIN_TIMEOUT_MS  100u
#define USFS_MAX_TIMEOUT_MS  600000u
#define USFS_MIN_OUTSTANDING 1u
#define USFS_MAX_OUTSTANDING 4096u

/*
 * Private commands used only between the coverage-aware ODM methods and the
 * still-loaded instrumented extension. They are never published as a runtime
 * device ABI.
 *
 * todo: rename; instrumentation should be abstract, not related to export or drain which is a part of gcov runtime
 */
#define USFS_CFG_INSTRUMENTATION_EXPORT 0x55534301 /* Exports deferred instrumentation data. */
#define USFS_CFG_INSTRUMENTATION_DRAIN  0x55534302 /* Drains exported instrumentation data. */

/*
 * Public commands accepted by the USFS configuration entry point.
 */
#define USFS_CFG_INIT            CFG_INIT   /* Initializes the extension. */
#define USFS_CFG_TERM            CFG_TERM   /* Terminates the extension. */
#define USFS_CFG_QUERY_LIFECYCLE 0x55534303 /* Queries the current lifecycle state. */

/*
 * Lifecycle states returned by USFS_CFG_QUERY_LIFECYCLE.
 */
#define USFS_KEXT_STATE_DOWN             0u /* Extension is not configured. */
#define USFS_KEXT_STATE_STARTING         1u /* Initialization is in progress. */
#define USFS_KEXT_STATE_ACTIVE           2u /* Extension accepts normal operations. */
#define USFS_KEXT_STATE_STOPPING         3u /* Termination is in progress. */
#define USFS_KEXT_STATE_CLEANUP_REQUIRED 4u /* A failed teardown requires a retry. */


struct kext_runtime_config
{
    uint32_t magic;
    uint16_t abi_version;
    uint16_t size;
    uint32_t set_mask;
    uint32_t request_timeout_ms;
    uint32_t pager_timeout_ms;
    uint32_t max_outstanding_requests;
    uint32_t active_connections;
    uint32_t unhealthy_connections;
    uint32_t outstanding_requests;
    uint32_t reserved[3];
};

struct usfs_dev_cfg
{
    uint32_t magic;
    uint16_t abi_version;
    uint16_t size;
    uint32_t flags;
    uint32_t generation;
    unsigned char instrumentation[USFS_CONFIG_INSTRUMENTATION_SIZE];
    uint32_t reserved;
};

struct usfs_config_coverage_export
{
    uint32_t magic;
    uint16_t abi_version;
    uint16_t size;
    uint32_t generation;
    uint32_t capacity;
    uint64_t user_buffer;
    uint32_t reserved[2];
};

struct usfs_kext_lifecycle_query
{
    uint32_t magic;
    uint16_t abi_version;
    uint16_t size;
    uint64_t user_buffer;
    uint32_t reserved[2];
};

struct usfs_kext_lifecycle_state
{
    uint32_t magic;
    uint16_t abi_version;
    uint16_t size;
    uint32_t state;
    uint32_t reserved[3];
};

#endif
