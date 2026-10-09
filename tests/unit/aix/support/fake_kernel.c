/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "fake_kernel.h"

#ifdef free
#undef free
#endif

struct usfs_fake_kernel_state usfs_fake_kernel;
heapaddr_t kernel_heap;

void usfs_fake_kernel_reset(void)
{
    memset(&usfs_fake_kernel, 0, sizeof(usfs_fake_kernel));
    usfs_fake_kernel.member_gid = (gid_t)-1;
    usfs_fake_kernel.attr.va_type = VREG;
    usfs_fake_kernel.attr.va_mode = 0644;
    usfs_fake_kernel.attr.va_uid = 1000;
    usfs_fake_kernel.attr.va_gid = 100;
}

void usfs_fake_kernel_event(enum usfs_fake_event event)
{
    if (usfs_fake_kernel.event_count < USFS_FAKE_EVENT_CAPACITY)
        usfs_fake_kernel.events[usfs_fake_kernel.event_count] = event;
    usfs_fake_kernel.event_count += 1;
}

int usfs_fake_kernel_clean(void)
{
    return usfs_fake_kernel.allocation_balance == 0 &&
           usfs_fake_kernel.allocation_errors == 0 &&
           usfs_fake_kernel.lock_depth == 0 &&
           usfs_fake_kernel.event_count <= USFS_FAKE_EVENT_CAPACITY;
}

void *usfs_kmalloc(enum usfs_allocation_site site, uint size, int align,
                   heapaddr_t heap)
{
    void *allocation;
    unsigned index;

    (void)align;
    (void)heap;
    usfs_fake_kernel.allocation_calls += 1;
    if (site == usfs_fake_kernel.allocation_fail_site) {
        usfs_fake_kernel.allocation_fail_site = 0;
        return NULL;
    }
    allocation = calloc(1, (size_t)size);
    if (allocation == NULL)
        return NULL;
    for (index = 0; index < 32u; index++) {
        if (usfs_fake_kernel.allocations[index] == NULL) {
            usfs_fake_kernel.allocations[index] = allocation;
            usfs_fake_kernel.allocation_balance += 1;
            return allocation;
        }
    }
    free(allocation);
    usfs_fake_kernel.allocation_errors += 1;
    return NULL;
}

int xmfree(void *allocation, heapaddr_t heap)
{
    unsigned index;

    (void)heap;
    if (allocation == NULL)
        return 0;
    for (index = 0; index < 32u; index++) {
        if (usfs_fake_kernel.allocations[index] == allocation) {
            usfs_fake_kernel.allocations[index] = NULL;
            usfs_fake_kernel.allocation_balance -= 1;
            free(allocation);
            return 0;
        }
    }
    usfs_fake_kernel.allocation_errors += 1;
    return EINVAL;
}

int privcheck_cr(int privilege, struct ucred *credential)
{
    int allowed = 0;

    (void)credential;
    usfs_fake_kernel.privcheck_calls += 1;
    usfs_fake_kernel_event(USFS_FAKE_EVENT_PRIVCHECK);
    if (privilege == BYPASS_DAC)
        allowed = usfs_fake_kernel.bypass_dac;
    else if (privilege == BYPASS_DAC_READ)
        allowed = usfs_fake_kernel.bypass_read;
    else if (privilege == BYPASS_DAC_WRITE)
        allowed = usfs_fake_kernel.bypass_write;
    else if (privilege == BYPASS_DAC_EXEC)
        allowed = usfs_fake_kernel.bypass_exec;
    else if (privilege == SET_OBJ_DAC)
        allowed = usfs_fake_kernel.set_obj_dac;
    return allowed ? 0 : EPERM;
}

int groupmember_cr(gid_t group, struct ucred *credential)
{
    (void)credential;
    usfs_fake_kernel.groupmember_calls += 1;
    usfs_fake_kernel_event(USFS_FAKE_EVENT_GROUPMEMBER);
    return usfs_fake_kernel.group_member &&
           group == usfs_fake_kernel.member_gid;
}
