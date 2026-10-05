// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_INSTRUMENTATION_SERVICES_H
#define USFS_INSTRUMENTATION_SERVICES_H

enum usfs_allocation_site
{
    USFS_ALLOC_REQUEST = 1,
    USFS_ALLOC_REQUEST_MESSAGE = 2,
    USFS_ALLOC_REQUEST_REPLY = 3,
    USFS_ALLOC_GNODE = 4,
    USFS_ALLOC_NODE = 5,
    USFS_ALLOC_CONNECTION = 6,
    USFS_ALLOC_OPEN_STATE = 7,
    USFS_ALLOC_REQUEST_WAIT = 8
};

enum usfs_uiomove_site
{
    USFS_UIOMOVE_REPLY_BODY = 1,
    USFS_UIOMOVE_WRITE_DATA = 2
};

void * usfs_kmalloc (const enum usfs_allocation_site site, const uint size_bytes, const int alignment, const heapaddr_t heap);
int usfs_kuiomove (
    const enum usfs_uiomove_site site,
    const caddr_t address,
    const long byte_count,
    const int direction,
    struct uio * user_io_request
);
int usfs_kpin_initial (void);
int usfs_kensure_pinned (void);
int usfs_kunpin (void);
int usfs_kunpin_deferred (void);
int usfs_kcoverage_drain_fault (void);
int usfs_kgfs_register (void);
int usfs_kgfsdel (const int filesystem_type);
int usfs_kdevswadd (const dev_t device_number, struct devsw * device_switch);
int usfs_kdevswdel (const dev_t device_number);
int usfs_kcoverage_initialize (void);

#endif
