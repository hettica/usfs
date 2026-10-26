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

int usfs_proxy_unsafe_unload(void)
{
    const int lock = usfs_config_lock_acquire();
    if (lock < 0) return USFS_FAILURE;
    int rc = USFS_FAILURE;
    mid_t module;
    struct usfs_proxy_status status;
    const int state = usfs_proxy_query_user(&module, &status);
    if (usfs_kext_is_loaded() != USFS_QUERY_ABSENT) goto done;
    if (state == USFS_QUERY_ABSENT) { rc = USFS_SUCCESS; goto done; }
    if (state != USFS_QUERY_PRESENT || status.state != USFS_PROXY_EMPTY || status.calls) goto done;
    if (status.abi && usfs_proxy_command(module, (dev_t)status.device, USFS_PROXY_UNSAFE_TERM, NULL, 0)) goto done;
    struct cfg_load unload;
    memset(&unload, 0, sizeof(unload));
    unload.kmid = module;
    if (sysconfig(SYS_KULOAD, &unload, sizeof(unload))) goto done;
    if (usfs_proxy_query_user(&module, &status) != USFS_QUERY_ABSENT) goto done;
    /* Only verified removal permits releasing the major/minor reservation. */
    int count = -1;
    const int major = genmajor((char *)USFS_DRIVER_NAME);
    if (major < 0) goto done;
    const int * minor = getminor(major, &count, (char *)USFS_DRIVER_NAME);
    if (minor && count == 1)
        rc = reldevno((char *)USFS_DRIVER_NAME, 1) ? USFS_FAILURE : USFS_SUCCESS;
    else if (!minor && count == 0)
        rc = relmajor((char *)USFS_DRIVER_NAME) ? USFS_FAILURE : USFS_SUCCESS;
done:
    if (usfs_config_lock_release(lock)) rc = USFS_FAILURE;
    return rc;
}
