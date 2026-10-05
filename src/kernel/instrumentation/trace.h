// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_INSTRUMENTATION_TRACE_H
#define USFS_INSTRUMENTATION_TRACE_H

/*
 * Stable AIX system-trace ABI. Hook IDs and subhook meanings are permanent
 * once released because saved binary traces are decoded by hook/subhook.
 */
#define USFS_TRACE_ABI_VERSION 1u

#define USFS_TRACE_CONTROL_HOOK 0xF5F10000UL
#define USFS_TRACE_REQUEST_HOOK 0xF5F20000UL
#define USFS_TRACE_FS_HOOK      0xF5F30000UL

enum usfs_trace_control_event
{
    USFS_TRACE_SCHEMA = 0x0001,
    USFS_TRACE_KEXT_STATE = 0x0002,
    USFS_TRACE_REGISTRATION = 0x0003,
    USFS_TRACE_CONNECTION = 0x0004,
    USFS_TRACE_MOUNT = 0x0005,
    USFS_TRACE_RUNTIME_CONFIG = 0x0006,
    USFS_TRACE_FAULT = 0x0007
};

enum usfs_trace_request_event
{
    USFS_TRACE_REQUEST_REJECTED = 0x0001,
    USFS_TRACE_REQUEST_QUEUED = 0x0002,
    USFS_TRACE_REQUEST_DELIVERED = 0x0003,
    USFS_TRACE_REPLY_ACCEPTED = 0x0004,
    USFS_TRACE_REQUEST_COMPLETED = 0x0005,
    USFS_TRACE_CONNECTION_ABORT = 0x0006
};

enum usfs_trace_fs_event
{
    USFS_TRACE_VFS_ENTRY = 0x0001,
    USFS_TRACE_VFS_RESULT = 0x0002,
    USFS_TRACE_VNODE_ENTRY = 0x0003,
    USFS_TRACE_VNODE_RESULT = 0x0004,
    USFS_TRACE_PAGER_ENTRY = 0x0005,
    USFS_TRACE_PAGER_RESULT = 0x0006,
    USFS_TRACE_CREATE_ATTR_REJECTED = 0x0007
};

enum usfs_trace_action
{
    USFS_TRACE_ACTION_ADD = 1,
    USFS_TRACE_ACTION_REMOVE = 2,
    USFS_TRACE_ACTION_ALLOCATE = 3,
    USFS_TRACE_ACTION_ADMIT = 4,
    USFS_TRACE_ACTION_READY = 5,
    USFS_TRACE_ACTION_CLOSE = 6,
    USFS_TRACE_ACTION_DEALLOCATE = 7,
    USFS_TRACE_ACTION_RESERVE = 8,
    USFS_TRACE_ACTION_PUBLISH = 9,
    USFS_TRACE_ACTION_UNMOUNT = 10,
    USFS_TRACE_ACTION_FORCE = 11,
    USFS_TRACE_ACTION_DEFERRED_RELEASE = 12
};

enum usfs_trace_registration_component
{
    USFS_TRACE_REG_PAGER = 1,
    USFS_TRACE_REG_GFS = 2,
    USFS_TRACE_REG_DEVSW = 3
};

/* Values below are copied into the trace stream, not exported as kernel ABI.
 * Keep them immutable even if an internal implementation enum later changes. */
enum usfs_trace_kext_state
{
    USFS_TRACE_KEXT_DOWN = 0,
    USFS_TRACE_KEXT_STARTING = 1,
    USFS_TRACE_KEXT_ACTIVE = 2,
    USFS_TRACE_KEXT_STOPPING = 3,
    USFS_TRACE_KEXT_CLEANUP_REQUIRED = 4
};

enum usfs_trace_connection_state
{
    USFS_TRACE_CONN_ACTIVE = 1,
    USFS_TRACE_CONN_UNHEALTHY = 2,
    USFS_TRACE_CONN_DEAD = 3
};

enum usfs_trace_mount_state
{
    USFS_TRACE_MOUNT_ACTIVE = 1,
    USFS_TRACE_MOUNT_STALE = 2
};

enum usfs_trace_request_class
{
    USFS_TRACE_CLASS_NORMAL = 0,
    USFS_TRACE_CLASS_PAGER = 1
};

enum usfs_trace_outcome
{
    USFS_TRACE_OUTCOME_SUCCESS = 1,
    USFS_TRACE_OUTCOME_DAEMON_ERROR = 2,
    USFS_TRACE_OUTCOME_TRANSPORT_FAILURE = 3,
    USFS_TRACE_OUTCOME_TIMEOUT = 4,
    USFS_TRACE_OUTCOME_INTERRUPTED = 5
};

enum usfs_trace_fault
{
    USFS_TRACE_FAULT_NONE = 0,
    USFS_TRACE_FAULT_TIMEOUT = 1,
    USFS_TRACE_FAULT_PROTOCOL = 2,
    USFS_TRACE_FAULT_DAEMON_LOST = 3,
    USFS_TRACE_FAULT_FORCED_RECOVERY = 4,
    USFS_TRACE_FAULT_CLEANUP_REQUIRED = 5
};

enum usfs_trace_operation
{
    USFS_TRACE_OP_LOOKUP = 1,
    USFS_TRACE_OP_GETATTR = 2,
    USFS_TRACE_OP_OPEN = 3,
    USFS_TRACE_OP_READ = 4,
    USFS_TRACE_OP_RELEASE = 5,
    USFS_TRACE_OP_READDIR = 6,
    USFS_TRACE_OP_READLINK = 7,
    USFS_TRACE_OP_STATFS = 8,
    USFS_TRACE_OP_CREATE = 9,
    USFS_TRACE_OP_MKDIR = 10,
    USFS_TRACE_OP_UNLINK = 11,
    USFS_TRACE_OP_RMDIR = 12,
    USFS_TRACE_OP_RENAME = 13,
    USFS_TRACE_OP_SYMLINK = 14,
    USFS_TRACE_OP_LINK = 15,
    USFS_TRACE_OP_SETATTR = 16,
    USFS_TRACE_OP_WRITE = 17,
    USFS_TRACE_OP_FLUSH = 18,
    USFS_TRACE_OP_FSYNC = 19,
    USFS_TRACE_OP_SYNCFS = 20,
    USFS_TRACE_OP_CREATE_ATTR = 21,
    USFS_TRACE_OP_FORGET = 22
};

enum usfs_trace_vfs_operation
{
    USFS_TRACE_VFS_MOUNT = 1,
    USFS_TRACE_VFS_UNMOUNT = 2,
    USFS_TRACE_VFS_ROOT = 3,
    USFS_TRACE_VFS_STATFS = 4,
    USFS_TRACE_VFS_SYNC = 5,
    USFS_TRACE_VFS_SYNCVFS = 6,
    USFS_TRACE_VFS_VGET = 7
};

enum usfs_trace_vnode_operation
{
    USFS_TRACE_VN_LOOKUP = 1,
    USFS_TRACE_VN_GETATTR = 2,
    USFS_TRACE_VN_SETATTR = 3,
    USFS_TRACE_VN_OPEN = 4,
    USFS_TRACE_VN_CLOSE = 5,
    USFS_TRACE_VN_CREATE = 6,
    USFS_TRACE_VN_READ = 7,
    USFS_TRACE_VN_WRITE = 8,
    USFS_TRACE_VN_READDIR = 9,
    USFS_TRACE_VN_READLINK = 10,
    USFS_TRACE_VN_LINK = 11,
    USFS_TRACE_VN_UNLINK = 12,
    USFS_TRACE_VN_RENAME = 13,
    USFS_TRACE_VN_MKDIR = 14,
    USFS_TRACE_VN_RMDIR = 15,
    USFS_TRACE_VN_SYMLINK = 16,
    USFS_TRACE_VN_FSYNC = 17,
    USFS_TRACE_VN_MAP = 18,
    USFS_TRACE_VN_UNMAP = 19,
    USFS_TRACE_VN_STRATEGY = 20,
    USFS_TRACE_VN_RELEASE = 21,
    USFS_TRACE_VN_UNSUPPORTED = 22
};

#ifdef USFS_TRACE_DISABLED
    #define USFS_TRACE_DISCARD0(event_id_)                                                                                                           \
        do                                                                                                                                           \
        {                                                                                                                                            \
            (void)sizeof (event_id_);                                                                                                                \
        }                                                                                                                                            \
        while (0)
    #define USFS_TRACE_DISCARD1(event_id_, first_word_)               USFS_TRACE_DISCARD5 (event_id_, first_word_, 0, 0, 0, 0)
    #define USFS_TRACE_DISCARD2(event_id_, first_word_, second_word_) USFS_TRACE_DISCARD5 (event_id_, first_word_, second_word_, 0, 0, 0)
    #define USFS_TRACE_DISCARD3(event_id_, first_word_, second_word_, third_word_)                                                                   \
        USFS_TRACE_DISCARD5 (event_id_, first_word_, second_word_, third_word_, 0, 0)
    #define USFS_TRACE_DISCARD4(event_id_, first_word_, second_word_, third_word_, fourth_word_)                                                     \
        USFS_TRACE_DISCARD5 (event_id_, first_word_, second_word_, third_word_, fourth_word_, 0)
    #define USFS_TRACE_DISCARD5(event_id_, first_word_, second_word_, third_word_, fourth_word_, fifth_word_)                                        \
        do                                                                                                                                           \
        {                                                                                                                                            \
            (void)sizeof (event_id_);                                                                                                                \
            (void)sizeof (first_word_);                                                                                                              \
            (void)sizeof (second_word_);                                                                                                             \
            (void)sizeof (third_word_);                                                                                                              \
            (void)sizeof (fourth_word_);                                                                                                             \
            (void)sizeof (fifth_word_);                                                                                                              \
        }                                                                                                                                            \
        while (0)
    #define USFS_TRACE_CONTROL0 USFS_TRACE_DISCARD0
    #define USFS_TRACE_CONTROL1 USFS_TRACE_DISCARD1
    #define USFS_TRACE_CONTROL2 USFS_TRACE_DISCARD2
    #define USFS_TRACE_CONTROL3 USFS_TRACE_DISCARD3
    #define USFS_TRACE_CONTROL4 USFS_TRACE_DISCARD4
    #define USFS_TRACE_CONTROL5 USFS_TRACE_DISCARD5
    #define USFS_TRACE_REQUEST0 USFS_TRACE_DISCARD0
    #define USFS_TRACE_REQUEST1 USFS_TRACE_DISCARD1
    #define USFS_TRACE_REQUEST2 USFS_TRACE_DISCARD2
    #define USFS_TRACE_REQUEST3 USFS_TRACE_DISCARD3
    #define USFS_TRACE_REQUEST4 USFS_TRACE_DISCARD4
    #define USFS_TRACE_REQUEST5 USFS_TRACE_DISCARD5
    #define USFS_TRACE_FS0      USFS_TRACE_DISCARD0
    #define USFS_TRACE_FS1      USFS_TRACE_DISCARD1
    #define USFS_TRACE_FS2      USFS_TRACE_DISCARD2
    #define USFS_TRACE_FS3      USFS_TRACE_DISCARD3
    #define USFS_TRACE_FS4      USFS_TRACE_DISCARD4
    #define USFS_TRACE_FS5      USFS_TRACE_DISCARD5
#else
    #define USFS_TRACE_CONTROL0(event_id_)                            TRCHKL0T (USFS_TRACE_CONTROL_HOOK | (event_id_))
    #define USFS_TRACE_CONTROL1(event_id_, first_word_)               TRCHKL1T (USFS_TRACE_CONTROL_HOOK | (event_id_), (first_word_))
    #define USFS_TRACE_CONTROL2(event_id_, first_word_, second_word_) TRCHKL2T (USFS_TRACE_CONTROL_HOOK | (event_id_), (first_word_), (second_word_))
    #define USFS_TRACE_CONTROL3(event_id_, first_word_, second_word_, third_word_)                                                                   \
        TRCHKL3T (USFS_TRACE_CONTROL_HOOK | (event_id_), (first_word_), (second_word_), (third_word_))
    #define USFS_TRACE_CONTROL4(event_id_, first_word_, second_word_, third_word_, fourth_word_)                                                     \
        TRCHKL4T (USFS_TRACE_CONTROL_HOOK | (event_id_), (first_word_), (second_word_), (third_word_), (fourth_word_))
    #define USFS_TRACE_CONTROL5(event_id_, first_word_, second_word_, third_word_, fourth_word_, fifth_word_)                                        \
        TRCHKL5T (USFS_TRACE_CONTROL_HOOK | (event_id_), (first_word_), (second_word_), (third_word_), (fourth_word_), (fifth_word_))

    #define USFS_TRACE_REQUEST0(event_id_)                            TRCHKL0T (USFS_TRACE_REQUEST_HOOK | (event_id_))
    #define USFS_TRACE_REQUEST1(event_id_, first_word_)               TRCHKL1T (USFS_TRACE_REQUEST_HOOK | (event_id_), (first_word_))
    #define USFS_TRACE_REQUEST2(event_id_, first_word_, second_word_) TRCHKL2T (USFS_TRACE_REQUEST_HOOK | (event_id_), (first_word_), (second_word_))
    #define USFS_TRACE_REQUEST3(event_id_, first_word_, second_word_, third_word_)                                                                   \
        TRCHKL3T (USFS_TRACE_REQUEST_HOOK | (event_id_), (first_word_), (second_word_), (third_word_))
    #define USFS_TRACE_REQUEST4(event_id_, first_word_, second_word_, third_word_, fourth_word_)                                                     \
        TRCHKL4T (USFS_TRACE_REQUEST_HOOK | (event_id_), (first_word_), (second_word_), (third_word_), (fourth_word_))
    #define USFS_TRACE_REQUEST5(event_id_, first_word_, second_word_, third_word_, fourth_word_, fifth_word_)                                        \
        TRCHKL5T (USFS_TRACE_REQUEST_HOOK | (event_id_), (first_word_), (second_word_), (third_word_), (fourth_word_), (fifth_word_))

    #define USFS_TRACE_FS0(event_id_)                            TRCHKL0T (USFS_TRACE_FS_HOOK | (event_id_))
    #define USFS_TRACE_FS1(event_id_, first_word_)               TRCHKL1T (USFS_TRACE_FS_HOOK | (event_id_), (first_word_))
    #define USFS_TRACE_FS2(event_id_, first_word_, second_word_) TRCHKL2T (USFS_TRACE_FS_HOOK | (event_id_), (first_word_), (second_word_))
    #define USFS_TRACE_FS3(event_id_, first_word_, second_word_, third_word_)                                                                        \
        TRCHKL3T (USFS_TRACE_FS_HOOK | (event_id_), (first_word_), (second_word_), (third_word_))
    #define USFS_TRACE_FS4(event_id_, first_word_, second_word_, third_word_, fourth_word_)                                                          \
        TRCHKL4T (USFS_TRACE_FS_HOOK | (event_id_), (first_word_), (second_word_), (third_word_), (fourth_word_))
    #define USFS_TRACE_FS5(event_id_, first_word_, second_word_, third_word_, fourth_word_, fifth_word_)                                             \
        TRCHKL5T (USFS_TRACE_FS_HOOK | (event_id_), (first_word_), (second_word_), (third_word_), (fourth_word_), (fifth_word_))
#endif

#endif
