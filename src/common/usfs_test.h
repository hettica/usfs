// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_TEST_H
#define USFS_TEST_H

#ifdef _KERNEL
    #include <sys/inttypes.h>
#else
    #include <stdint.h>
#endif

#define USFS_TEST_ABI_VERSION         8u
#define USFS_TEST_IOC_INFO            0x55530200
#define USFS_TEST_IOC_ARM             0x55530201
#define USFS_TEST_IOC_STATUS          0x55530202
#define USFS_TEST_IOC_RESET           0x55530203
#define USFS_TEST_IOC_SCHEDULE_ARM    0x55530204
#define USFS_TEST_IOC_SCHEDULE_STATUS 0x55530205
#define USFS_TEST_IOC_NODE_STATUS     0x55530206
#define USFS_TEST_IOC_SELFTEST        0x55530207
#define USFS_TEST_IOC_EVICT_RELEASE   0x55530208
#define USFS_TEST_IOC_FID_PROBE       0x55530209

#define USFS_TEST_FID_PATH_MAX   1024u
#define USFS_TEST_FID_RAW_BYTES  24u
#define USFS_TEST_FSID_RAW_BYTES 16u

enum usfs_test_fid_action
{
    USFS_TEST_FID_CAPTURE = 1,
    USFS_TEST_FID_RESOLVE = 2
};

#define USFS_TEST_INFO_SELFTEST 0x00000002u

enum usfs_test_selftest_case
{
    USFS_TEST_SELFTEST_NONE = 0,
    USFS_TEST_SELFTEST_USERCOPY_FAULTS = 1,
    USFS_TEST_SELFTEST_UIOMOVE_IOVECS = 2,
    USFS_TEST_SELFTEST_TRACE_SCHEMA = 3,
    USFS_TEST_SELFTEST_MAX = USFS_TEST_SELFTEST_TRACE_SCHEMA
};

enum usfs_test_fault
{
    USFS_TEST_FAULT_NONE = 0,
    USFS_TEST_FAULT_PIN = 1,
    USFS_TEST_FAULT_COVERAGE_INIT = 2,
    USFS_TEST_FAULT_GFS_ADD = 3,
    USFS_TEST_FAULT_DEVSW_ADD = 4,
    USFS_TEST_FAULT_DEVSW_DEL = 5,
    USFS_TEST_FAULT_GFS_DEL = 6,
    USFS_TEST_FAULT_UNPIN = 7,
    USFS_TEST_FAULT_REQUEST_ALLOC = 8,
    USFS_TEST_FAULT_REQUEST_MESSAGE_ALLOC = 9,
    USFS_TEST_FAULT_REQUEST_REPLY_ALLOC = 10,
    USFS_TEST_FAULT_GNODE_ALLOC = 11,
    USFS_TEST_FAULT_NODE_ALLOC = 12,
    USFS_TEST_FAULT_CONNECTION_ALLOC = 13,
    USFS_TEST_FAULT_REPLY_COPY = 14,
    USFS_TEST_FAULT_WRITE_COPY = 15,
    USFS_TEST_FAULT_OPEN_STATE_ALLOC = 16,
    USFS_TEST_FAULT_REQUEST_WAIT_ALLOC = 17,
    USFS_TEST_FAULT_INIT_UNWIND = 18,
    USFS_TEST_FAULT_MAX = USFS_TEST_FAULT_INIT_UNWIND
};

enum usfs_test_schedule
{
    USFS_TEST_SCHEDULE_NONE = 0,
    USFS_TEST_SCHEDULE_MOUNT_TERM = 1,
    USFS_TEST_SCHEDULE_CHANNEL_TERM = 2,
    USFS_TEST_SCHEDULE_LOOKUP_IDENTITY = 3,
    USFS_TEST_SCHEDULE_CACHE_EVICT = 4,
    USFS_TEST_SCHEDULE_CHANNEL_DEALLOC_TERM = 5,
    USFS_TEST_SCHEDULE_VGET_FORCE_UNMOUNT = 6,
    USFS_TEST_SCHEDULE_MAX = USFS_TEST_SCHEDULE_VGET_FORCE_UNMOUNT
};

enum usfs_test_schedule_point
{
    USFS_TEST_POINT_MOUNT_RESERVED = 0x00000001u,
    USFS_TEST_POINT_TERM_BEFORE_CLOSE = 0x00000002u,
    USFS_TEST_POINT_TERM_BUSY_REOPENED = 0x00000004u,
    USFS_TEST_POINT_CHANNEL_PRE_ADMISSION = 0x00000008u,
    USFS_TEST_POINT_TERM_GATE_CLOSED = 0x00000010u,
    USFS_TEST_POINT_CHANNEL_REJECTED = 0x00000020u,
    USFS_TEST_POINT_LOOKUP_POST_REPLY = 0x00000040u,
    USFS_TEST_POINT_LOOKUP_RESOLVED = 0x00000080u,
    USFS_TEST_POINT_CACHE_BEFORE_EVICT = 0x00000100u,
    USFS_TEST_POINT_CACHE_EVICT_RELEASED = 0x00000200u,
    USFS_TEST_POINT_CACHE_AFTER_WRITE = 0x00000400u,
    USFS_TEST_POINT_CACHE_EVICT_DONE = 0x00000800u,
    USFS_TEST_POINT_CHANNEL_UNPUBLISHED = 0x00001000u,
    USFS_TEST_POINT_VGET_RESERVED = 0x00002000u,
    USFS_TEST_POINT_FORCE_UNMOUNT_STALE = 0x00004000u
};

enum usfs_test_node_reuse
{
    USFS_TEST_NODE_REUSE_LOOKUP = 1,
    USFS_TEST_NODE_REUSE_PARENT = 2,
    USFS_TEST_NODE_REUSE_CREATE = 3
};

struct usfs_test_info
{
    uint32_t abi_version;
    uint32_t fault_max;
    uint32_t flags;
    uint32_t schedule_max;
};

struct usfs_test_arm
{
    uint32_t abi_version;
    uint32_t fault;
    int32_t error;
    uint32_t occurrence;
};

struct usfs_test_status
{
    uint32_t abi_version;
    uint32_t fault;
    int32_t error;
    uint32_t occurrence;
    uint32_t observed;
    uint32_t fired;
    uint32_t armed;
    uint32_t reserved;
};

struct usfs_test_schedule_arm
{
    uint32_t abi_version;
    uint32_t schedule;
    uint32_t reserved[2];
};

struct usfs_test_schedule_status
{
    uint32_t abi_version;
    uint32_t schedule;
    uint32_t reached;
    uint32_t channel_pre_count;
    uint32_t channel_rejected_count;
    uint32_t timed_out;
    uint32_t active;
    uint32_t lookup_post_count;
    uint32_t lookup_resolved_count;
    uint32_t reserved;
};

struct usfs_test_node_status
{
    uint32_t abi_version;
    uint32_t reserved;
    uint64_t created;
    uint64_t reclaimed;
    uint64_t live;
    uint64_t peak_live;
    uint64_t lookup_reused;
    uint64_t parent_reused;
    uint64_t create_reused;
    uint64_t parent_rebuilt;
    uint64_t hold_calls;
    uint64_t release_calls;
    uint64_t root_created;
    uint64_t root_reclaimed;
    uint64_t duplicate_live;
    uint64_t accounting_errors;
};

struct usfs_test_selftest
{
    uint32_t abi_version;
    uint32_t case_id;
    uint32_t passed;
    int32_t observed;
    uint32_t reserved[4];
};

struct usfs_test_fid_probe
{
    uint32_t abi_version;                                   // USFS_TEST_ABI_VERSION for the test-only ioctl.
    uint32_t action;                                        // Capture from path or resolve saved bytes.
    char path[USFS_TEST_FID_PATH_MAX];                      // Absolute object path for capture, mount path for resolve.
    char expected_path[USFS_TEST_FID_PATH_MAX];             // Optional current object path for canonical-vnode comparison.
    unsigned char file_id[USFS_TEST_FID_RAW_BYTES];         // Full native AIX fileid including cleared padding.
    unsigned char file_system_id[USFS_TEST_FSID_RAW_BYTES]; // Native AIX fsid captured with the FID.
    uint32_t vnode_type;                                    // Expected or observed AIX vnode type.
    uint32_t reserved;                                      // Must be zero.
    uint64_t inode;                                         // Expected or observed backend inode.
};

#endif
