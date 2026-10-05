// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"

#ifdef USFS_TESTING
    #include "instrumentation/testing/runtime.h"
#endif

#ifdef USFS_COVERAGE
    #include "instrumentation/coverage/runtime.h"
#endif

static int is_module_pinned;

static int check_allocation_fault (const enum usfs_allocation_site site)
{
#ifdef USFS_TESTING
    static const int faults[] = { USFS_TEST_FAULT_NONE,
                                  USFS_TEST_FAULT_REQUEST_ALLOC,
                                  USFS_TEST_FAULT_REQUEST_MESSAGE_ALLOC,
                                  USFS_TEST_FAULT_REQUEST_REPLY_ALLOC,
                                  USFS_TEST_FAULT_GNODE_ALLOC,
                                  USFS_TEST_FAULT_NODE_ALLOC,
                                  USFS_TEST_FAULT_CONNECTION_ALLOC,
                                  USFS_TEST_FAULT_OPEN_STATE_ALLOC,
                                  USFS_TEST_FAULT_REQUEST_WAIT_ALLOC };

    return usfs_test_fault (faults[(int)site]);
#else
    ignore_parameter site;
    return 0;
#endif
}

void * usfs_kmalloc (const enum usfs_allocation_site site, const uint size_bytes, const int alignment, const heapaddr_t heap)
{
    if (check_allocation_fault (site) != 0)
        return NULL;

    return xmalloc (size_bytes, alignment, heap);
}

static int check_uiomove_fault (const enum usfs_uiomove_site site)
{
#ifdef USFS_TESTING
    static const int faults[] = { USFS_TEST_FAULT_NONE, USFS_TEST_FAULT_REPLY_COPY, USFS_TEST_FAULT_WRITE_COPY };

    return usfs_test_fault (faults[(int)site]);
#else
    ignore_parameter site;
    return 0;
#endif
}

int usfs_kuiomove (const enum usfs_uiomove_site site, const caddr_t address, const long byte_count, const int direction, struct uio * user_io_request)
{
    const int rc = check_uiomove_fault (site);
    if (rc != 0)
        return rc;

    return uiomove (address, byte_count, direction, user_io_request);
}

int usfs_kpin_initial (void)
{
#ifdef USFS_TESTING
    const int fault_error = usfs_test_fault (USFS_TEST_FAULT_PIN);
    if (fault_error != 0)
        return fault_error;
#endif

    const int rc = pincode ((int (*) ())usfs_kext_entry);
    if (rc == 0)
        is_module_pinned = true;

    return rc;
}

int usfs_kensure_pinned (void)
{
    if (is_module_pinned)
        return 0;

    const int rc = pincode ((int (*) ())usfs_kext_entry);
    if (rc == 0)
        is_module_pinned = true;

    return rc;
}

int usfs_kunpin (void)
{
#ifdef USFS_TESTING
    const int fault_error = usfs_test_fault (USFS_TEST_FAULT_UNPIN);
    if (fault_error != 0)
        return fault_error;

    usfs_test_shutdown ();
#endif

    const int rc = unpincode ((int (*) ())usfs_kext_entry);
    if (rc == 0)
        is_module_pinned = false;

    return rc;
}

int usfs_kunpin_deferred (void)
{
#ifdef USFS_TESTING
    usfs_test_shutdown ();
#endif

    const int rc = unpincode ((int (*) ())usfs_kext_entry);
    if (rc == 0)
        is_module_pinned = false;

    return rc;
}

int usfs_kcoverage_drain_fault (void)
{
#ifdef USFS_TESTING
    return usfs_test_fault (USFS_TEST_FAULT_UNPIN);
#else
    return 0;
#endif
}

int usfs_kgfs_register (void)
{
#ifdef USFS_TESTING
    const int rc = usfs_test_fault (USFS_TEST_FAULT_GFS_ADD);
    if (rc != 0)
        return rc;
#endif
    return register_fs_implementation ();
}

int usfs_kgfsdel (const int filesystem_type)
{
#ifdef USFS_TESTING
    const int rc = usfs_test_fault (USFS_TEST_FAULT_GFS_DEL);
    if (rc != 0)
        return rc;
#endif
    return gfsdel (filesystem_type);
}

int usfs_kdevswadd (const dev_t device_number, struct devsw * device_switch)
{
#ifdef USFS_TESTING
    const int rc = usfs_test_fault (USFS_TEST_FAULT_DEVSW_ADD);
    if (rc != 0)
        return rc;
#endif
    return devswadd (device_number, device_switch);
}

int usfs_kdevswdel (const dev_t device_number)
{
#ifdef USFS_TESTING
    const int rc = usfs_test_fault (USFS_TEST_FAULT_DEVSW_DEL);
    if (rc != 0)
        return rc;
#endif
    return devswdel (device_number);
}

int usfs_kcoverage_initialize (void)
{
#ifdef USFS_COVERAGE
    #ifdef USFS_TESTING
    const int rc = usfs_test_fault (USFS_TEST_FAULT_COVERAGE_INIT);
    if (rc != 0)
        return rc;
    #endif
    return usfs_gcov_runtime_initialize (usfs_gcov_expected_units ());
#else
    return 0;
#endif
}
