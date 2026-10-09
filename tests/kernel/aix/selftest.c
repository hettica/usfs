/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "definitions.h"
#include "instrumentation/testing/runtime.h"

static int usfs_test_usercopy_faults (struct usfs_test_selftest * result)
{
    volatile uintptr_t invalid_value = UINT64_MAX;
    caddr_t invalid = (caddr_t)invalid_value;
    unsigned char byte = 0;
    int observed = 0;

    if (copyin (invalid, (caddr_t)&byte, sizeof (byte)) != 0)
        observed |= 1;
    if (copyout ((caddr_t)&byte, invalid, sizeof (byte)) != 0)
        observed |= 2;
    result->observed = observed;
    result->passed = observed == 3;
    return 0;
}

static void usfs_test_init_uio (struct uio * uiop, struct iovec iovecs[2], caddr_t first, size_t first_length, caddr_t second, size_t second_length)
{
    memset (uiop, 0, sizeof (*uiop));
    memset (iovecs, 0, 2 * sizeof (*iovecs));
    iovecs[0].iov_base = first;
    iovecs[0].iov_len = first_length;
    iovecs[1].iov_base = second;
    iovecs[1].iov_len = second_length;
    uiop->uio_iov = iovecs;
    uiop->uio_iovcnt = 2;
    uiop->uio_offset = 10;
    uiop->uio_resid = (int32long64_t)(first_length + second_length);
    uiop->uio_segflg = UIO_SYSSPACE;
}

static int usfs_test_uiomove_iovecs (struct usfs_test_selftest * result)
{
    static const char input[] = "abcde";
    char read_first[2] = { 0, 0 };
    char read_second[3] = { 0, 0, 0 };
    char write_first[2] = { 'v', 'w' };
    char write_second[3] = { 'x', 'y', 'z' };
    char output[5] = { 0, 0, 0, 0, 0 };
    struct iovec iovecs[2];
    struct uio uio;
    int observed = 0;

    usfs_test_init_uio (&uio, iovecs, read_first, sizeof (read_first), read_second, sizeof (read_second));
    if (uiomove ((caddr_t)input, 5, UIO_READ, &uio) == 0)
    {
        if (memcmp (read_first, "ab", 2) == 0 &&
            memcmp (read_second, "cde", 3) == 0)
            observed |= 1;
        if (uio.uio_resid == 0)
            observed |= 2;
        if (uio.uio_offset == 15)
            observed |= 4;
        /* AIX leaves the exhausted final iovec selected. */
        if (uio.uio_iovcnt == 1)
            observed |= 8;
        if (uio.uio_iovdcnt == 1)
            observed |= 16;
    }

    usfs_test_init_uio (&uio, iovecs, write_first, sizeof (write_first), write_second, sizeof (write_second));
    if (uiomove ((caddr_t)output, 5, UIO_WRITE, &uio) == 0)
    {
        if (memcmp (output, "vwxyz", 5) == 0)
            observed |= 32;
        if (uio.uio_resid == 0)
            observed |= 64;
        if (uio.uio_offset == 15)
            observed |= 128;
        if (uio.uio_iovcnt == 1)
            observed |= 256;
        if (uio.uio_iovdcnt == 1)
            observed |= 512;
    }

    result->observed = observed;
    result->passed = observed == 1023;
    return 0;
}

static int usfs_test_trace_schema (struct usfs_test_selftest * result)
{
    USFS_TRACE_CONTROL5(
        USFS_TRACE_SCHEMA,
        USFS_TRACE_ABI_VERSION,
        USFS_PROTOCOL_VERSION,
        64,
        gfs.gfs_type,
        1
    );
    USFS_TRACE_CONTROL5(
        USFS_TRACE_KEXT_STATE,
        USFS_TRACE_KEXT_ACTIVE,
        USFS_TRACE_KEXT_ACTIVE,
        0,
        0,
        1
    );
    USFS_TRACE_CONTROL5(
        USFS_TRACE_REGISTRATION,
        USFS_TRACE_REG_GFS,
        USFS_TRACE_ACTION_ADD,
        0,
        USFS_TRACE_KEXT_ACTIVE,
        1
    );
    USFS_TRACE_CONTROL5(
        USFS_TRACE_CONNECTION,
        USFS_TRACE_ACTION_READY,
        7,
        USFS_TRACE_CONN_ACTIVE,
        2,
        0
    );
    USFS_TRACE_CONTROL5(
        USFS_TRACE_MOUNT,
        USFS_TRACE_ACTION_PUBLISH,
        7,
        USFS_TRACE_MOUNT_ACTIVE,
        1,
        0
    );
    USFS_TRACE_CONTROL5(
        USFS_TRACE_RUNTIME_CONFIG,
        30000,
        5000,
        128,
        7,
        0
    );
    USFS_TRACE_CONTROL5(
        USFS_TRACE_FAULT,
        USFS_TRACE_FAULT_TIMEOUT,
        ETIMEDOUT,
        7,
        USFS_TRACE_OP_LOOKUP,
        42
    );

    USFS_TRACE_REQUEST5(
        USFS_TRACE_REQUEST_REJECTED,
        USFS_TRACE_OP_LOOKUP,
        2,
        USFS_TRACE_CLASS_NORMAL,
        EAGAIN,
        128
    );
    USFS_TRACE_REQUEST5(
        USFS_TRACE_REQUEST_QUEUED,
        42,
        USFS_TRACE_OP_LOOKUP,
        2,
        USFS_TRACE_CLASS_NORMAL,
        1
    );
    USFS_TRACE_REQUEST5(
        USFS_TRACE_REQUEST_DELIVERED,
        42,
        USFS_TRACE_OP_LOOKUP,
        7,
        64,
        0
    );
    USFS_TRACE_REQUEST5(
        USFS_TRACE_REPLY_ACCEPTED,
        42,
        USFS_TRACE_OP_LOOKUP,
        7,
        0,
        32
    );
    USFS_TRACE_REQUEST5(
        USFS_TRACE_REQUEST_COMPLETED,
        42,
        USFS_TRACE_OP_LOOKUP,
        USFS_TRACE_OUTCOME_SUCCESS,
        0,
        32
    );
    USFS_TRACE_REQUEST5(
        USFS_TRACE_CONNECTION_ABORT,
        7,
        USFS_TRACE_CONN_ACTIVE,
        USFS_TRACE_CONN_DEAD,
        1,
        1
    );

    USFS_TRACE_FS5(
        USFS_TRACE_VFS_ENTRY,
        USFS_TRACE_VFS_ROOT,
        7,
        1,
        0,
        0
    );
    USFS_TRACE_FS5(
        USFS_TRACE_VFS_RESULT,
        USFS_TRACE_VFS_ROOT,
        7,
        1,
        0,
        2
    );
    USFS_TRACE_FS5(
        USFS_TRACE_VNODE_ENTRY,
        USFS_TRACE_OP_LOOKUP,
        7,
        2,
        42,
        USFS_TRACE_CLASS_NORMAL
    );
    USFS_TRACE_FS5(
        USFS_TRACE_VNODE_RESULT,
        USFS_TRACE_OP_LOOKUP,
        7,
        2,
        42,
        0
    );
    USFS_TRACE_FS5(
        USFS_TRACE_PAGER_ENTRY,
        7,
        2,
        4096,
        4096,
        0
    );
    USFS_TRACE_FS5(
        USFS_TRACE_PAGER_RESULT,
        7,
        2,
        4096,
        4096,
        0
    );

    result->observed = 19;
    result->passed = 1;
    return 0;
}

int usfs_test_kernel_selftest (uint32_t case_id, struct usfs_test_selftest * result)
{
    result->abi_version = USFS_TEST_ABI_VERSION;
    result->case_id = case_id;
    result->passed = 0;
    result->observed = 0;
    memset (result->reserved, 0, sizeof (result->reserved));

    if (case_id == USFS_TEST_SELFTEST_USERCOPY_FAULTS)
        return usfs_test_usercopy_faults (result);
    if (case_id == USFS_TEST_SELFTEST_UIOMOVE_IOVECS)
        return usfs_test_uiomove_iovecs (result);
    if (case_id == USFS_TEST_SELFTEST_TRACE_SCHEMA)
        return usfs_test_trace_schema (result);
    return EINVAL;
}
