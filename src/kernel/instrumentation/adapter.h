// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_INSTRUMENTATION_ADAPTER_H
#define USFS_INSTRUMENTATION_ADAPTER_H

struct usfs_dev_cfg;
struct usfs_mount_data;
struct uio;

enum usfs_checkpoint
{
    USFS_INSTRUMENT_MOUNT_RESERVED = 1,
    USFS_INSTRUMENT_TERM_BEFORE_CLOSE = 2,
    USFS_INSTRUMENT_TERM_BUSY_REOPENED = 3,
    USFS_INSTRUMENT_CHANNEL_PRE_ADMISSION = 4,
    USFS_INSTRUMENT_TERM_GATE_CLOSED = 5,
    USFS_INSTRUMENT_CHANNEL_REJECTED = 6,
    USFS_INSTRUMENT_LOOKUP_POST_REPLY = 7,
    USFS_INSTRUMENT_LOOKUP_RESOLVED = 8,
    USFS_INSTRUMENT_CACHE_BEFORE_EVICT = 9,
    USFS_INSTRUMENT_CACHE_AFTER_WRITE = 10,
    USFS_INSTRUMENT_CACHE_EVICT_DONE = 11,
    USFS_INSTRUMENT_CHANNEL_UNPUBLISHED = 12,
    USFS_INSTRUMENT_VGET_RESERVED = 13,
    USFS_INSTRUMENT_FORCE_UNMOUNT_STALE = 14
};

enum usfs_instrumentation_node_reuse
{
    USFS_INSTRUMENT_NODE_REUSE_NONE = 0,
    USFS_INSTRUMENT_NODE_REUSE_LOOKUP = 1,
    USFS_INSTRUMENT_NODE_REUSE_PARENT = 2,
    USFS_INSTRUMENT_NODE_REUSE_CREATE = 3
};

/* Configuration and lifecycle policy. */
uint32_t usfs_instrumentation_config_flags (void);
int validate_instrumentation_config (const struct usfs_dev_cfg * device_config);
int set_up_instrumentation (const struct usfs_dev_cfg * device_config);
int initialize_instrumentation (void);
void tear_down_instrumentation (void);
int should_defer_kext_cleanup (const struct usfs_dev_cfg * device_config);
void on_kext_cleanup_deferred (const uint32_t generation);
int process_instrumentation_cfg_command (const int command, struct uio * user_io_request);
int is_instrumentation_cfg_command (const int command);
int usfs_prepare_deferred_cleanup (void);

/*
 * Checkpoints may sleep in an instrumented build. Callers must not hold a
 * simple lock across this interface.
 */
int usfs_checkpoint (const enum usfs_checkpoint checkpoint);

/* Observation calls never sleep and may be made while global_lock is held. */
void usfs_instrumentation_node_created (struct usfs_mount_data * mount_data, const uint64_t node_id);
void usfs_instrumentation_node_reclaimed (const uint64_t node_id, const int is_linked);
void usfs_instrumentation_node_reused (const enum usfs_instrumentation_node_reuse source);
void usfs_instrumentation_parent_rebuilt (void);
void usfs_instrumentation_vnode_hold (const uint64_t reference_count);
void usfs_instrumentation_vnode_release (const uint64_t reference_count);

/* Recognize and dispatch commands owned by an instrumentation backend. */
int process_instrumentation_dev_ctl_command (const int command, void * user_space_buffer, const chan_t channel);
int is_instrumentation_dev_ctl_command (const int command);

#endif
