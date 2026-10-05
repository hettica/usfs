// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_CONFIGURATION_TRANSACTIONS_H
#define USFS_CONFIGURATION_TRANSACTIONS_H

/* Included by the configuration implementation after its AIX service helpers.
 * The native unit suite supplies deterministic services to exercise failure
 * ownership without risking a live registration on the preceding revision. */

/* Resolves (or assigns) the major number for the driver. Returns USFS_SUCCESS. */
static int usfs_get_major_number (int * major)
{
    const int result = genmajor ((char *)USFS_DRIVER_NAME);
    if (result < 0)
    {
        return USFS_FAILURE;
    }

    *major = result;
    return USFS_SUCCESS;
}

/*
 * Allocates a fresh minor number and composes the full device number. Used by
 * the Configure flow.
 */
static int usfs_allocate_device_number (dev_t * device_number)
{
    /* genminor() tuning: -1 lets ODM pick the base minor; one number, step 1. */
    const int minor_base_any = -1;
    const int minor_count = 1;
    const int minor_step = 1;

    int major = 0;
    if (usfs_get_major_number (&major) != USFS_SUCCESS)
    {
        return USFS_FAILURE;
    }

    const int * minor = genminor ((char *)USFS_DRIVER_NAME, major, minor_base_any, minor_count, minor_step, minor_step);
    if (minor == NULL)
    {
        return USFS_FAILURE;
    }

    *device_number = makedev64 (major, *minor);
    return USFS_SUCCESS;
}

/*
 * Looks up the already-assigned minor number and composes the full device
 * number. Used by the Unconfigure flow.
 */
static int usfs_lookup_device_number (dev_t * device_number)
{
    int major = 0;
    if (usfs_get_major_number (&major) != USFS_SUCCESS)
    {
        return USFS_QUERY_ERROR;
    }

    int minor_count = -1;
    const int * minor = getminor (major, &minor_count, (char *)USFS_DRIVER_NAME);
    if (minor == NULL && minor_count == 0)
    {
        /* getminor documents NULL with count zero as verified absence.
         * Keep the major until kernel teardown is confirmed by the caller. */
        return USFS_QUERY_ABSENT;
    }
    if (minor == NULL || minor_count != 1)
        return USFS_QUERY_ERROR;

    *device_number = makedev64 (major, *minor);
    return USFS_QUERY_PRESENT;
}

/* Releases the driver's major (and its minors) back to ODM. Returns USFS_SUCCESS. */
static int usfs_release_device_numbers (int number_state)
{
    /* TODO: release only this instance's minor; free the major with the last
     * instance once multiple logical devices are supported. */
    const int release_major = 1;

    /* reldevno requires a minor record. A retry after minor cleanup may own
     * only the major assigned by genmajor; relmajor is the documented API. */
    if (number_state == USFS_QUERY_ABSENT)
        return relmajor ((char *)USFS_DRIVER_NAME) == 0 ? USFS_SUCCESS : USFS_FAILURE;
    if (number_state != USFS_QUERY_PRESENT)
        return USFS_FAILURE;
    if (reldevno ((char *)USFS_DRIVER_NAME, release_major) != 0)
    {
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

static int usfs_query_kext_lifecycle (mid_t module_id, dev_t device_number, uint32_t * state_value)
{
    struct cfg_dd sysquery;
    struct usfs_kext_lifecycle_query request;
    struct usfs_kext_lifecycle_state state;

    memset (&sysquery, 0, sizeof (sysquery));
    memset (&request, 0, sizeof (request));
    memset (&state, 0, sizeof (state));
    request.magic = USFS_CONFIG_DDS_MAGIC;
    request.abi_version = USFS_CONFIG_ABI_VERSION;
    request.size = sizeof (request);
    request.user_buffer = (uint64_t)(unsigned long)&state;
    sysquery.kmid = module_id;
    sysquery.cmd = USFS_CFG_QUERY_LIFECYCLE;
    sysquery.devno = device_number;
    sysquery.ddsptr = (caddr_t)&request;
    sysquery.ddslen = sizeof (request);
    if (usfs_config_instrumentation_checkpoint (
            USFS_CONFIG_METHOD_LIFECYCLE_QUERY
        ) != USFS_SUCCESS ||
        sysconfig (SYS_CFGDD, &sysquery, sizeof (sysquery)) != 0)
        return USFS_QUERY_ERROR;
    if (state.magic != USFS_CONFIG_DDS_MAGIC ||
        state.abi_version != USFS_CONFIG_ABI_VERSION ||
        state.size != sizeof (state) ||
        state.state > USFS_KEXT_STATE_CLEANUP_REQUIRED ||
        state.reserved[0] != 0 || state.reserved[1] != 0 ||
        state.reserved[2] != 0)
        return USFS_QUERY_ERROR;
    *state_value = state.state;
    return USFS_QUERY_PRESENT;
}

int usfs_kext_unload (void)
{
    const mid_t module_id = usfs_kext_get_module_id ();
    uint32_t lifecycle_state;
    if (module_id == USFS_MODULE_ID_NONE)
    {
        return USFS_FAILURE;
    }

    /* SYS_QUERYLOAD establishes presence, not termination. Never rely on the
     * loader to protect live or uncertain registrations from SYS_KULOAD. */
    if (usfs_query_kext_lifecycle (module_id, 0, &lifecycle_state) !=
            USFS_QUERY_PRESENT ||
        lifecycle_state != USFS_KEXT_STATE_DOWN)
        return USFS_FAILURE;

    struct cfg_load query = usfs_kext_make_query ();
    query.kmid = module_id;

    if (sysconfig (SYS_KULOAD, &query, sizeof (query)) != 0)
    {
        return USFS_FAILURE;
    }

    return USFS_SUCCESS;
}

/* ------------------------------------------------------------------------- *
 * Device configure / unconfigure (device-driver level)
 * ------------------------------------------------------------------------- */

/*
 * Configures the device-driver instance for <logical_device_name>: allocates
 * device numbers, creates the /dev node, runs USFS_CFG_INIT and marks the CuDv
 * object AVAILABLE.
 */
static int usfs_discard_device_records (const char * logical_device_name, dev_t device_number)
{
    if (usfs_remove_device_node (logical_device_name, device_number, USFS_QUERY_PRESENT) != USFS_SUCCESS)
        return USFS_FAILURE;
    return usfs_release_device_numbers (USFS_QUERY_PRESENT);
}

static int usfs_kernel_is_down (mid_t module_id, dev_t device_number)
{
    uint32_t state;
    return usfs_query_kext_lifecycle (module_id, device_number, &state) ==
               USFS_QUERY_PRESENT &&
           state == USFS_KEXT_STATE_DOWN;
}

static int usfs_configure_device (const char * logical_device_name, int * cleanup_complete)
{
    const mid_t module_id = usfs_kext_get_module_id ();
    dev_t device_number = 0;
    int number_state;

    *cleanup_complete = 0;
    if (module_id == USFS_MODULE_ID_NONE)
        return USFS_FAILURE;

    number_state = usfs_lookup_device_number (&device_number);
    if (number_state == USFS_QUERY_ERROR)
        return USFS_FAILURE;
    /* An earlier failed invocation may still own this registration. Retain
     * its number and require successful unconfiguration before a new INIT. */
    if (!usfs_kernel_is_down (module_id, device_number))
        return USFS_FAILURE;
    if (number_state == USFS_QUERY_ABSENT &&
        usfs_allocate_device_number (&device_number) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (usfs_create_device_node (logical_device_name, device_number) != USFS_SUCCESS)
    {
        *cleanup_complete = usfs_discard_device_records (logical_device_name, device_number) ==
                            USFS_SUCCESS;
        return USFS_FAILURE;
    }

    if (usfs_devsw_send_command (module_id, device_number, USFS_CFG_INIT) != USFS_SUCCESS)
    {
        if (usfs_kernel_is_down (module_id, device_number))
            *cleanup_complete = usfs_discard_device_records (logical_device_name, device_number) ==
                                USFS_SUCCESS;
        return USFS_FAILURE;
    }

    if (usfs_mark_device_available (logical_device_name) != USFS_SUCCESS)
    {
        if (usfs_devsw_send_command (module_id, device_number, USFS_CFG_TERM) ==
                USFS_SUCCESS &&
            usfs_kernel_is_down (module_id, device_number))
            *cleanup_complete = usfs_discard_device_records (logical_device_name, device_number) ==
                                USFS_SUCCESS;
        return USFS_FAILURE;
    }

    *cleanup_complete = 1;
    return USFS_SUCCESS;
}

static int usfs_ensure_kernel_device_down (mid_t module_id, dev_t device_number, int number_state)
{
    uint32_t lifecycle_state = USFS_KEXT_STATE_DOWN;

    if (usfs_query_kext_lifecycle (module_id, device_number, &lifecycle_state) != USFS_QUERY_PRESENT)
        return USFS_FAILURE;
    if (lifecycle_state == USFS_KEXT_STATE_DOWN)
        return USFS_SUCCESS;
    if (lifecycle_state != USFS_KEXT_STATE_ACTIVE &&
        lifecycle_state != USFS_KEXT_STATE_CLEANUP_REQUIRED)
        return USFS_FAILURE;

    if (number_state != USFS_QUERY_PRESENT ||
        usfs_devsw_send_command (module_id, device_number, USFS_CFG_TERM) !=
            USFS_SUCCESS ||
        !usfs_kernel_is_down (module_id, device_number))
        return USFS_FAILURE;

    return usfs_config_instrumentation_checkpoint (
        USFS_CONFIG_METHOD_AFTER_CFG_TERM
    );
}

/*
 * Unconfigures the device-driver instance for <logical_device_name>: runs
 * USFS_CFG_TERM, releases device numbers, marks the CuDv object DEFINED and removes
 * the /dev node.
 */
static int usfs_unconfigure_device (const char * logical_device_name, int kernel_loaded)
{
    dev_t device_number = 0;
    const int number_state = usfs_lookup_device_number (&device_number);

    if (number_state == USFS_QUERY_ERROR)
        return USFS_FAILURE;

    /* Establish pathname ownership before terminating the live extension. */
    if (usfs_validate_device_node (logical_device_name, device_number, number_state) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (kernel_loaded)
    {
        const mid_t module_id = usfs_kext_get_module_id ();

        if (module_id == USFS_MODULE_ID_NONE ||
            usfs_ensure_kernel_device_down (module_id, device_number, number_state) != USFS_SUCCESS)
            return USFS_FAILURE;
    }

    if (usfs_remove_device_node (logical_device_name, device_number, number_state) != USFS_SUCCESS)
        return USFS_FAILURE;
    if (usfs_release_device_numbers (number_state) != USFS_SUCCESS)
        return USFS_FAILURE;
    if (usfs_config_instrumentation_checkpoint (
            USFS_CONFIG_METHOD_AFTER_DEVICE_CLEANUP
        ) != USFS_SUCCESS)
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

/* ------------------------------------------------------------------------- *
 * High-level orchestration
 * ------------------------------------------------------------------------- */

static int usfs_configuration_identity_valid (const char * logical_device_name)
{
    if (logical_device_name == NULL ||
        strcmp (logical_device_name, USFS_LOGICAL_DEVICE_NAME) != 0 ||
        usfs_device_is_available (logical_device_name) == USFS_QUERY_ERROR)
        return USFS_FAILURE;

    dev_t existing_number = 0;
    const int existing_state = usfs_lookup_device_number (&existing_number);
    if (existing_state == USFS_QUERY_ERROR ||
        usfs_validate_device_node (logical_device_name, existing_number, existing_state) != USFS_SUCCESS)
        return USFS_FAILURE;

    return USFS_SUCCESS;
}

static int usfs_load_extension_for_configuration (int * loaded_here)
{
    const int loaded = usfs_kext_is_loaded ();

    *loaded_here = 0;
    if (loaded == USFS_QUERY_ERROR)
        return USFS_FAILURE;
    if (loaded != USFS_QUERY_ABSENT)
        return USFS_SUCCESS;

    if (usfs_kext_load () != USFS_SUCCESS)
        return USFS_FAILURE;
    *loaded_here = 1;
    return usfs_config_instrumentation_checkpoint (
        USFS_CONFIG_METHOD_AFTER_LOAD
    );
}

static int usfs_ensure_device_configured (const char * logical_device_name, int loaded_here, int * configured_here, int * cleanup_complete)
{
    int available = usfs_device_is_available (logical_device_name);

    if (available == USFS_QUERY_ERROR)
        return USFS_FAILURE;
    if (loaded_here && available == USFS_QUERY_PRESENT)
    {
        /* CuDv cannot truthfully be AVAILABLE when its extension was absent.
         * Convert that interrupted-operation residue back to a retryable
         * defined state before rebuilding the device registration. */
        if (usfs_mark_device_defined (logical_device_name) != USFS_SUCCESS)
            return USFS_FAILURE;
        available = USFS_QUERY_ABSENT;
    }

    *configured_here = 0;
    if (available != USFS_QUERY_ABSENT)
        return USFS_SUCCESS;

    if (usfs_configure_device (logical_device_name, cleanup_complete) !=
        USFS_SUCCESS)
        return USFS_FAILURE;

    *configured_here = 1;
    return usfs_config_instrumentation_checkpoint (
        USFS_CONFIG_METHOD_AFTER_DEVICE_CONFIG
    );
}

static void usfs_rollback_configuration (const char * logical_device_name, int configured_here, int loaded_here, int cleanup_complete)
{
    if (configured_here &&
        usfs_unconfigure_device (logical_device_name, 1) != USFS_SUCCESS)
        cleanup_complete = 0;
    if (loaded_here && cleanup_complete &&
        usfs_kext_is_loaded () == USFS_QUERY_PRESENT)
        cleanup_complete = usfs_kext_unload () == USFS_SUCCESS;
    if (configured_here && cleanup_complete)
        (void)usfs_mark_device_defined (logical_device_name);
}

static int usfs_configure_locked (const char * logical_device_name)
{
    int loaded_here = 0;
    int configured_here = 0;
    int cleanup_complete = 1;

    if (usfs_configuration_identity_valid (logical_device_name) != USFS_SUCCESS)
        return USFS_FAILURE;

    if (usfs_load_extension_for_configuration (&loaded_here) != USFS_SUCCESS)
    {
        usfs_rollback_configuration (logical_device_name, configured_here, loaded_here, cleanup_complete);
        return USFS_FAILURE;
    }

    if (usfs_ensure_device_configured (logical_device_name, loaded_here, &configured_here, &cleanup_complete) == USFS_SUCCESS)
    {
        /* Reconcile the entire entry with the current dynamic GFS type,
         * including residue and duplicates from interrupted transactions. */
        if (usfs_type_register (logical_device_name) == USFS_SUCCESS)
            return USFS_SUCCESS;
    }

    /* Reverse state introduced by this invocation. A failure is never
     * converted to success, even when the rollback itself succeeds. */
    usfs_rollback_configuration (logical_device_name, configured_here, loaded_here, cleanup_complete);
    return USFS_FAILURE;
}

int usfs_configure (const char * logical_device_name)
{
    const int lock_id = usfs_config_lock_acquire ();
    int rc;

    if (lock_id == -1)
        return USFS_FAILURE;
    rc = usfs_configure_locked (logical_device_name);
    if (usfs_config_lock_release (lock_id) != USFS_SUCCESS)
        rc = USFS_FAILURE;
    return rc;
}

static int usfs_query_unconfiguration_state (const char * logical_device_name, int * extension_state, int * device_state)
{
    *extension_state = usfs_kext_is_loaded ();
    if (*extension_state == USFS_QUERY_ERROR)
        return USFS_FAILURE;
    *device_state = usfs_device_is_available (logical_device_name);
    if (*device_state == USFS_QUERY_ERROR)
        return USFS_FAILURE;
    return USFS_SUCCESS;
}

static int usfs_unload_extension_if_present (int extension_state)
{
    if (extension_state != USFS_QUERY_PRESENT)
        return USFS_SUCCESS;
    if (usfs_kext_unload () != USFS_SUCCESS)
        return USFS_FAILURE;
    return usfs_config_instrumentation_checkpoint (
        USFS_CONFIG_METHOD_AFTER_UNLOAD
    );
}

static int usfs_unregister_type_if_present (void)
{
    const int registered = usfs_type_is_registered ();

    if (registered == USFS_QUERY_ERROR ||
        (registered == USFS_QUERY_PRESENT &&
         usfs_type_unregister () != USFS_SUCCESS))
        return USFS_FAILURE;
    return usfs_config_instrumentation_checkpoint (
        USFS_CONFIG_METHOD_AFTER_VFS_UNREGISTER
    );
}

static int usfs_unconfigure_locked (const char * logical_device_name)
{
    int extension_state;
    int device_state;

    if (logical_device_name == NULL ||
        strcmp (logical_device_name, USFS_LOGICAL_DEVICE_NAME) != 0)
        return USFS_FAILURE;
    if (usfs_query_unconfiguration_state (logical_device_name, &extension_state, &device_state) != USFS_SUCCESS)
        return USFS_FAILURE;

    /* Reconcile residue even if a previous attempt already changed CuDv or
     * unloaded the extension before encountering a later userspace error. */
    if (usfs_unconfigure_device (logical_device_name, extension_state == USFS_QUERY_PRESENT) !=
        USFS_SUCCESS)
        return USFS_FAILURE;
    if (usfs_unload_extension_if_present (extension_state) != USFS_SUCCESS)
        return USFS_FAILURE;
    if (usfs_unregister_type_if_present () != USFS_SUCCESS)
        return USFS_FAILURE;
    if (device_state == USFS_QUERY_PRESENT &&
        usfs_mark_device_defined (logical_device_name) != USFS_SUCCESS)
        return USFS_FAILURE;
    return USFS_SUCCESS;
}

int usfs_unconfigure (const char * logical_device_name)
{
    const int lock_id = usfs_config_lock_acquire ();
    int rc;

    if (lock_id == -1)
        return USFS_FAILURE;
    rc = usfs_unconfigure_locked (logical_device_name);
    if (usfs_config_lock_release (lock_id) != USFS_SUCCESS)
        rc = USFS_FAILURE;
    return rc;
}

#endif
