// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"
#include "usfs_gcov.h"
#include "usfs_test.h"

#ifdef USFS_TESTING
    #include "instrumentation/testing/runtime.h"
#endif

#ifdef USFS_COVERAGE
    #include "instrumentation/coverage/control.h"
    #include "instrumentation/coverage/runtime.h"
    #include "usfs_gcov.h"
#endif

/* All compile-time instrumentation selection is confined to this adapter. */

#ifdef USFS_COVERAGE
static int coverage_export_pending;
static uint32_t coverage_generation;
#endif

uint32_t usfs_instrumentation_config_flags (void)
{
    uint32_t flags = 0;

#ifdef USFS_TESTING
    flags |= USFS_CONFIG_FLAG_INSTRUMENTATION;
#endif
#ifdef USFS_COVERAGE
    flags |= USFS_CONFIG_FLAG_DEFERRED_EXPORT;
#endif
    return flags;
}

int validate_instrumentation_config (const struct usfs_dev_cfg * device_config)
{
    static const unsigned char empty_instrumentation[USFS_CONFIG_INSTRUMENTATION_SIZE] = { 0 };

    if ((device_config->flags & ~usfs_instrumentation_config_flags ()) != 0)
        return EINVAL;

    if ((device_config->flags & USFS_CONFIG_FLAG_INSTRUMENTATION) == 0)
    {
        if (memcmp (device_config->instrumentation, empty_instrumentation, sizeof (empty_instrumentation)) != 0)
            return EINVAL;
    }
#ifndef USFS_TESTING
    if ((device_config->flags & USFS_CONFIG_FLAG_INSTRUMENTATION) != 0)
        return EINVAL;
#endif
#ifdef USFS_COVERAGE
    if ((device_config->flags & USFS_CONFIG_FLAG_DEFERRED_EXPORT) != 0)
    {
        if (device_config->generation == 0)
            return EINVAL;
    }
#else
    if ((device_config->flags & USFS_CONFIG_FLAG_DEFERRED_EXPORT) != 0)
        return EINVAL;
#endif
    return 0;
}

int set_up_instrumentation (const struct usfs_dev_cfg * device_config)
{
#ifdef USFS_TESTING
    return usfs_test_initialize (device_config->instrumentation, (device_config->flags & USFS_CONFIG_FLAG_INSTRUMENTATION) != 0);
#else
    ignore_parameter device_config;
    return 0;
#endif
}

int initialize_instrumentation (void)
{
#ifdef USFS_COVERAGE
    usfs_gcov_register_all ();
    return usfs_kcoverage_initialize ();
#else
    return 0;
#endif
}

void tear_down_instrumentation (void)
{
#ifdef USFS_COVERAGE
    usfs_gcov_runtime_shutdown ();
#endif
}

int should_defer_kext_cleanup (const struct usfs_dev_cfg * device_config)
{
#ifdef USFS_COVERAGE
    return (device_config->flags & USFS_CONFIG_FLAG_DEFERRED_EXPORT) != 0;
#else
    ignore_parameter device_config;
    return false;
#endif
}

void on_kext_cleanup_deferred (const uint32_t generation)
{
#ifdef USFS_COVERAGE
    coverage_export_pending = true;
    coverage_generation = generation;
#else
    ignore_parameter generation;
#endif
}

int is_instrumentation_cfg_command (const int command)
{
    if (command == USFS_CFG_INSTRUMENTATION_EXPORT)
        return true;

    return command == USFS_CFG_INSTRUMENTATION_DRAIN;
}

#ifdef USFS_COVERAGE
static int finish_deferred_export (void)
{
    const uint32_t completed_generation = coverage_generation;

    coverage_export_pending = false;
    coverage_generation = 0;
    /* Counter storage remains static, so prepare_export can reconstruct the
     * registry if the final unpin fails. */
    tear_down_instrumentation ();
    /* Publish terminal state before the last unpin. */
    usfs_lifecycle_state_replace (USFS_KEXT_DOWN);
    const int rc = usfs_kunpin_deferred ();
    if (rc != 0)
    {
        coverage_export_pending = true;
        coverage_generation = completed_generation;
        usfs_lifecycle_state_replace (USFS_KEXT_CLEANUP_REQUIRED);

        return rc;
    }

    return 0;
}

static int prepare_export_cleanup (const int command)
{
    if (command != USFS_CFG_INSTRUMENTATION_DRAIN)
        return 0;

    const int rc = usfs_kcoverage_drain_fault ();
    if (rc != 0)
        return rc;

    return usfs_prepare_deferred_cleanup ();
}
#endif

int process_instrumentation_cfg_command (const int command, struct uio * user_io_request)
{
#ifdef USFS_COVERAGE
    if (!coverage_export_pending)
        return EBUSY;

    const int pin_error = usfs_kensure_pinned ();
    if (pin_error != 0)
        return pin_error;

    const int cleanup_error = prepare_export_cleanup (command);
    const int export_error = usfs_gcov_config_export (user_io_request, true, coverage_generation);
    if (export_error != 0)
        return export_error;

    if (cleanup_error != 0)
    {
        usfs_lifecycle_state_replace (USFS_KEXT_CLEANUP_REQUIRED);

        return cleanup_error;
    }

    if (command == USFS_CFG_INSTRUMENTATION_EXPORT)
        return 0;

    return finish_deferred_export ();
#else
    ignore_parameter command;
    ignore_parameter user_io_request;
    return EINVAL;
#endif
}

int usfs_checkpoint (const enum usfs_checkpoint checkpoint)
{
#ifdef USFS_TESTING
    static const unsigned schedule_points[] = { 0,
                                                USFS_TEST_POINT_MOUNT_RESERVED,
                                                USFS_TEST_POINT_TERM_BEFORE_CLOSE,
                                                USFS_TEST_POINT_TERM_BUSY_REOPENED,
                                                USFS_TEST_POINT_CHANNEL_PRE_ADMISSION,
                                                USFS_TEST_POINT_TERM_GATE_CLOSED,
                                                USFS_TEST_POINT_CHANNEL_REJECTED,
                                                USFS_TEST_POINT_LOOKUP_POST_REPLY,
                                                USFS_TEST_POINT_LOOKUP_RESOLVED,
                                                USFS_TEST_POINT_CACHE_BEFORE_EVICT,
                                                USFS_TEST_POINT_CACHE_AFTER_WRITE,
                                                USFS_TEST_POINT_CACHE_EVICT_DONE,
                                                USFS_TEST_POINT_CHANNEL_UNPUBLISHED,
                                                USFS_TEST_POINT_VGET_RESERVED,
                                                USFS_TEST_POINT_FORCE_UNMOUNT_STALE };

    return usfs_test_schedule_point (schedule_points[(int)checkpoint]);
#else
    ignore_parameter checkpoint;
    return 0;
#endif
}

void usfs_instrumentation_node_created (struct usfs_mount_data * mount_data, const uint64_t node_id)
{
#ifdef USFS_TESTING
    usfs_test_node_created (mount_data, node_id);
#else
    ignore_parameter mount_data;
    ignore_parameter node_id;
#endif
}

void usfs_instrumentation_node_reclaimed (const uint64_t node_id, const int is_linked)
{
#ifdef USFS_TESTING
    usfs_test_node_reclaimed (node_id, is_linked);
#else
    ignore_parameter node_id;
    ignore_parameter is_linked;
#endif
}

void usfs_instrumentation_node_reused (const enum usfs_instrumentation_node_reuse source)
{
#ifdef USFS_TESTING
    static const int reuse_sources[] = { 0, USFS_TEST_NODE_REUSE_LOOKUP, USFS_TEST_NODE_REUSE_PARENT, USFS_TEST_NODE_REUSE_CREATE };

    if (source != USFS_INSTRUMENT_NODE_REUSE_NONE)
        usfs_test_node_reused (reuse_sources[(int)source]);
#else
    ignore_parameter source;
#endif
}

void usfs_instrumentation_parent_rebuilt (void)
{
#ifdef USFS_TESTING
    usfs_test_parent_rebuilt ();
#endif
}

void usfs_instrumentation_vnode_hold (const uint64_t reference_count)
{
#ifdef USFS_TESTING
    usfs_test_vnode_hold (reference_count);
#else
    ignore_parameter reference_count;
#endif
}

void usfs_instrumentation_vnode_release (const uint64_t reference_count)
{
#ifdef USFS_TESTING
    usfs_test_vnode_release (reference_count);
#else
    ignore_parameter reference_count;
#endif
}

int is_instrumentation_dev_ctl_command (const int command)
{
    switch (command)
    {
        case USFS_GCOV_IOC_INFO:
        case USFS_GCOV_IOC_SNAPSHOT:
        case USFS_GCOV_IOC_RESET:

        case USFS_TEST_IOC_INFO:
        case USFS_TEST_IOC_ARM:
        case USFS_TEST_IOC_STATUS:
        case USFS_TEST_IOC_RESET:
        case USFS_TEST_IOC_SCHEDULE_ARM:
        case USFS_TEST_IOC_SCHEDULE_STATUS:
        case USFS_TEST_IOC_SELFTEST:
        case USFS_TEST_IOC_NODE_STATUS:
        case USFS_TEST_IOC_EVICT_RELEASE:
        case USFS_TEST_IOC_FID_PROBE:
            return true;

        default:
            return false;
    }
}

int process_instrumentation_dev_ctl_command (const int command, void * user_space_buffer, const chan_t channel)
{
    ignore_parameter user_space_buffer;
    ignore_parameter channel;

    switch (command)
    {
#ifdef USFS_COVERAGE
        case USFS_GCOV_IOC_INFO:
        case USFS_GCOV_IOC_SNAPSHOT:
        case USFS_GCOV_IOC_RESET:
            return usfs_gcov_control_ioctl (command, user_space_buffer, channel);
#endif

#ifdef USFS_TESTING
        case USFS_TEST_IOC_INFO:
        case USFS_TEST_IOC_ARM:
        case USFS_TEST_IOC_STATUS:
        case USFS_TEST_IOC_RESET:
        case USFS_TEST_IOC_SCHEDULE_ARM:
        case USFS_TEST_IOC_SCHEDULE_STATUS:
        case USFS_TEST_IOC_NODE_STATUS:
        case USFS_TEST_IOC_EVICT_RELEASE:
        case USFS_TEST_IOC_FID_PROBE:
        case USFS_TEST_IOC_SELFTEST:
            return usfs_test_control_ioctl (command, user_space_buffer, channel);
#endif
        default:
            return EINVAL;
    }
}
