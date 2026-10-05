// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"
#include "instrumentation/adapter.h"
#include "instrumentation/services.h"
#include "device/operations/close.h"
#include "device/operations/ioctl.h"
#include "device/operations/mpx.h"
#include "device/operations/open.h"
#include "device/operations/read.h"
#include "device/operations/select.h"
#include "device/operations/write.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

/* Kernel extension configuration and teardown. */

enum
{
    CONFIGURATION_COMMAND_NONE = -1
};

/*
 * Registration ownership is recorded independently from the lifecycle state.
 * This permits USFS_CFG_TERM to retry a partial unwind without re-publishing an
 * entry point or losing track of a resource that remains registered.
 */
static int g_locks_are_initialized = false;
static int g_pager_is_registered = false;
static int g_filesystem_is_registered = false;
static int g_device_driver_is_registered = false;
static int g_configuration_command = CONFIGURATION_COMMAND_NONE;
static dev_t g_configured_device_number = 0;

struct devsw g_device_driver_descriptor;

/* Initializes the USFS device-driver descriptor. */
void init_device_driver_descriptor (void)
{
    memset (&g_device_driver_descriptor, 0, sizeof (g_device_driver_descriptor));

    /* Keep fully prototyped callbacks despite devsw's legacy callback fields. */

    // Install supported device entry points
    g_device_driver_descriptor.d_open = usfs_dev_open;
    g_device_driver_descriptor.d_close = usfs_dev_close;
    g_device_driver_descriptor.d_read = usfs_dev_read;
    g_device_driver_descriptor.d_write = usfs_dev_write;
    g_device_driver_descriptor.d_ioctl = usfs_dev_ioctl;
    g_device_driver_descriptor.d_mpx = usfs_dev_mpx;
    g_device_driver_descriptor.d_config = usfs_kext_entry;
    g_device_driver_descriptor.d_select = usfs_dev_select;

    // Reject unsupported device entry points
    g_device_driver_descriptor.d_strategy = nodev;
    g_device_driver_descriptor.d_print = nodev;
    g_device_driver_descriptor.d_dump = nodev;
    g_device_driver_descriptor.d_revoke = nodev;

    g_device_driver_descriptor.d_ttys = NULL;
    g_device_driver_descriptor.d_dsdptr = NULL;

    g_device_driver_descriptor.d_opts = DEV_MPSAFE | DEV_64BIT;
}

static void set_kext_state_code (const int new_state_code)
{
    const int old_state_code = usfs_lifecycle_state_replace (new_state_code);

    USFS_TRACE_CONTROL5 (USFS_TRACE_KEXT_STATE, old_state_code, new_state_code, g_configuration_command, 0, g_configured_device_number);
}

static int is_lifecycle_query_valid (const struct usfs_kext_lifecycle_query * query)
{
    if (query == NULL)
        return false;

    if (query->magic != USFS_CONFIG_DDS_MAGIC)
        return false;

    if (query->abi_version != USFS_CONFIG_ABI_VERSION)
        return false;

    if (query->size != sizeof (*query))
        return false;

    if (query->user_buffer == 0)
        return false;

    if (query->reserved[0] != 0)
        return false;

    if (query->reserved[1] != 0)
        return false;

    return true;
}

static int read_lifecycle_query (struct uio * user_io_request, struct usfs_kext_lifecycle_query * query)
{
    if (user_io_request == NULL)
        return EINVAL;

    if ((uint64_t)user_io_request->uio_resid != sizeof (*query))
        return EINVAL;

    memset (query, 0, sizeof (*query));

    const int rc = uiomove ((caddr_t)query, sizeof (*query), UIO_WRITE, user_io_request);
    if (rc != 0)
        return rc;

    if (!is_lifecycle_query_valid (query))
        return EINVAL;

    return 0;
}

static int send_lifecycle_state (const struct usfs_kext_lifecycle_query * query)
{
    struct usfs_kext_lifecycle_state state = { 0 };

    state.magic = USFS_CONFIG_DDS_MAGIC;
    state.abi_version = USFS_CONFIG_ABI_VERSION;
    state.size = sizeof (state);
    state.state = (uint32_t)usfs_lifecycle_state_read ();

    const int rc = copyout ((caddr_t)&state, (caddr_t)query->user_buffer, sizeof (state));
    if (rc != 0)
        return EFAULT;

    return 0;
}

static int process_query_lifecycle_command (struct uio * user_io_request)
{
    struct usfs_kext_lifecycle_query query;

    const int rc = read_lifecycle_query (user_io_request, &query);
    if (rc != 0)
        return rc;

    return send_lifecycle_state (&query);
}

static int validate_device_configuration (const struct usfs_dev_cfg * device_configuration)
{
    if (device_configuration->magic != USFS_CONFIG_DDS_MAGIC)
        return EINVAL;

    if (device_configuration->abi_version != USFS_CONFIG_ABI_VERSION)
        return EINVAL;

    if (device_configuration->size != sizeof (*device_configuration))
        return EINVAL;

    if (device_configuration->reserved != 0)
        return EINVAL;

    return validate_instrumentation_config (device_configuration);
}

static int read_device_configuration (struct uio * user_io_request, struct usfs_dev_cfg * device_configuration)
{
    memset (device_configuration, 0, sizeof (*device_configuration));

    if (user_io_request == NULL)
        return 0;

    if (user_io_request->uio_resid == 0)
        return 0;

    if ((uint64_t)user_io_request->uio_resid != sizeof (*device_configuration))
        return EINVAL;

    const int rc = uiomove ((caddr_t)device_configuration, sizeof (*device_configuration), UIO_WRITE, user_io_request);
    if (rc != 0)
        return rc;

    return validate_device_configuration (device_configuration);
}

static int remove_device_registration (const dev_t device_number)
{
    if (!g_device_driver_is_registered)
        return 0;

    const int rc = usfs_kdevswdel (device_number);
    USFS_TRACE_CONTROL5 (USFS_TRACE_REGISTRATION, USFS_TRACE_REG_DEVSW, USFS_TRACE_ACTION_REMOVE, rc, usfs_lifecycle_state_read (), device_number);
    if (rc != 0)
        return rc;

    g_device_driver_is_registered = false;

    return 0;
}

static int remove_filesystem_registration (void)
{
    if (!g_filesystem_is_registered)
        return 0;

    const int rc = usfs_kgfsdel (gfs.gfs_type);
    USFS_TRACE_CONTROL5 (USFS_TRACE_REGISTRATION, USFS_TRACE_REG_GFS, USFS_TRACE_ACTION_REMOVE, rc, usfs_lifecycle_state_read (), gfs.gfs_type);
    if (rc != 0)
        return rc;

    g_filesystem_is_registered = false;

    return 0;
}

static int remove_pager_registration (void)
{
    if (!g_pager_is_registered)
        return 0;

    const int rc = usfs_pager_unregister ();
    USFS_TRACE_CONTROL5 (USFS_TRACE_REGISTRATION, USFS_TRACE_REG_PAGER, USFS_TRACE_ACTION_REMOVE, rc, usfs_lifecycle_state_read (), 0);
    if (rc != 0)
        return rc;

    g_pager_is_registered = false;

    return 0;
}

static int remove_registrations (const dev_t device_number)
{
    int rc = remove_device_registration (device_number);
    if (rc != 0)
        return rc;

    /* Keep runtime locks until every callback that claimed the guard exits. */
    if (usfs_lifecycle_mpx_has_users ())
        return EBUSY;

    rc = remove_filesystem_registration ();
    if (rc != 0)
        return rc;

    return remove_pager_registration ();
}

static int remove_runtime_resources (const dev_t device_number)
{
    const int rc = remove_registrations (device_number);
    if (rc != 0)
        return rc;

    if (g_locks_are_initialized)
    {
        destroy_global_locks ();
        g_locks_are_initialized = false;
    }

    return 0;
}

/* Called while pinned, before the final instrumentation snapshot. A failed
 * initialization may have left registrations and locks for this retry. */
int usfs_prepare_deferred_cleanup (void)
{
    if (usfs_lifecycle_state_read () != USFS_KEXT_CLEANUP_REQUIRED)
        return EBUSY;

    const int rc = remove_runtime_resources (g_configured_device_number);
    if (rc != 0)
        report_cleanup_failure (rc, (uint32_t)g_mount_count);

    return rc;
}

static int unpin_cleaned_extension (void)
{
    tear_down_instrumentation ();

    // We set the final state before the unpin, because a successful unpin
    // forbids any later kext data modifications; a failed unpin restores the
    // retryable cleanup state below
    set_kext_state_code (USFS_KEXT_DOWN);

    const int rc = usfs_kunpin ();
    if (rc != 0)
    {
        report_cleanup_failure (rc, 0);
        set_kext_state_code (USFS_KEXT_CLEANUP_REQUIRED);
        return rc;
    }

    //! WARN: No module-data access is permitted after this point

    return 0;
}

static int finish_cleanup (const dev_t device_number, const int should_defer_cleanup, const uint32_t generation)
{
    const int rc = remove_runtime_resources (device_number);
    if (rc != 0)
    {
        if (should_defer_cleanup)
            on_kext_cleanup_deferred (generation);

        report_cleanup_failure (rc, (uint32_t)g_mount_count);
        set_kext_state_code (USFS_KEXT_CLEANUP_REQUIRED);
        return rc;
    }

    if (should_defer_cleanup)
    {
        on_kext_cleanup_deferred (generation);
        set_kext_state_code (USFS_KEXT_CLEANUP_REQUIRED);
        return 0;
    }

    return unpin_cleaned_extension ();
}

static int cleanup_failed_initialization (
    const dev_t device_number,
    const int should_defer_cleanup,
    const uint32_t generation,
    const int initialization_rc
)
{
    synchronized_with (g_lifecycle_lock)
    {
        g_gate_is_open = false;
    }

    set_kext_state_code (USFS_KEXT_STOPPING);

    const int cleanup_rc = finish_cleanup (device_number, should_defer_cleanup, generation);
    if (cleanup_rc != 0)
        return cleanup_rc;

    return initialization_rc;
}

static int pin_kext_memory (const struct usfs_dev_cfg * device_configuration)
{
    const int rc = usfs_kpin_initial ();
    if (rc == 0)
        return 0;

    const int should_defer_cleanup = should_defer_kext_cleanup (device_configuration);

    if (should_defer_cleanup)
    {
        on_kext_cleanup_deferred (device_configuration->generation);
        set_kext_state_code (USFS_KEXT_CLEANUP_REQUIRED);
        return rc;
    }

    set_kext_state_code (USFS_KEXT_DOWN);

    return rc;
}

static int register_pager (void)
{
    const int rc = usfs_pager_register ();

    USFS_TRACE_CONTROL5 (USFS_TRACE_REGISTRATION, USFS_TRACE_REG_PAGER, USFS_TRACE_ACTION_ADD, rc, usfs_lifecycle_state_read (), 0);

    if (rc == 0)
        g_pager_is_registered = true;

    return rc;
}

static int register_filesystem (void)
{
    const int rc = usfs_kgfs_register ();

    USFS_TRACE_CONTROL5 (USFS_TRACE_REGISTRATION, USFS_TRACE_REG_GFS, USFS_TRACE_ACTION_ADD, rc, usfs_lifecycle_state_read (), gfs.gfs_type);

    if (rc == 0)
        g_filesystem_is_registered = true;

    return rc;
}

static int register_device_driver (const dev_t device_number)
{
    const int rc = usfs_kdevswadd (device_number, &g_device_driver_descriptor);

    USFS_TRACE_CONTROL5 (USFS_TRACE_REGISTRATION, USFS_TRACE_REG_DEVSW, USFS_TRACE_ACTION_ADD, rc, usfs_lifecycle_state_read (), device_number);

    if (rc == 0)
        g_device_driver_is_registered = true;

    return rc;
}

static int register_and_activate_extension (const dev_t device_number, const struct usfs_dev_cfg * device_configuration)
{
    const int should_defer_cleanup = should_defer_kext_cleanup (device_configuration);
    const uint32_t generation = device_configuration->generation;

    int rc = initialize_instrumentation ();
    if (rc != 0)
        return cleanup_failed_initialization (device_number, should_defer_cleanup, generation, rc);

    rc = register_pager ();
    if (rc != 0)
        return cleanup_failed_initialization (device_number, should_defer_cleanup, generation, rc);

    rc = register_filesystem ();
    if (rc != 0)
        return cleanup_failed_initialization (device_number, should_defer_cleanup, generation, rc);

    rc = register_device_driver (device_number);
    if (rc != 0)
        return cleanup_failed_initialization (device_number, should_defer_cleanup, generation, rc);

    // Activate kext only after its callbacks can claim the runtime guard.
    int activation_rc = 0;

    synchronized_with (g_lifecycle_lock)
    {
        activation_rc = usfs_lifecycle_mpx_activate ();
        if (activation_rc == 0)
        {
            g_gate_is_open = true;
            set_kext_state_code (USFS_KEXT_ACTIVE);
        }
    }

    if (activation_rc != 0)
        return cleanup_failed_initialization (device_number, should_defer_cleanup, generation, activation_rc);

    return 0;
}

static int prepare_kext_configuration (struct uio * user_io_request, struct usfs_dev_cfg * device_configuration)
{
    int rc = read_device_configuration (user_io_request, device_configuration);
    if (rc != 0)
    {
        set_kext_state_code (USFS_KEXT_DOWN);
        return rc;
    }

    rc = set_up_instrumentation (device_configuration);
    if (rc != 0)
    {
        set_kext_state_code (USFS_KEXT_DOWN);
        return rc;
    }

    /* pin_kext_memory publishes the appropriate failure state itself. */
    return pin_kext_memory (device_configuration);
}

static void initialize_runtime_descriptors (void)
{
    init_global_lock ();
    g_locks_are_initialized = true;
    g_status_channel_present = false;

    init_global_virtual_fs_operations ();
    init_global_virtual_inode_operations ();
    init_global_gfs_descriptor ();
    init_device_driver_descriptor ();
}

static void record_configured_device (const dev_t device_number)
{
    g_configured_device_number = device_number;

    USFS_TRACE_CONTROL5 (
        USFS_TRACE_SCHEMA,
        USFS_TRACE_ABI_VERSION,
        USFS_PROTOCOL_VERSION,
        sizeof (unsigned long) * (unsigned)CHAR_BIT,
        gfs.gfs_type,
        device_number
    );
}

static int initialize_kext (const dev_t device_number, struct uio * user_io_request)
{
    const int initialization_claimed = usfs_lifecycle_state_claim (USFS_KEXT_DOWN, USFS_KEXT_STARTING);
    if (!initialization_claimed)
    {
        if (usfs_lifecycle_state_read () == USFS_KEXT_ACTIVE)
            return 0;

        return EBUSY;
    }

    record_configured_device (device_number);

    struct usfs_dev_cfg device_configuration;

    const int rc = prepare_kext_configuration (user_io_request, &device_configuration);
    if (rc != 0)
        return rc;

    initialize_runtime_descriptors ();

    return register_and_activate_extension (device_number, &device_configuration);
}

static int on_cleanup_started (const int prior_state)
{
    const int rc = usfs_checkpoint (USFS_INSTRUMENT_TERM_BEFORE_CLOSE);
    if (rc != 0)
        set_kext_state_code (prior_state);

    return rc;
}

static int has_runtime_users_locked (void)
{
    if (g_mount_count != 0)
        return true;

    if (g_status_channel_present)
        return true;

    if (usfs_lifecycle_mpx_has_users ())
        return true;

    return has_active_connections ();
}

static void restore_cleanup_gate_locked (const int prior_state)
{
    if (prior_state == USFS_KEXT_ACTIVE)
        g_gate_is_open = true;

    set_kext_state_code (prior_state);

    if (g_device_driver_is_registered)
        usfs_lifecycle_mpx_reopen ();
}

static int is_cleanup_blocked_locked (void)
{
    int kext_is_busy = false;

    synchronized_with (g_connections_table_lock)
    {
        kext_is_busy = has_runtime_users_locked ();

        if (!kext_is_busy && g_kext_is_running_control_operation == 0)
            kext_is_busy = !usfs_lifecycle_mpx_seal ();
    }

    return kext_is_busy || g_kext_is_running_control_operation != 0;
}

static int close_cleanup_gate_locked (const int prior_state)
{
    const int guard_rc = usfs_lifecycle_mpx_begin_close ();
    if (guard_rc != 0)
    {
        restore_cleanup_gate_locked (prior_state);
        return guard_rc;
    }

    g_gate_is_open = false;

    if (!is_cleanup_blocked_locked ())
        return 0;

    restore_cleanup_gate_locked (prior_state);

    return EBUSY;
}

static int close_gate (const int prior_state)
{
    int rc = 0;

    synchronized_with (g_lifecycle_lock)
    {
        rc = close_cleanup_gate_locked (prior_state);
    }

    if (rc == 0)
        return 0;

    (void)usfs_checkpoint (USFS_INSTRUMENT_TERM_BUSY_REOPENED);

    return EBUSY;
}

static int on_gate_closed (const int prior_state)
{
    const int rc = usfs_checkpoint (USFS_INSTRUMENT_TERM_GATE_CLOSED);
    if (rc == 0)
        return 0;

    synchronized_with (g_lifecycle_lock)
    {
        restore_cleanup_gate_locked (prior_state);
    }

    return rc;
}

static int prepare_cleanup_gate (const int prior_state)
{
    if (!g_locks_are_initialized)
        return 0;

    int rc = on_cleanup_started (prior_state);
    if (rc != 0)
        return rc;

    rc = close_gate (prior_state);
    if (rc != 0)
        return rc;

    return on_gate_closed (prior_state);
}

static int can_terminate_from_state (const int prior_state)
{
    if (prior_state == USFS_KEXT_ACTIVE)
        return true;

    return prior_state == USFS_KEXT_CLEANUP_REQUIRED;
}

static int clean_up_configured_extension (const dev_t device_number, const struct usfs_dev_cfg * device_configuration)
{
    const int prior_state = usfs_lifecycle_state_read ();

    if (prior_state == USFS_KEXT_DOWN)
        return 0;

    if (!can_terminate_from_state (prior_state))
        return EBUSY;

    if (!usfs_lifecycle_state_claim (prior_state, USFS_KEXT_STOPPING))
        return EBUSY;

    const int rc = prepare_cleanup_gate (prior_state);
    if (rc != 0)
        return rc;

    const int device_removal_rc = remove_device_registration (device_number);
    if (device_removal_rc != 0)
    {
        synchronized_with (g_lifecycle_lock)
        {
            restore_cleanup_gate_locked (prior_state);
        }

        return device_removal_rc;
    }

    const int should_defer_cleanup = should_defer_kext_cleanup (device_configuration);

    /* Successful final cleanup may unpin the module: do not access module data afterward. */
    return finish_cleanup (device_number, should_defer_cleanup, device_configuration->generation);
}

static int terminate_kext (const dev_t device_number, struct uio * user_io_request)
{
    struct usfs_dev_cfg device_configuration;

    const int rc = read_device_configuration (user_io_request, &device_configuration);
    if (rc != 0)
        return rc;

    if (device_number != g_configured_device_number)
        return EINVAL;

    return clean_up_configured_extension (device_number, &device_configuration);
}

static int process_init_command (const dev_t device_number, struct uio * user_io_request)
{
    const int old_state = usfs_lifecycle_state_read ();
    const int rc = initialize_kext (device_number, user_io_request);

    USFS_TRACE_CONTROL5 (USFS_TRACE_KEXT_STATE, old_state, usfs_lifecycle_state_read (), USFS_CFG_INIT, rc, device_number);

    return rc;
}

static int process_term_command (const dev_t device_number, struct uio * user_io_request)
{
    const int old_state = usfs_lifecycle_state_read ();

    /* No module-data access is permitted after full successful termination. */
    const int rc = terminate_kext (device_number, user_io_request);
    if (rc == 0)
        return 0;

    USFS_TRACE_CONTROL5 (USFS_TRACE_KEXT_STATE, old_state, usfs_lifecycle_state_read (), USFS_CFG_TERM, rc, device_number);

    return rc;
}

static int process_public_configuration_command (const dev_t device_number, const int command, struct uio * user_io_request)
{
    if (command == USFS_CFG_QUERY_LIFECYCLE)
        return process_query_lifecycle_command (user_io_request);

    if (command == USFS_CFG_INIT)
        return process_init_command (device_number, user_io_request);

    if (command == USFS_CFG_TERM)
        return process_term_command (device_number, user_io_request);

    return EINVAL;
}

static int is_public_configuration_command (const int command)
{
    if (command == USFS_CFG_QUERY_LIFECYCLE)
        return true;

    if (command == USFS_CFG_INIT)
        return true;

    return command == USFS_CFG_TERM;
}

int usfs_kext_entry (const dev_t device_number, const int command, struct uio * user_io_request)
{
    g_configuration_command = command;

    if (is_instrumentation_cfg_command (command))
        return process_instrumentation_cfg_command (command, user_io_request);

    if (is_public_configuration_command (command))
        return process_public_configuration_command (device_number, command, user_io_request);

    return EINVAL;
}
