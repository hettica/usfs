// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT
#include "usfs_common.h"
#include "usfs_proxy.h"

int usfs_proxy_command(mid_t module, dev_t device, int command, void * data, size_t size)
{
    struct cfg_dd request;
    memset(&request, 0, sizeof(request));
    request.kmid = module;
    request.devno = device;
    request.cmd = command;
    request.ddsptr = data;
    request.ddslen = (int)size;
    return sysconfig(SYS_CFGDD, &request, sizeof(request));
}

int usfs_proxy_query_user(mid_t * module, struct usfs_proxy_status * status)
{
    struct cfg_load query;
    memset(&query, 0, sizeof(query));
    memset(status, 0, sizeof(*status));
    *module = 0;
    query.path = (char *)USFS_PROXY_PATH;
    query.libpath = (char *)"";
    if (sysconfig(SYS_QUERYLOAD, &query, sizeof(query))) return USFS_QUERY_ERROR;
    *module = query.kmid;
    if (!query.kmid) return USFS_QUERY_ABSENT;
    if (usfs_proxy_command(query.kmid, 0, USFS_PROXY_QUERY, status, sizeof(*status)))
    {
        /* A loaded but not yet initialized proxy can be retried by configure. */
        if (errno == ENODEV) return USFS_QUERY_PRESENT;
        return USFS_QUERY_ERROR;
    }
    if (status->abi != USFS_PROXY_ABI || status->size != sizeof(*status) ||
        !memchr(status->proxy_build, 0, sizeof(status->proxy_build)) ||
        !memchr(status->implementation_build, 0, sizeof(status->implementation_build)) ||
        !memchr(status->variant, 0, sizeof(status->variant)))
    { errno = EPROTO; return USFS_QUERY_ERROR; }
    return USFS_QUERY_PRESENT;
}

int usfs_proxy_ensure_loaded(void)
{
    mid_t module;
    struct usfs_proxy_status status;
    const int state = usfs_proxy_query_user(&module, &status);
    if (state == USFS_QUERY_ERROR)
    {
        if (errno == EPROTO)
            fprintf(stderr, "USFS: resident proxy ABI is incompatible; reboot or remove the proxy during explicit maintenance after unloading USFS.\n");
        return USFS_FAILURE;
    }
    if (state == USFS_QUERY_PRESENT) return USFS_SUCCESS;
    struct cfg_load load;
    memset(&load, 0, sizeof(load));
    load.path = (char *)USFS_PROXY_PATH;
    load.libpath = (char *)"";
    return sysconfig(SYS_KLOAD, &load, sizeof(load)) ? USFS_FAILURE : USFS_SUCCESS;
}

int usfs_proxy_prepare_device(dev_t device)
{
    mid_t module;
    struct usfs_proxy_status status;
    if (usfs_proxy_query_user(&module, &status) != USFS_QUERY_PRESENT) return USFS_FAILURE;
    if (status.registered && status.pager_registered)
        return status.device == (uint64_t)device ? USFS_SUCCESS : USFS_FAILURE;
    return usfs_proxy_command(module, device, CFG_INIT, NULL, 0) ? USFS_FAILURE : USFS_SUCCESS;
}

int usfs_proxy_detached(void)
{
    mid_t module;
    struct usfs_proxy_status status;
    const int state = usfs_proxy_query_user(&module, &status);
    return state == USFS_QUERY_ABSENT ||
        (state == USFS_QUERY_PRESENT && status.state == USFS_PROXY_EMPTY && !status.calls);
}

mid_t usfs_proxy_control_module(mid_t implementation_module)
{
    mid_t module;
    struct usfs_proxy_status status;
    const int state = usfs_proxy_query_user(&module, &status);
    if (state == USFS_QUERY_ERROR) return 0;
    if (state == USFS_QUERY_PRESENT && status.state != USFS_PROXY_EMPTY) return module;
    return implementation_module;
}

static int release_device_reservations (char * driver_name)
{
    const int major_number = genmajor (driver_name);
    if (major_number < 0)
        return USFS_FAILURE;

    int minor_count = -1;
    const int * minor_number = getminor (major_number, &minor_count, driver_name);

    if (minor_number != NULL && minor_count == 1)
        return reldevno (driver_name, 1) ? USFS_FAILURE : USFS_SUCCESS;

    if (minor_number == NULL && minor_count == 0)
        return relmajor (driver_name) ? USFS_FAILURE : USFS_SUCCESS;

    return USFS_FAILURE;
}

static int release_proxy_device_numbers (void)
{
    // ODM accepts a mutable driver name; keep the shared name const-qualified.
    char * driver_name = strdup (USFS_DRIVER_NAME);
    if (driver_name == NULL)
        return USFS_FAILURE;

    const int rc = release_device_reservations (driver_name);

    free (driver_name);

    return rc;
}

// The caller holds the configuration lock throughout teardown and ODM cleanup.
static int unload_detached_proxy (void)
{
    mid_t module = 0;
    struct usfs_proxy_status status = { 0 };
    const int state = usfs_proxy_query_user (&module, &status);

    if (usfs_kext_is_loaded () != USFS_QUERY_ABSENT)
        return USFS_FAILURE;

    if (state == USFS_QUERY_ABSENT)
        return USFS_SUCCESS;

    if (state != USFS_QUERY_PRESENT)
        return USFS_FAILURE;

    if (status.state != USFS_PROXY_EMPTY)
        return USFS_FAILURE;

    if (status.calls != 0)
        return USFS_FAILURE;

    if (status.abi != 0)
    {
        const int rc = usfs_proxy_command (module, (dev_t)status.device, USFS_PROXY_UNSAFE_TERM, NULL, 0);
        if (rc != 0)
            return USFS_FAILURE;
    }

    struct cfg_load unload = { 0 };

    unload.kmid = module;

    if (sysconfig (SYS_KULOAD, &unload, sizeof (unload)) != 0)
        return USFS_FAILURE;

    if (usfs_proxy_query_user (&module, &status) != USFS_QUERY_ABSENT)
        return USFS_FAILURE;

    // Only verified removal permits releasing the major/minor reservation.
    return release_proxy_device_numbers ();
}

int usfs_proxy_unsafe_unload (void)
{
    const int configuration_lock = usfs_config_lock_acquire ();
    if (configuration_lock < 0)
        return USFS_FAILURE;

    int rc = unload_detached_proxy ();

    if (usfs_config_lock_release (configuration_lock) != 0)
        rc = USFS_FAILURE;

    return rc;
}
