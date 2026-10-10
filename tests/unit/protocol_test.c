/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "usfs_config.h"
#include "usfs_proto.h"
#include "usfs_validate.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

static void test_layout(struct tap_state *state)
{
    struct usfs_forget_in forget = { 1 };
    TAP_EQ_SIZE(state, sizeof(forget), 8, "FORGET count preserves the fixed wire layout");
    tap_ok(state, !usfs_forget_in_valid(NULL, 1), "FORGET rejects a missing body");
    tap_ok(state, usfs_forget_in_valid(&forget, 1), "FORGET accepts exact ownership release");
    tap_ok(state, !usfs_forget_in_valid(&forget, 0), "FORGET rejects unowned references");
    forget.count = 0;
    tap_ok(state, !usfs_forget_in_valid(&forget, 1), "FORGET rejects zero count");
    forget.count = UINT64_MAX;
    tap_ok(state, usfs_forget_in_valid(&forget, UINT64_MAX) &&
           !usfs_forget_in_valid(&forget, UINT64_MAX - 1), "FORGET validates full-width counts without wrapping");
    TAP_EQ_SIZE(state, sizeof(struct usfs_dev_cfg), 32,
                "configuration DDS size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_dev_cfg, instrumentation), 16,
                "configuration DDS instrumentation offset");
    TAP_EQ_SIZE(state, offsetof(struct usfs_dev_cfg, reserved), 28,
                "configuration DDS final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_config_coverage_export), 32,
                "configuration coverage export size");
    TAP_EQ_SIZE(state,
                offsetof(struct usfs_config_coverage_export, reserved), 24,
                "configuration coverage export final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_kext_lifecycle_query), 24,
                "configuration lifecycle query size");
    TAP_EQ_SIZE(state,
                offsetof(struct usfs_kext_lifecycle_query, reserved), 16,
                "configuration lifecycle query final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_kext_lifecycle_state), 24,
                "configuration lifecycle state size");
    TAP_EQ_SIZE(state,
                offsetof(struct usfs_kext_lifecycle_state, reserved), 12,
                "configuration lifecycle state final field offset");
    TAP_EQ_SIZE(state, sizeof(struct kext_runtime_config), 48,
                "runtime configuration size");
    TAP_EQ_SIZE(state, offsetof(struct kext_runtime_config, set_mask), 8,
                "runtime configuration mask offset");
    TAP_EQ_SIZE(state, offsetof(struct kext_runtime_config, reserved), 36,
                "runtime configuration final field offset");
    TAP_EQ_SIZE(state, sizeof(struct kext_state), 584,
                "status v1 size");
    TAP_EQ_SIZE(state, offsetof(struct kext_state, accepted_requests), 72,
                "status counter offset");
    TAP_EQ_SIZE(state, offsetof(struct kext_state, normal_latency), 152,
                "status latency offset");
    TAP_EQ_SIZE(state, offsetof(struct kext_state, last_fault), 520,
                "status last-fault offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_in_hdr), 40, "in header size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_in_hdr, pad), 36, "in header final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_open_in), 8, "open request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_open_in, isdir), 4, "open request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_getattr_in), 8, "getattr request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_getattr_in, fh), 0, "getattr request field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_read_in), 24, "read request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_read_in, pad), 20, "read request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_release_in), 16, "release request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_release_in, isdir), 12, "release request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_readdir_in), 24, "readdir request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_readdir_in, pad), 20, "readdir request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_vget_in), 8, "vget request size");
    TAP_EQ_SIZE(state, sizeof(struct usfs_create_in), 8, "create request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_create_in, mode), 4, "create request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_mkdir_in), 8, "mkdir request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_mkdir_in, pad), 4, "mkdir request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_rename_in), 16, "rename request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_rename_in, pad), 12, "rename request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_symlink_in), 8, "symlink request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_symlink_in, pad), 4, "symlink request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_link_in), 8, "link request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_link_in, newparent), 0, "link request field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_setattr_in), 72, "setattr request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_setattr_in, pad), 68, "setattr request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_write_in), 24, "write request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_write_in, flags), 20, "write request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_flush_in), 16, "flush request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_flush_in, pad), 12,
                "flush request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_fsync_in), 32, "fsync request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_fsync_in, pad), 28,
                "fsync request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_syncfs_in), 8, "syncfs request size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_syncfs_in, pad), 4,
                "syncfs request final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_reply_header), 24, "out header size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_reply_header, pad), 20, "out header final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_attr), 88, "attribute size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_attr, pad), 84, "attribute final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_entry_out), 96, "entry reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_entry_out, attr), 8, "entry reply attribute offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_fid_out), 8, "fid reply size");
    TAP_EQ_SIZE(state, sizeof(struct usfs_vget_out), 104, "vget reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_vget_out, entry), 8, "vget reply entry offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_attr_out), 96, "attribute reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_attr_out, parent), 88, "attribute reply final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_open_out), 16, "open reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_open_out, pad), 12, "open reply final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_create_out), 104, "create reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_create_out, fh), 96, "create reply final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_write_out), 16, "write reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_write_out, offset), 8, "write reply final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_statfs_out), 48, "statfs reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_statfs_out, namemax), 44, "statfs reply final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_readdir_out), 16, "readdir reply size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_readdir_out, pad), 12, "readdir reply final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_dirent), 16, "directory entry header size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_dirent, reclen), 14, "directory entry final field offset");
    TAP_EQ_SIZE(state, sizeof(struct usfs_dev_info), 24, "device info size");
    TAP_EQ_SIZE(state, offsetof(struct usfs_dev_info, cookie), 16, "device info final field offset");
}

static void test_status(struct tap_state *state)
{
    struct kext_state status;
    struct usfs_latency_stats latency;
    unsigned index;

    memset(&status, 0, sizeof(status));
    status.magic = USFS_STATUS_MAGIC;
    status.abi_version = USFS_STATUS_ABI_VERSION;
    status.size = sizeof(status);
    status.health = USFS_HEALTH_HEALTHY;
    tap_ok(state, usfs_status_validate(&status) == 0,
           "valid status snapshot accepted");
    status.magic = 0;
    tap_ok(state, usfs_status_validate(&status) != 0,
           "status magic validated");
    status.magic = USFS_STATUS_MAGIC;
    status.abi_version += 1u;
    tap_ok(state, usfs_status_validate(&status) != 0,
           "status ABI version validated");
    status.abi_version = USFS_STATUS_ABI_VERSION;
    status.reserved[2] = 1;
    tap_ok(state, usfs_status_validate(&status) != 0,
           "status reserved fields validated");

    memset(&latency, 0, sizeof(latency));
    tap_ok(state, usfs_status_percentile_ms(&latency, 50u) == 0,
           "empty latency has no percentile");
    for (index = 0; index < USFS_STATUS_LATENCY_BUCKETS; ++index)
    {
        memset(&latency, 0, sizeof(latency));
        latency.samples = 1;
        latency.buckets[index] = 1;
        tap_ok(state,
               usfs_status_percentile_ms(&latency, 50u) ==
                   usfs_status_latency_bounds_ms[index],
               "latency histogram boundary selected");
    }
    memset(&latency, 0, sizeof(latency));
    latency.samples = 10;
    latency.buckets[0] = 4;
    latency.buckets[1] = 6;
    tap_ok(state, usfs_status_percentile_ms(&latency, 50u) == 2u,
           "latency percentile uses nearest-rank selection");
    memset(&latency, 0, sizeof(latency));
    latency.samples = 1;
    latency.buckets[USFS_STATUS_LATENCY_BUCKETS - 1u] = 1;
    tap_ok(state, usfs_status_percentile_ms(&latency, 95u) == UINT32_MAX,
           "latency overflow bucket preserved");
    latency.samples = UINT64_MAX;
    latency.buckets[USFS_STATUS_LATENCY_BUCKETS - 1u] = UINT64_MAX;
    tap_ok(state, usfs_status_percentile_ms(&latency, 95u) == UINT32_MAX,
           "latency percentile tolerates a saturated sample counter");
    tap_ok(state, usfs_status_percentile_ms(&latency, 0u) == 0,
           "zero percentile rejected");
    tap_ok(state, usfs_status_percentile_ms(&latency, 101u) == 0,
           "out-of-range percentile rejected");
}

static void test_arithmetic(struct tap_state *state)
{
    uint32_t u32 = 0;
    uint64_t u64 = 0;

    tap_ok(state, usfs_u32_add_checked(0, 0, &u32) && u32 == 0,
           "u32 zero addition");
    tap_ok(state, usfs_u32_add_checked(UINT32_MAX - 1u, 1u, &u32) &&
                  u32 == UINT32_MAX, "u32 maximum addition");
    tap_ok(state, !usfs_u32_add_checked(UINT32_MAX, 1u, &u32),
           "u32 overflow rejected");
    tap_ok(state, !usfs_u32_add_checked(1u, 1u, 0),
           "u32 null result rejected");
    tap_ok(state, usfs_u64_add_checked(UINT64_MAX - 1u, 1u, &u64) &&
                  u64 == UINT64_MAX, "u64 maximum addition");
    tap_ok(state, !usfs_u64_add_checked(UINT64_MAX, 1u, &u64),
           "u64 overflow rejected");
    tap_ok(state, usfs_u32_sub_checked(UINT32_MAX, UINT32_MAX, &u32) &&
                  u32 == 0, "u32 subtraction boundary");
    tap_ok(state, !usfs_u32_sub_checked(0, 1, &u32),
           "u32 underflow rejected");
    tap_ok(state, usfs_u64_sub_checked(UINT64_MAX, 1, &u64) &&
                  u64 == UINT64_MAX - 1, "u64 subtraction boundary");
    tap_ok(state, !usfs_u64_sub_checked(0, 1, &u64),
           "u64 underflow rejected");
    tap_ok(state, usfs_size_to_u32_checked((size_t)UINT32_MAX, &u32) &&
                  u32 == UINT32_MAX, "maximum size converts to u32");
#if SIZE_MAX > UINT32_MAX
    tap_ok(state, !usfs_size_to_u32_checked((size_t)UINT32_MAX + 1u, &u32),
           "oversized size conversion rejected");
#else
    tap_ok(state, 1, "oversized size conversion rejected # SKIP 32-bit size_t");
#endif
    tap_ok(state, usfs_align8_checked(0, &u32) && u32 == 0,
           "align zero");
    tap_ok(state, usfs_align8_checked(1, &u32) && u32 == 8,
           "align one");
    tap_ok(state, usfs_align8_checked(8, &u32) && u32 == 8,
           "align aligned value");
    tap_ok(state, !usfs_align8_checked(UINT32_MAX, &u32),
           "align overflow rejected");
}

static void test_mount_metadata(struct tap_state *state)
{
    uint64_t value = 0;
    static const char valid[] = "fd=7,chan=3,cookie=abcdef,rw=1";
    static const char overflow[] = "cookie=10000000000000000";
    static const char junk[] = "rw=1junk";
    static const char unterminated[] = { 'r', 'w', '=', '1' };

    tap_ok(state, usfs_bounded_region_valid(128u, 64, 16, 48u),
           "bounded metadata region accepted");
    tap_ok(state, !usfs_bounded_region_valid(128u, -1, 16, 48u),
           "negative metadata offset rejected");
    tap_ok(state, !usfs_bounded_region_valid(128u, 64, -1, 48u),
           "negative metadata length rejected");
    tap_ok(state, !usfs_bounded_region_valid(128u, 32, 16, 48u),
           "metadata before fixed header rejected");
    tap_ok(state, !usfs_bounded_region_valid(128u, 120, 16, 48u),
           "metadata range beyond record rejected");
    tap_ok(state, usfs_info_field_u64(valid, (uint32_t)sizeof(valid),
                                      "chan", 4u, 10u, &value) == 1 &&
                  value == 3u, "decimal mount field parsed");
    tap_ok(state, usfs_info_field_u64(valid, (uint32_t)sizeof(valid),
                                      "cookie", 6u, 16u, &value) == 1 &&
                  value == 0xabcdefu, "hex mount field parsed");
    tap_ok(state, usfs_info_field_u64(valid, (uint32_t)sizeof(valid),
                                      "missing", 7u, 10u, &value) == 0,
           "absent mount field distinguished");
    tap_ok(state, usfs_info_field_u64(overflow,
                                      (uint32_t)sizeof(overflow),
                                      "cookie", 6u, 16u, &value) == -1,
           "overflowing mount field rejected");
    tap_ok(state, usfs_info_field_u64(junk, (uint32_t)sizeof(junk),
                                      "rw", 2u, 10u, &value) == -1,
           "mount field trailing junk rejected");
    tap_ok(state, usfs_info_field_u64(unterminated,
                                      (uint32_t)sizeof(unterminated),
                                      "rw", 2u, 10u, &value) == -1,
           "unterminated mount metadata rejected");
    tap_ok(state, usfs_info_field_u64(valid, (uint32_t)sizeof(valid),
                                      "rw", 2u, 8u, &value) == -1,
           "unsupported mount numeric base rejected");
}

static void test_request_sizes(struct tap_state *state)
{
    uint64_t length = calculate_request_buffer_size(0, 0, 0);

    tap_ok(state, length == sizeof(struct usfs_in_hdr),
           "empty request size calculated");
    tap_ok(state, usfs_request_sizes_valid(0, 0, 0, length),
           "empty request sizes accepted");
    length = calculate_request_buffer_size(64, USFS_MAX_NAME, USFS_MAX_DATA);
    tap_ok(state, length == USFS_MSG_MAX &&
                  usfs_request_sizes_valid(USFS_MAX_NAME, USFS_MAX_DATA,
                                           USFS_MSG_MAX, length),
           "maximum request size accepted");
    length = calculate_request_buffer_size(0, USFS_MAX_NAME + 1u, 0);
    tap_ok(state, !usfs_request_sizes_valid(USFS_MAX_NAME + 1u, 0, 0,
                                            length),
           "oversized request name rejected");
    length = calculate_request_buffer_size(0, 0, USFS_MAX_DATA + 1u);
    tap_ok(state, !usfs_request_sizes_valid(0, USFS_MAX_DATA + 1u, 0,
                                            length),
           "oversized request data rejected");
    length = calculate_request_buffer_size(0, 0, 0);
    tap_ok(state, !usfs_request_sizes_valid(0, 0, USFS_MSG_MAX + 1u,
                                            length),
           "oversized reply capacity rejected");
    length = calculate_request_buffer_size(65, USFS_MAX_NAME, USFS_MAX_DATA);
    tap_ok(state, !usfs_request_sizes_valid(USFS_MAX_NAME, USFS_MAX_DATA,
                                             0, length),
           "request total beyond message limit rejected");

    length = calculate_request_buffer_size(UINT32_MAX, 0, 0);
    tap_ok(state, length > UINT32_MAX && !usfs_request_sizes_valid(0, 0, 0, length),
           "request body cannot wrap the allocated message length");
}

static void test_scalars(struct tap_state *state)
{
    tap_ok(state, !usfs_opcode_valid(0), "opcode zero rejected");
    tap_ok(state, usfs_opcode_valid(USFS_OP_LOOKUP), "first opcode accepted");
    tap_ok(state, usfs_opcode_valid(USFS_OP_VGET), "last opcode accepted");
    tap_ok(state, !usfs_opcode_valid(USFS_OP_VGET + 1), "unknown opcode rejected");
    tap_ok(state, !usfs_reply_errno_valid(-1), "negative errno rejected");
    tap_ok(state, usfs_reply_errno_valid(0), "success errno accepted");
    tap_ok(state, usfs_reply_errno_valid(USFS_AIX_ERRNO_MAX), "maximum AIX errno accepted");
    tap_ok(state, !usfs_reply_errno_valid(USFS_AIX_ERRNO_MAX + 1),
           "unknown AIX errno rejected");
    tap_ok(state, usfs_nsec_valid(999999999u), "maximum nanoseconds accepted");
    tap_ok(state, !usfs_nsec_valid(1000000000u), "nanoseconds overflow rejected");
    tap_ok(state, !usfs_nodeid_valid(0), "zero node id rejected");
    tap_ok(state, usfs_nodeid_valid(UINT64_MAX), "maximum node id accepted");
    tap_ok(state, usfs_data_size_valid(0), "zero data size accepted");
    tap_ok(state, usfs_data_size_valid(USFS_MAX_DATA), "maximum data size accepted");
    tap_ok(state, !usfs_data_size_valid(USFS_MAX_DATA + 1u), "oversized data rejected");
    tap_ok(state, usfs_offset_count_valid(UINT64_MAX, 0), "maximum zero-length offset accepted");
    tap_ok(state, !usfs_offset_count_valid(UINT64_MAX, 1), "offset plus count overflow rejected");
    tap_ok(state, usfs_file_offset_count_valid(INT64_MAX, 0),
           "maximum signed file offset accepted");
    tap_ok(state, !usfs_file_offset_count_valid(-1, 1),
           "negative file offset rejected");
    tap_ok(state, !usfs_file_offset_count_valid(INT64_MAX, 1),
           "signed file offset overflow rejected");
    tap_ok(state, usfs_write_flags_valid(USFS_WRITE_APPEND), "known write flag accepted");
    tap_ok(state, !usfs_write_flags_valid(USFS_WRITE_APPEND << 1), "unknown write flag rejected");
}

static void test_durability(struct tap_state *state)
{
    struct usfs_fsync_in fsync_request;
    struct usfs_syncfs_in syncfs_request;

    memset(&fsync_request, 0, sizeof(fsync_request));
    tap_ok(state, usfs_fsync_in_valid(&fsync_request),
           "whole-file fsync request accepted");
    fsync_request.offset = 1;
    tap_ok(state, !usfs_fsync_in_valid(&fsync_request),
           "whole-file fsync rejects range fields");
    fsync_request.flags = USFS_FSYNC_RANGE | USFS_FSYNC_DATASYNC;
    fsync_request.offset = 7;
    fsync_request.length = 9;
    tap_ok(state, usfs_fsync_in_valid(&fsync_request),
           "data-only range fsync request accepted");
    fsync_request.offset = (uint64_t)INT64_MAX;
    fsync_request.length = 1;
    tap_ok(state, !usfs_fsync_in_valid(&fsync_request),
           "range fsync overflow rejected");
    fsync_request.offset = 0;
    fsync_request.length = 0;
    fsync_request.flags = 0x80000000u;
    tap_ok(state, !usfs_fsync_in_valid(&fsync_request),
           "unknown fsync flags rejected");
    fsync_request.flags = 0;
    fsync_request.pad = 1;
    tap_ok(state, !usfs_fsync_in_valid(&fsync_request),
           "fsync padding rejected");
    tap_ok(state, !usfs_fsync_in_valid(NULL),
           "null fsync request rejected");

    memset(&syncfs_request, 0, sizeof(syncfs_request));
    tap_ok(state, usfs_syncfs_in_valid(&syncfs_request) &&
                  (syncfs_request.mode = USFS_SYNCFS_FORCE,
                   usfs_syncfs_in_valid(&syncfs_request)) &&
                  (syncfs_request.mode = USFS_SYNCFS_QUIESCE,
                   usfs_syncfs_in_valid(&syncfs_request)),
           "all syncfs modes accepted");
    syncfs_request.mode = USFS_SYNCFS_QUIESCE + 1u;
    tap_ok(state, !usfs_syncfs_in_valid(&syncfs_request),
           "unknown syncfs mode rejected");
    syncfs_request.mode = USFS_SYNCFS_TRY;
    syncfs_request.pad = 1;
    tap_ok(state, !usfs_syncfs_in_valid(&syncfs_request),
           "syncfs padding rejected");
    tap_ok(state, !usfs_syncfs_in_valid(NULL),
           "null syncfs request rejected");
}

static void test_attributes(struct tap_state *state)
{
    struct usfs_setattr_in input;
    struct usfs_attr output;

    memset(&input, 0, sizeof(input));
    input.valid = USFS_SET_ATIME | USFS_SET_MTIME | USFS_SET_CTIME;
    input.atimensec = 999999999u;
    input.mtimensec = 999999999u;
    input.ctimensec = 999999999u;
    tap_ok(state, usfs_setattr_valid(&input), "valid setattr fields accepted");
    input.valid |= 0x80000000u;
    tap_ok(state, !usfs_setattr_valid(&input), "unknown setattr flag rejected");
    input.valid &= ~0x80000000u;
    input.atimensec = 1000000000u;
    tap_ok(state, !usfs_setattr_valid(&input), "invalid setattr nanoseconds rejected");

    memset(&output, 0, sizeof(output));
    output.ino = 1;
    output.nlink = 1;
    output.blksize = 4096;
    tap_ok(state, usfs_attr_valid(&output), "valid attributes accepted");
    output.ino = 0;
    tap_ok(state, !usfs_attr_valid(&output), "zero attribute inode rejected");
    output.ino = 1;
    output.nlink = 0;
    tap_ok(state, usfs_attr_valid(&output),
           "zero link count accepted for an unlinked open file");
    output.nlink = SHRT_MAX;
    output.blocks = LONG_MAX;
    tap_ok(state, usfs_attr_valid(&output), "largest representable AIX link and block counts accepted");
    output.nlink = (uint32_t)SHRT_MAX + 1u;
    tap_ok(state, !usfs_attr_valid(&output), "link count exceeding AIX vattr is rejected");
    output.nlink = 1;
    output.blocks = (uint64_t)LONG_MAX + 1u;
    tap_ok(state, !usfs_attr_valid(&output), "block count exceeding AIX vattr is rejected");
    output.blocks = 0;
    output.mtimensec = 1000000000u;
    tap_ok(state, !usfs_attr_valid(&output), "invalid attribute nanoseconds rejected");
    output.mtimensec = 0;
    output.size = (uint64_t)INT64_MAX + 1u;
    tap_ok(state, !usfs_attr_valid(&output),
           "attribute size beyond AIX offset range rejected");
}

static void test_golden_headers(struct tap_state *state)
{
    struct usfs_in_hdr input;
    struct usfs_reply_header output;
    static const unsigned char input_le[40] = {
        0x28,0,0,0, 10,0, 4,0, 8,7,6,5,4,3,2,1,
        0x18,0x17,0x16,0x15,0x14,0x13,0x12,0x11,
        0x24,0x23,0x22,0x21, 0x34,0x33,0x32,0x31,
        0x44,0x43,0x42,0x41, 0,0,0,0
    };
    static const unsigned char input_be[40] = {
        0,0,0,0x28, 0,10, 0,4, 1,2,3,4,5,6,7,8,
        0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x21,0x22,0x23,0x24, 0x31,0x32,0x33,0x34,
        0x41,0x42,0x43,0x44, 0,0,0,0
    };
    static const unsigned char output_le[24] = {
        0x18,0,0,0, 10,0, 4,0, 8,7,6,5,4,3,2,1,
        0x0d,0,0,0, 0,0,0,0
    };
    static const unsigned char output_be[24] = {
        0,0,0,0x18, 0,10, 0,4, 1,2,3,4,5,6,7,8,
        0,0,0,0x0d, 0,0,0,0
    };
    const uint16_t endian = 1;
    int little = *(const unsigned char *)&endian == 1;

    memset(&input, 0, sizeof(input));
    input.len = 40;
    input.version = USFS_PROTOCOL_VERSION;
    input.opcode = 4;
    input.unique = UINT64_C(0x0102030405060708);
    input.nodeid = UINT64_C(0x1112131415161718);
    input.uid = UINT32_C(0x21222324);
    input.gid = UINT32_C(0x31323334);
    input.pid = UINT32_C(0x41424344);
    tap_ok(state, memcmp(&input, little ? input_le : input_be, sizeof(input)) == 0,
           "request header golden bytes");

    memset(&output, 0, sizeof(output));
    output.len = 24;
    output.version = USFS_PROTOCOL_VERSION;
    output.opcode = 4;
    output.id = UINT64_C(0x0102030405060708);
    output.error = 13;
    tap_ok(state, memcmp(&output, little ? output_le : output_be, sizeof(output)) == 0,
           "reply header golden bytes");
}

static void test_names(struct tap_state *state)
{
    const char good[] = "name";
    const char missing[] = {'b', 'a', 'd'};

    tap_ok(state, calculate_bounded_opcode_specific_argument_length(good, sizeof(good),
                                           USFS_MAX_NAME) == sizeof(good),
           "terminated name length calculated");
    tap_ok(state, calculate_bounded_opcode_specific_argument_length(missing, sizeof(missing),
                                           USFS_MAX_NAME) == 0,
           "unterminated name has no bounded length");
    tap_ok(state, calculate_bounded_opcode_specific_argument_length(good, 0, USFS_MAX_NAME) == 0,
           "zero name capacity has no bounded length");
    tap_ok(state, calculate_bounded_opcode_specific_argument_length(NULL, sizeof(good),
                                           USFS_MAX_NAME) == 0,
           "null name has no bounded length");
}

static void test_headers(struct tap_state *state)
{
    struct usfs_reply_header header;

    memset(&header, 0, sizeof(header));
    header.len = sizeof(header);
    header.version = USFS_PROTOCOL_VERSION;
    header.opcode = USFS_OP_GETATTR;
    tap_ok(state, usfs_out_header_valid(&header, sizeof(header)),
           "valid success header accepted");
    header.version++;
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header)),
           "wrong version rejected");
    header.version = USFS_PROTOCOL_VERSION;
    header.error = USFS_AIX_ERRNO_MAX + 1;
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header)),
           "invalid errno header rejected");
    header.error = 5;
    header.len++;
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header) + 1u),
           "error reply body rejected");
    header.error = 0;
    header.len = sizeof(header) - 1u;
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header)),
           "short header length rejected");
}

static void test_dirent(struct tap_state *state)
{
    unsigned char storage[32];
    struct usfs_dirent *entry = (struct usfs_dirent *)storage;
    char *name = (char *)(entry + 1);

    memset(storage, 0, sizeof(storage));
    entry->ino = 2;
    entry->namelen = 3;
    entry->reclen = 24;
    memcpy(name, "abc", 4);
    tap_ok(state, usfs_dirent_valid(entry, sizeof(storage)),
           "valid directory entry accepted");
    entry->reclen = 23;
    tap_ok(state, !usfs_dirent_valid(entry, sizeof(storage)),
           "misaligned directory entry rejected");
    entry->reclen = 24;
    name[3] = 'x';
    tap_ok(state, !usfs_dirent_valid(entry, sizeof(storage)),
           "unterminated directory entry rejected");
    name[3] = '\0';
    entry->namelen = USFS_MAX_NAME;
    tap_ok(state, !usfs_dirent_valid(entry, sizeof(storage)),
           "oversized directory name rejected");
}

static void fill_valid_attr(struct usfs_attr *attr, uint64_t ino)
{
    memset(attr, 0, sizeof(*attr));
    attr->ino = ino;
    attr->nlink = 1;
    attr->blksize = 4096;
}

static void test_operation_replies(struct tap_state *state)
{
    struct usfs_entry_out entry;
    struct usfs_attr_out attr;
    struct usfs_create_out create;
    struct usfs_open_out open_reply;
    struct usfs_write_out write_reply;
    struct usfs_statfs_out statfs_reply;
    unsigned char directory[41];
    struct usfs_readdir_out *directory_header =
        (struct usfs_readdir_out *)directory;
    struct usfs_dirent *directory_entry =
        (struct usfs_dirent *)(directory + sizeof(*directory_header));
    char *directory_name = (char *)(directory_entry + 1);

    memset(&entry, 0, sizeof(entry));
    entry.nodeid = 2;
    fill_valid_attr(&entry.attr, 2);
    tap_ok(state, usfs_entry_out_valid(&entry), "valid entry reply accepted");
    entry.nodeid = 0;
    tap_ok(state, !usfs_entry_out_valid(&entry), "zero entry node rejected");
    entry.nodeid = 2;
    entry.attr.ino = 3;
    tap_ok(state, usfs_entry_out_valid(&entry),
           "distinct protocol node and filesystem inode accepted");
    entry.attr.ino = 2;
    entry.attr.atimensec = 1000000000u;
    tap_ok(state, !usfs_entry_out_valid(&entry),
           "invalid entry attributes rejected");

    memset(&attr, 0, sizeof(attr));
    fill_valid_attr(&attr.attr, 2);
    attr.parent = 1;
    tap_ok(state, usfs_attr_out_valid(&attr), "valid attribute reply accepted");
    attr.parent = 0;
    tap_ok(state, !usfs_attr_out_valid(&attr), "zero attribute parent rejected");

    memset(&create, 0, sizeof(create));
    create.nodeid = 5;
    fill_valid_attr(&create.attr, 5);
    tap_ok(state, usfs_create_out_valid(&create), "valid create reply accepted");
    create.nodeid = 0;
    tap_ok(state, !usfs_create_out_valid(&create), "zero create node rejected");

    memset(&open_reply, 0, sizeof(open_reply));
    tap_ok(state, usfs_open_out_valid(&open_reply), "valid open reply accepted");
    open_reply.pad = 1;
    tap_ok(state, !usfs_open_out_valid(&open_reply), "open padding rejected");

    memset(&write_reply, 0, sizeof(write_reply));
    write_reply.written = 4;
    write_reply.offset = 8;
    tap_ok(state, usfs_write_out_valid(&write_reply, 4),
           "valid write reply accepted");
    write_reply.written = 5;
    tap_ok(state, !usfs_write_out_valid(&write_reply, 4),
           "write count beyond request rejected");
    write_reply.written = 1;
    write_reply.offset = UINT64_MAX;
    tap_ok(state, !usfs_write_out_valid(&write_reply, 4),
           "write offset overflow rejected");
    write_reply.offset = (uint64_t)INT64_MAX;
    tap_ok(state, !usfs_write_out_valid(&write_reply, 4),
           "write result beyond AIX offset range rejected");
    write_reply.offset = 0;
    write_reply.pad = 1;
    tap_ok(state, !usfs_write_out_valid(&write_reply, 4),
           "write padding rejected");

    memset(&statfs_reply, 0, sizeof(statfs_reply));
    statfs_reply.blocks = 10;
    statfs_reply.bfree = 5;
    statfs_reply.bavail = 4;
    statfs_reply.files = 10;
    statfs_reply.ffree = 5;
    statfs_reply.bsize = 4096;
    statfs_reply.namemax = USFS_MAX_NAME - 1u;
    tap_ok(state, usfs_statfs_out_valid(&statfs_reply),
           "valid statfs reply accepted");
    statfs_reply.bfree = 11;
    tap_ok(state, !usfs_statfs_out_valid(&statfs_reply),
           "statfs free blocks beyond total rejected");
    statfs_reply.bfree = 5;
    statfs_reply.bavail = 6;
    tap_ok(state, !usfs_statfs_out_valid(&statfs_reply),
           "statfs available blocks beyond free rejected");
    statfs_reply.bavail = 4;
    statfs_reply.ffree = 11;
    tap_ok(state, !usfs_statfs_out_valid(&statfs_reply),
           "statfs free files beyond total rejected");
    statfs_reply.ffree = 5;
    statfs_reply.namemax = USFS_MAX_NAME;
    tap_ok(state, !usfs_statfs_out_valid(&statfs_reply),
           "statfs oversized name limit rejected");
    statfs_reply.namemax = 1;
    statfs_reply.bsize = 0;
    tap_ok(state, !usfs_statfs_out_valid(&statfs_reply),
           "statfs zero block size rejected");

    memset(directory, 0, sizeof(directory));
    directory_header->snapshot_id = 1;
    directory_header->count = 1;
    directory_entry->ino = 2;
    directory_entry->namelen = 3;
    directory_entry->reclen = 24;
    memcpy(directory_name, "abc", 4);
    tap_ok(state, usfs_readdir_reply_valid(directory, 40),
           "valid readdir reply accepted");
    directory_header->snapshot_id = 0;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 40),
           "zero directory snapshot identity rejected");
    directory_header->snapshot_id = USFS_DIRECTORY_CURSOR_ID_MAX + 1u;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 40),
           "directory snapshot identity outside positive offset range rejected");
    directory_header->snapshot_id = 1;
    directory_header->count = 2;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 40),
           "readdir count beyond records rejected");
    directory_header->count = 1;
    directory_entry->reclen = 16;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 40),
           "short readdir record rejected");
    directory_entry->reclen = 24;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 41),
           "trailing readdir bytes rejected");
    directory_header->pad = 1;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 40),
           "readdir header padding rejected");
    directory_header->pad = 0;
    directory_entry->ino = 0;
    tap_ok(state, !usfs_readdir_reply_valid(directory, 40),
           "zero readdir inode rejected");
}

static void test_defensive_exits(struct tap_state *state)
{
    uint64_t u64 = 0;
    struct usfs_setattr_in setattr;
    struct usfs_attr attr;
    struct usfs_reply_header header;
    unsigned char storage[32];
    struct usfs_dirent *entry = (struct usfs_dirent *)storage;
    static const char upper_hex[] = "cookie=ABCDEF";
    static const char clipped_name[] = "abc\0junk";

    tap_ok(state, !usfs_u64_add_checked(1, 2, NULL),
           "u64 null result rejected");
    tap_ok(state, !usfs_u32_sub_checked(2, 1, NULL),
           "u32 subtraction null result rejected");
    tap_ok(state, !usfs_u64_sub_checked(2, 1, NULL),
           "u64 subtraction null result rejected");
    tap_ok(state, !usfs_size_to_u32_checked(1, NULL),
           "size conversion null result rejected");
    tap_ok(state, !usfs_align8_checked(1, NULL),
           "alignment null result rejected");
    tap_ok(state, usfs_info_field_u64(upper_hex, sizeof(upper_hex),
                                      "cookie", 6, 16, &u64) == 1 &&
                  u64 == UINT64_C(0xabcdef),
           "uppercase hexadecimal mount field parsed");

    memset(&setattr, 0, sizeof(setattr));
    tap_ok(state, !usfs_setattr_valid(NULL), "null setattr rejected");
    setattr.pad = 1;
    tap_ok(state, !usfs_setattr_valid(&setattr),
           "setattr padding rejected");
    memset(&setattr, 0, sizeof(setattr));
    setattr.valid = USFS_SET_MTIME;
    setattr.mtimensec = 1000000000u;
    tap_ok(state, !usfs_setattr_valid(&setattr),
           "setattr invalid mtime rejected");
    memset(&setattr, 0, sizeof(setattr));
    setattr.valid = USFS_SET_CTIME;
    setattr.ctimensec = 1000000000u;
    tap_ok(state, !usfs_setattr_valid(&setattr),
           "setattr invalid ctime rejected");

    memset(&attr, 0, sizeof(attr));
    attr.ino = 1;
    attr.blksize = 4096;
    tap_ok(state, !usfs_attr_valid(NULL), "null attributes rejected");
    attr.pad = 1;
    tap_ok(state, !usfs_attr_valid(&attr), "attribute padding rejected");

    tap_ok(state, calculate_bounded_opcode_specific_argument_length("abcd", 5, 4) == 0,
           "name terminator beyond maximum is not scanned");
    tap_ok(state, calculate_bounded_opcode_specific_argument_length("x", 2, 0) == 0,
           "zero name maximum has no bounded length");
    tap_ok(state, calculate_bounded_opcode_specific_argument_length(clipped_name,
                                           sizeof(clipped_name), 4) == 4,
           "name scan is clipped to its declared maximum");

    memset(&header, 0, sizeof(header));
    header.len = sizeof(header);
    header.version = USFS_PROTOCOL_VERSION;
    header.opcode = USFS_OP_GETATTR;
    tap_ok(state, !usfs_out_header_valid(NULL, sizeof(header)),
           "null reply header rejected");
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header) - 1u),
           "truncated reply header rejected");
    header.pad = 1;
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header)),
           "reply header padding rejected");
    header.pad = 0;
    header.opcode = 0;
    tap_ok(state, !usfs_out_header_valid(&header, sizeof(header)),
           "reply header invalid opcode rejected");

    memset(storage, 0, sizeof(storage));
    entry->ino = 2;
    entry->namelen = 3;
    entry->reclen = 24;
    memcpy(entry + 1, "abc", 4);
    tap_ok(state, !usfs_dirent_valid(NULL, sizeof(storage)),
           "null directory entry rejected");
    tap_ok(state, !usfs_dirent_valid(entry, sizeof(*entry) - 1u),
           "truncated directory entry header rejected");
    entry->namelen = 0;
    tap_ok(state, !usfs_dirent_valid(entry, sizeof(storage)),
           "empty directory entry name rejected");
    entry->namelen = 3;
    tap_ok(state, !usfs_dirent_valid(entry, sizeof(*entry)),
           "directory record beyond available bytes rejected");
    tap_ok(state, !usfs_readdir_reply_valid(NULL, sizeof(uint64_t)),
           "null readdir reply rejected");
    tap_ok(state, !usfs_readdir_reply_valid(storage, 1),
           "truncated readdir header rejected");
}

static void test_atomic_creation(struct tap_state *state)
{
    struct usfs_create_attr_in input = { 0 };
    struct usfs_create_out output = { 0 };
    TAP_EQ_SIZE(state, sizeof(input), 80, "atomic create request preserves fixed-width aligned layout");
    input.attr.valid = USFS_SET_MODE;
    tap_ok(state, usfs_create_attr_in_valid(&input), "atomic create accepts mode-only default activation");
    input.attr.valid = 0;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create requires mode");
    input.attr.valid = USFS_SET_MODE | USFS_SET_TIMES_NOW;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create rejects unsupported initial bits");
    input.attr.valid = USFS_SET_MODE; input.attr.fh = 1;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create rejects preexisting handle");
    input.attr.fh = 0; input.attr.pad = 1;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create rejects padding");
    input.attr.pad = 0; input.attr.size = UINT64_MAX;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create rejects unrepresentable size");
    input.attr.size = 0; input.attr.ctimensec = 1000000000u;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create rejects malformed time");
    input.attr.ctimensec = 0; input.activation = 3;
    tap_ok(state, !usfs_create_attr_in_valid(&input), "atomic create rejects unknown activation");
    output.attr.ino = 42; output.attr.mode = 0100600; output.attr.blksize = 4096;
    tap_ok(state, usfs_create_attr_out_valid(&output, USFS_CREATE_DEFAULT), "default reply transfers no identity or handle");
    output.nodeid = 42;
    tap_ok(state, usfs_create_attr_out_valid(&output, USFS_CREATE_LOOKUP), "lookup reply transfers only identity");
    output.fh = 77;
    tap_ok(state, usfs_create_attr_out_valid(&output, USFS_CREATE_OPEN), "open reply transfers identity and handle");
    tap_ok(state, !usfs_create_attr_out_valid(&output, USFS_CREATE_LOOKUP), "lookup reply rejects a temporary open");
    output.fh = 0;
    tap_ok(state, !usfs_create_attr_out_valid(&output, USFS_CREATE_DEFAULT), "default reply rejects activated identity");
    output.nodeid = USFS_ROOT_ID;
    tap_ok(state, !usfs_create_attr_out_valid(&output, USFS_CREATE_OPEN), "created regular file cannot claim the reserved root identity");
}

static void test_file_identifier_replies(struct tap_state *state)
{
    struct usfs_fid_out fid_reply = { 0 };
    struct usfs_vget_out vget_reply = { 0 };

    tap_ok(state, !usfs_fid_out_valid(&fid_reply), "zero backend identifier rejected");

    fid_reply.token = 7;
    tap_ok(state, usfs_fid_out_valid(&fid_reply), "nonzero backend identifier accepted");

    vget_reply.token = fid_reply.token;
    vget_reply.entry.nodeid = 42;
    vget_reply.entry.attr.ino = 42;
    vget_reply.entry.attr.mode = 0100600;
    vget_reply.entry.attr.nlink = 1;
    vget_reply.entry.attr.blksize = 4096;
    tap_ok(state, usfs_vget_out_valid(&vget_reply, fid_reply.token), "matching vget identity and entry accepted");
    tap_ok(state, !usfs_vget_out_valid(&vget_reply, 0), "zero requested vget identifier rejected");
    tap_ok(state, !usfs_vget_out_valid(&vget_reply, 8), "wrong requested vget identifier rejected");

    vget_reply.token = 0;
    tap_ok(state, !usfs_vget_out_valid(&vget_reply, fid_reply.token), "zero vget reply identifier rejected");

    vget_reply.token = fid_reply.token;
    vget_reply.entry.nodeid = 0;
    tap_ok(state, !usfs_vget_out_valid(&vget_reply, fid_reply.token), "vget reply without a node rejected");

    vget_reply.entry.nodeid = 42;
    vget_reply.entry.attr.blksize = 0;
    tap_ok(state, !usfs_vget_out_valid(&vget_reply, fid_reply.token), "vget reply with malformed attributes rejected");
}

int main(void)
{
    struct tap_state state;

    tap_plan(&state, 282);
    test_layout(&state);
    test_status(&state);
    test_arithmetic(&state);
    test_mount_metadata(&state);
    test_request_sizes(&state);
    test_scalars(&state);
    test_durability(&state);
    test_attributes(&state);
    test_golden_headers(&state);
    test_names(&state);
    test_headers(&state);
    test_dirent(&state);
    test_operation_replies(&state);
    test_defensive_exits(&state);
    test_atomic_creation(&state);
    test_file_identifier_replies(&state);
    return tap_finish(&state);
}
