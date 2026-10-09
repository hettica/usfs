/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "definitions.h"
#include "tap.h"

#include <string.h>

#ifdef USFS_COVERAGE
static int lifecycle = USFS_KEXT_CLEANUP_REQUIRED;
static int prepare_error, export_error, unpin_error;
static unsigned prepares, exports, unpins, shutdowns;
static uint32_t exported_generation;
static int unpin_observed_down;

int usfs_kensure_pinned(void) { return 0; }
int usfs_kcoverage_initialize(void) { return 0; }
void usfs_gcov_register_all(void) { }
int usfs_kcoverage_drain_fault(void) { return 0; }
void usfs_gcov_runtime_shutdown(void) { ++shutdowns; }
int usfs_prepare_deferred_cleanup(void) { ++prepares; return prepare_error; }
int usfs_lifecycle_state_replace(int desired)
{
    int previous = lifecycle;
    lifecycle = desired;
    return previous;
}
int usfs_kunpin_deferred(void)
{
    ++unpins;
    unpin_observed_down = lifecycle == USFS_KEXT_DOWN;
    return unpin_error;
}
int usfs_gcov_config_export(struct uio *uiop, int reset_after, uint32_t generation)
{
    (void)uiop;
    (void)reset_after;
    ++exports;
    exported_generation = generation;
    return export_error;
}
int usfs_gcov_control_ioctl(int command, void *argument, chan_t collector)
{
    (void)command; (void)argument; (void)collector;
    return EINVAL;
}

int main(void)
{
    struct tap_state tap;
    tap_plan(&tap, 8);
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_DRAIN, NULL) == EBUSY,
           "drain without deferred ownership fails");
    on_kext_cleanup_deferred(73);
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_EXPORT, NULL) == 0 &&
           prepares == 0 && exports == 1 && unpins == 0 && exported_generation == 73,
           "export preserves registration and pin ownership");
    prepare_error = EBUSY;
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_DRAIN, NULL) == EBUSY &&
           prepares == 1 && exports == 2 && unpins == 0 && lifecycle == USFS_KEXT_CLEANUP_REQUIRED,
           "failed registration cleanup exports evidence and retains the pin");
    prepare_error = 0;
    export_error = EFAULT;
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_DRAIN, NULL) == EFAULT &&
           prepares == 2 && unpins == 0 && shutdowns == 0,
           "failed export preserves deferred ownership for retry");
    export_error = 0;
    unpin_error = EBUSY;
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_DRAIN, NULL) == EBUSY &&
           unpins == 1 && shutdowns == 1 && unpin_observed_down && lifecycle == USFS_KEXT_CLEANUP_REQUIRED,
           "failed unpin restores retryable lifecycle after terminal publication");
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_EXPORT, NULL) == 0 &&
           exported_generation == 73 && unpins == 1,
           "failed unpin retains the original export generation");
    unpin_error = 0;
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_DRAIN, NULL) == 0 &&
           prepares == 4 && unpins == 2 && shutdowns == 2 && unpin_observed_down && lifecycle == USFS_KEXT_DOWN,
           "successful retry completes cleanup before final unpin");
    tap_ok(&tap, process_instrumentation_cfg_command(USFS_CFG_INSTRUMENTATION_DRAIN, NULL) == EBUSY &&
           unpins == 2, "completed drain cannot release the pin twice");
    return tap_finish(&tap);
}
#else
int main (void)
{
    struct tap_state tap;
    struct usfs_dev_cfg configuration;

    tap_plan (&tap, 11);
    memset (&configuration, 0, sizeof (configuration));

    configuration.magic = USFS_CONFIG_DDS_MAGIC;
    configuration.abi_version = USFS_CONFIG_ABI_VERSION;
    configuration.size = sizeof (configuration);
    tap_ok (&tap, usfs_instrumentation_config_flags () == 0, "normal adapter accepts no instrumentation flags");
    tap_ok (&tap, validate_instrumentation_config (&configuration) == 0, "normal adapter accepts empty instrumentation configuration");

    configuration.flags = 0x80000000u;
    tap_ok (&tap, validate_instrumentation_config (&configuration) == EINVAL, "normal adapter rejects unknown flags");

    configuration.flags = USFS_CONFIG_FLAG_INSTRUMENTATION;
    tap_ok (&tap, validate_instrumentation_config (&configuration) == EINVAL, "normal adapter rejects testing payload flag");

    configuration.flags = USFS_CONFIG_FLAG_DEFERRED_EXPORT;
    configuration.generation = 1;
    tap_ok (&tap, validate_instrumentation_config (&configuration) == EINVAL, "normal adapter rejects deferred coverage export");
    configuration.flags = 0;
    configuration.generation = 0;
    configuration.instrumentation[0] = 1;
    tap_ok (&tap, validate_instrumentation_config (&configuration) == EINVAL, "normal adapter rejects unflagged opaque payload");

    memset (configuration.instrumentation, 0, sizeof (configuration.instrumentation));
    tap_ok (&tap, set_up_instrumentation (&configuration) == 0 && initialize_instrumentation () == 0, "normal adapter lifecycle initialization is a no-op");
    tap_ok (&tap, !should_defer_kext_cleanup (&configuration), "normal adapter never defers cleanup");
    tap_ok (&tap, process_instrumentation_cfg_command (123, NULL) == EINVAL, "normal adapter rejects private configuration commands");
    tap_ok (&tap, !is_instrumentation_dev_ctl_command (123), "normal adapter leaves device commands unhandled");
    tap_ok (&tap, usfs_checkpoint (USFS_INSTRUMENT_MOUNT_RESERVED) == 0, "normal adapter checkpoint cannot change production flow");
    return tap_finish (&tap);
}
#endif
