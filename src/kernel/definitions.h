// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEFINITIONS_H
#define USFS_DEFINITIONS_H

#include <sys/access.h>
#include <sys/adspace.h>
#include <sys/atomic_op.h>
#include <sys/buf.h>
#include <sys/chownx.h>
#include <sys/cred.h>
#include <sys/device.h>
#include <sys/dir.h>
#include <sys/dump.h>
#include <sys/errno.h>
#include <sys/gfs.h>
#include <sys/intr.h>
#include <sys/lock_alloc.h>
#include <sys/lock_def.h>
#include <sys/m_param.h>
#include <sys/malloc.h>
#include <sys/pin.h>
/* Import readiness constants/selnotify without the userspace poll wrapper. */
#define _MSGQSUPPORT
#include <sys/poll.h>
#undef _MSGQSUPPORT
#include <sys/priv.h>
#include <sys/shm.h>
#include <sys/sleep.h>
#include <sys/syncvfs.h>
#include <sys/syspest.h>
#include <sys/systm.h>
#include <sys/timer.h>
#include <sys/trchkid.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/uprintf.h>
#include <sys/vattr.h>
#include <sys/vfs.h>
#include <sys/vmuser.h>
#include <sys/vnode.h>

#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "device/protocol/request_build.h"
#include "device/protocol/transport_validation.h"
#include "fs/helpers.h"
#include "fs/io_helpers.h"
#include "instrumentation/adapter.h"
#include "instrumentation/services.h"
#include "instrumentation/trace.h"
#include "synchronized.h"
#include "usfs_config.h"
#include "usfs_proto.h"
#include "usfs_validate.h"

#define ignore_parameter (void)

enum usfs_conn_state
{
    USFS_CONN_ACTIVE = 1,
    USFS_CONN_UNHEALTHY = 2,
    USFS_CONN_DEAD = 3,
    USFS_CONN_CLOSED = 4
};

enum usfs_request_class
{
    USFS_REQUEST_NORMAL = 0,
    USFS_REQUEST_PAGER = 1
};

enum usfs_kext_state
{
    USFS_KEXT_DOWN = 0,
    USFS_KEXT_STARTING = 1,
    USFS_KEXT_ACTIVE = 2,
    USFS_KEXT_STOPPING = 3,
    USFS_KEXT_CLEANUP_REQUIRED = 4
};

enum usfs_req_state
{
    USFS_REQ_PENDING = 1,
    USFS_REQ_SENDING = 2,
    USFS_REQ_SENT = 3,
    USFS_REQ_REPLYING = 4,
    USFS_REQ_ANSWERED = 5,
    USFS_REQ_ABORTED = 6
};

enum usfs_mount_state
{
    USFS_MOUNT_ACTIVE = 1,
    USFS_MOUNT_STALE = 2
};

struct wait_control_block;

struct usfs_request
{
    struct usfs_request * next;
    int state;
    int abandoned;
    int counted;
    int cleanup;
    struct wait_control_block * wait;
    uint64_t id;
    char * request_buffer;
    uint32_t request_buffer_size;
    char * reply_buffer;
    uint32_t max_allowed_reply_buffer_size;
    uint32_t reply_buffer_size;
    int error;
    int metrics_finished;
    int request_class;
    uint64_t started_ns;
};

// todo: rename to rpc connection?
struct usfs_connection
{
    Simple_lock lock; // Serializes access to this connection's mutable state.
    int state;        // Current connection lifecycle state.
    chan_t channel;   // Multiplexed device channel that identifies this connection.
    dev_t device_number;
    ushort select_events;
    uint64_t cookie;  // Per-connection value returned to the userspace daemon.
    int refs_counter; // References held by mounts and temporary users.
    struct usfs_mount_data * mounted_data;
    int mounts_counter;                            // Number of mounts bound to this connection.
    uint64_t next_request_number;                  // Next unique identifier assigned to a request.
    tid_t read_queue_event;                        // Event used to wake daemon readers waiting for requests.
    struct usfs_request * pending_requests_head;   // First request waiting for delivery to the daemon.
    struct usfs_request * pending_requests_tail;   // Last request waiting for delivery to the daemon.
    struct usfs_request * delivered_requests_head; // First delivered request awaiting a daemon reply.
    uint32_t request_timeout_ms;                   // Reply deadline for ordinary filesystem requests.
    uint32_t pager_timeout_ms;                     // Reply deadline for pager requests.
    uint32_t max_outstanding_requests;             // Per-connection limit on accepted requests.
    uint32_t outstanding_requests;                 // Accepted requests currently consuming that limit.
    uint32_t cleanup_outstanding;                  // At most one reserved cleanup exchange.
    struct usfs_request * cleanup_waiters;         // FIFO admission waiters; protected by lock.
    int ready;                                     // The daemon has received this connection's welcome data.
    int degraded_mount_counted;                    // Prevents duplicate degraded-mount observability reports.
};

struct usfs_runtime_limits
{
    uint32_t request_timeout_ms;
    uint32_t pager_timeout_ms;
    uint32_t max_outstanding_requests;
};

#define USFS_MAX_CONNECTIONS 64
#define USFS_STATUS_CHANNEL  USFS_MAX_CONNECTIONS

struct usfs_open_state
{
    struct usfs_open_state * next;
    uint64_t fh;
    int32long64_t flags;
    uint32_t refs;
    uint32_t active;
    uint32_t closing;
    uint32_t operation_refs;
    uint32_t cache_refs;
    uint32_t read_mappings;
    uint32_t write_mappings;
};

struct usfs_node
{
    struct usfs_node * next;
    struct gnode * gn;
    struct vnode * vn;
    uint64_t nodeid;
    uint64_t fid_token; /* Backend identity recorded when FID/VGET resolves this vnode. */
    uint64_t parent;
    uint64_t lookup_refs; /* successful reply transfers, including vnode reuse */
    struct usfs_open_state * opens;
    uint32_t mapping_count;
    /* Sticky until vnode reclamation; protected by namespace_lock/mutation_lock. */
    uint32_t cache_shared_writable;
    struct usfs_open_state *cache_reader, *cache_writer;
    uint64_t cache_size;
    uint64_t cache_extent;
    int writeback_error;
    Complex_lock pageout_lock;  /* backend pageout versus truncate */
    Complex_lock mutation_lock; /* process write syscall and truncate ownership */
    short gn_lock_occurrence;
    short vn_lock_occurrence;
};

struct usfs_mount_data
{
    Complex_lock namespace_lock; /* namespace and authorization metadata */
    struct usfs_connection * conn;
    struct usfs_node * root;
    struct usfs_node * nodes;
    uint64_t reclaiming_nodes; /* Detached nodes still using this mount; global_lock. */
    uint32_t inflight_vgets;    /* VGET callbacks retaining this mount; global_lock. */
    int unmount_in_progress;   /* Protects the forced-unmount caller outside global_lock. */
    int stale_accounted;       /* Deferred recovery owns one stale-mount/VFS release. */
    int state;
    int writable;
};

struct usfs_private_data
{
    int counter;
};

extern Simple_lock g_connections_table_lock;
extern Simple_lock g_lifecycle_lock;
extern Complex_lock global_lock;

extern int g_mount_count;
extern int g_gate_is_open;
extern int g_kext_is_running_control_operation;
extern int g_status_channel_present;

extern struct usfs_connection * g_connections[USFS_MAX_CONNECTIONS];
extern struct usfs_runtime_limits g_usfs_runtime_limits;
extern struct vnodeops gn_ops;
extern struct vfsops vfsops;
extern struct gfs gfs;
extern struct usfs_private_data usfs_private_data;
extern struct devsw g_device_driver_descriptor;

struct usfs_connection * get_connection_by_channel (chan_t chan);
void acquire_connection (struct usfs_connection * conn);
void release_connection (struct usfs_connection * connection);
int usfs_call (struct usfs_connection * connection, struct usfs_request * request);
int usfs_call_with_specific_request_class (struct usfs_connection * connection, struct usfs_request * request, enum usfs_request_class request_class);
void abort_connection (struct usfs_connection * connection);
void wake_closed_connection (struct usfs_connection * connection);

enum usfs_request_outcome
{
    USFS_REQUEST_SUCCESS = 1,
    USFS_REQUEST_DAEMON_ERROR = 2,
    USFS_REQUEST_TRANSPORT_FAILURE = 3,
    USFS_REQUEST_TIMEOUT = 4,
    USFS_REQUEST_INTERRUPTED = 5
};

void set_up_observability_infrastructure (void);
void tear_down_observability_infrastructure (void);
void on_request_processing_started (struct usfs_request * request, enum usfs_request_class request_class, uint32_t outstanding_requests_counter);
void on_request_processing_finished (struct usfs_request * request, enum usfs_request_outcome outcome);
void on_request_rejected (void);
void on_connection_failed (struct usfs_connection * conn, int old_state, int new_state);
void on_mount_released (struct usfs_connection * connection);
void on_stale_mount_added (void);
void on_stale_mount_released (void);
void on_forced_recovery (const struct usfs_connection * connection, int should_defer_mount_cleanup);
void report_protocol_error (const struct usfs_connection * connection, int error, uint16_t opcode, uint64_t request_id);
void report_timeout (const struct usfs_connection * connection, uint16_t opcode, uint64_t request_id);
void report_connection_lost (const struct usfs_connection * connection);
void report_cleanup_failure (int error, uint32_t mounts_counter);
int take_kext_state_snapshot (struct kext_state * state, chan_t channel);
void usfs_ras_report (uint32_t fault, int error, int channel, uint16_t opcode, uint64_t unique, uint32_t mounts);
int take_kext_runtime_config_snapshot (struct kext_runtime_config * config);
int set_kext_runtime_config (const struct kext_runtime_config * config);
int usfs_lifecycle_reserve_mount (uint64_t chan, uint64_t cookie, struct usfs_connection ** conn_out);
void usfs_lifecycle_release_mount (struct usfs_connection * connection);
void usfs_lifecycle_finish_mount (struct usfs_connection * connection);
int has_active_connections (void);
int usfs_lifecycle_state_read (void);
int usfs_lifecycle_state_replace (int desired);
int usfs_lifecycle_state_claim (int expected, int desired);
int usfs_lifecycle_mpx_enter (const int is_deallocation);
void usfs_lifecycle_mpx_leave (void);
int usfs_lifecycle_mpx_begin_close (void);
int usfs_lifecycle_mpx_has_users (void);
int usfs_lifecycle_mpx_seal (void);
void usfs_lifecycle_mpx_reopen (void);
int usfs_lifecycle_mpx_activate (void);
enum usfs_control_policy
{
    USFS_CONTROL_EXACTLY_ONE_CONNECTION = 1,
    USFS_CONTROL_ONLY_COLLECTOR_CONNECTION = 2
};
int usfs_lifecycle_control_enter (chan_t collector, enum usfs_control_policy policy);
void usfs_lifecycle_control_leave (void);
void usfs_pager_strategy (struct buf *, int, int);
int usfs_pager_register (void);
int usfs_pager_unregister (void);

int usfs_init (struct gfs * gfsp);
int usfs_mount (struct vfs * vfsp, struct ucred * crp);
int usfs_unmount (struct vfs * vfsp, int flags, struct ucred * crp);
int usfs_root (struct vfs * vfsp, struct vnode ** vpp, struct ucred * crp);
int usfs_statfs (struct vfs * vfsp, struct statfs * stafsp, struct ucred * crp);
int usfs_sync (struct gfs * gfsp);
int usfs_vget (struct vfs * vfsp, struct vnode ** vpp, struct fileid * fidp, struct ucred * crp);
int usfs_cntl (struct vfs *, int, caddr_t, size_t, struct ucred *);
int usfs_quotactl (struct vfs *, int, uid_t, caddr_t, struct ucred *);
int usfs_syncvfs (struct gfs *, struct vfs *, int, struct ucred *);
int usfs_aclxcntl (struct vfs *, struct vnode *, int, struct uio *, size_t *, struct ucred *);
int usfs_statfsvp (struct vfs *, struct vnode *, struct statfs *, struct ucred *);

int gn_link (struct vnode *, struct vnode *, char *, struct ucred *);
int gn_mkdir (struct vnode *, char *, int32long64_t, struct ucred *);
int gn_mknod (struct vnode *, caddr_t, int32long64_t, dev_t, struct ucred *);
int gn_remove (struct vnode *, struct vnode *, char *, struct ucred *);
int gn_rename (struct vnode *, struct vnode *, caddr_t, struct vnode *, struct vnode *, caddr_t, struct ucred *);
int gn_rmdir (struct vnode *, struct vnode *, char *, struct ucred *);
int gn_lookup (struct vnode *, struct vnode **, char *, int32long64_t, struct vattr *, struct ucred *);
int gn_symlink (struct vnode *, char *, char *, struct ucred *);

int gn_fid (struct vnode *, struct fileid *, struct ucred *);
int gn_open (struct vnode *, int32long64_t, ext_t, caddr_t *, struct ucred *);
int gn_create (struct vnode *, struct vnode **, int32long64_t, caddr_t, int32long64_t, caddr_t *, struct ucred *);
int gn_hold (struct vnode *);
int gn_rele (struct vnode *);
int gn_close (struct vnode *, int32long64_t, caddr_t, struct ucred *);

int gn_access (struct vnode *, int32long64_t, int32long64_t, struct ucred *);
int gn_getattr (struct vnode *, struct vattr *, struct ucred *);
int gn_setattr (struct vnode *, int32long64_t, int32long64_t, int32long64_t, int32long64_t, struct ucred *);

int gn_fclear (struct vnode *, int32long64_t, offset_t, offset_t, caddr_t, struct ucred *);
int gn_fsync (struct vnode *, int32long64_t, int32long64_t, struct ucred *);
int gn_ftrunc (struct vnode *, int32long64_t, offset_t, caddr_t, struct ucred *);
int gn_rdwr (struct vnode *, enum uio_rw, int32long64_t, struct uio *, ext_t, caddr_t, struct vattr *, struct ucred *);
int gn_readlink (struct vnode *, struct uio *, struct ucred *);
int gn_readdir (struct vnode *, struct uio *, struct ucred *);

int gn_map (struct vnode *, caddr_t, uint32long64_t, uint32long64_t, uint32long64_t, struct ucred *);
int gn_unmap (struct vnode *, int32long64_t, struct ucred *);
int gn_lockctl (struct vnode *, offset_t, struct eflock *, int32long64_t, int (*) (), ulong *, struct ucred *);
int gn_ioctl (struct vnode *, int32long64_t, caddr_t, size_t, ext_t, struct ucred *);
int gn_select (struct vnode *, int32long64_t, ushort, ushort *, void (*) (), caddr_t, struct ucred *);
int gn_strategy (struct vnode *, struct buf *, struct ucred *);
int gn_revoke (struct vnode *, int32long64_t, int32long64_t, struct vattr *, struct ucred *);
int gn_getacl (struct vnode *, struct uio *, struct ucred *);
int gn_setacl (struct vnode *, struct uio *, struct ucred *);
int gn_getpcl (struct vnode *, struct uio *, struct ucred *);
int gn_setpcl (struct vnode *, struct uio *, struct ucred *);
int gn_seek (struct vnode *, offset_t *, struct ucred *);
int gn_fsync_range (struct vnode *, int32long64_t, int32long64_t, offset_t, offset_t, struct ucred *);
int gn_create_attr (struct vnode *, struct vnode **, int32long64_t, char *, struct vattr *, int32long64_t, caddr_t *, struct ucred *);
int gn_finfo (struct vnode *, int32long64_t, void *, size_t, struct ucred *);
int gn_map_lloff (struct vnode *, caddr_t, offset_t, offset_t, uint32long64_t, uint32long64_t, struct ucred *);
int gn_readdir_eofp (struct vnode *, struct uio *, int *, struct ucred *);
int gn_rdwr_attr (struct vnode *, enum uio_rw, int32long64_t, struct uio *, ext_t, caddr_t, struct vattr *, struct vattr *, struct ucred *);
int gn_memcntl (struct vnode *, int, void *, struct ucred *);
int gn_getea (struct vnode *, const char *, struct uio *, struct ucred *);
int gn_setea (struct vnode *, const char *, struct uio *, int, struct ucred *);
int gn_listea (struct vnode *, struct uio *, struct ucred *);
int gn_removeea (struct vnode *, const char *, struct ucred *);
int gn_statea (struct vnode *, const char *, struct vattr *, struct ucred *);
int gn_getxacl (struct vnode *, uint64_t, acl_type_t *, struct uio *, size_t *, mode_t *, struct ucred *);
int gn_setxacl (struct vnode *, uint64_t, acl_type_t, struct uio *, mode_t, struct ucred *);
int gn_erdwr_attr (struct vnode *, enum uio_rw, int32long64_t, struct uio *, ext_t, caddr_t, struct vattr *, struct vattr *, struct ucred *, struct file_secattr *);

void init_global_virtual_fs_operations (void);
void init_global_virtual_inode_operations (void);
void init_global_gfs_descriptor (void);
void init_global_lock (void);
void destroy_global_locks (void);
int register_fs_implementation (void);

void init_device_driver_descriptor (void);

int usfs_kext_entry (dev_t device_number, int command, struct uio * user_io_request);

#endif
