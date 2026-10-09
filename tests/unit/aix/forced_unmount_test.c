/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include <stdlib.h>
#include "definitions.h"

#include <string.h>

Complex_lock global_lock;
heapaddr_t kernel_heap;

static unsigned lock_depth;
static unsigned abort_calls;
static unsigned lifecycle_release_calls;
static unsigned connection_release_calls;
static unsigned vnode_free_calls;
static unsigned gnode_free_calls;
static unsigned node_free_calls;
static unsigned mutation_lock_frees;
static unsigned vfsrele_calls;
static unsigned forced_recovery_calls;
static unsigned deferred_recovery_calls;
static unsigned stale_mount_release_calls;
static unsigned connection_acquire_calls, forget_calls;
static uint64_t forget_id, forget_count;
static int forget_error;
static void (*dispose_hook)(void), (*abort_hook)(void), (*forget_hook)(void);
static struct usfs_mount_data *watched_mount;
static struct vnode *other_vnode;
static struct vfs *watched_vfs;
static unsigned mount_frees, dispose_calls;
static int dispose_error;

static void invoke_once(void (**hook)(void))
{
    void (*callback)(void) = *hook;
    *hook = NULL;
    if (callback != NULL) callback();
}

void simple_lock(Simple_lock *lock) { (void)lock; ++lock_depth; }
void simple_unlock(Simple_lock *lock) { (void)lock; --lock_depth; }
void acquire_connection(struct usfs_connection *connection)
{ (void)connection; ++connection_acquire_calls; }
static int _call_conn_no_reply(struct usfs_connection *conn, const uint16_t opcode,
    const uint64_t id, const void *body, const uint32_t size, struct ucred *crp)
{
    (void)conn; (void)crp;
    if (lock_depth != 0 || opcode != USFS_OP_FORGET || size != sizeof(struct usfs_forget_in))
        return EINVAL;
    ++forget_calls; forget_id = id;
    invoke_once(&forget_hook);
    forget_count = ((const struct usfs_forget_in *)body)->count;
    return forget_error;
}
#include "fs/forget.h"

void lock_write(complex_lock_t lock)
{
    (void)lock;
    lock_depth += 1;
}

void lock_done(complex_lock_t lock)
{
    (void)lock;
    lock_depth -= 1;
}

int usfs_checkpoint (const enum usfs_checkpoint checkpoint)
{
    (void)checkpoint;
    return 0;
}

void lock_free(void *lock)
{
    (void)lock;
    ++mutation_lock_frees;
}

int xmfree(void *address, heapaddr_t heap)
{
    if (address == watched_mount) ++mount_frees;
    (void)heap;
    node_free_calls += 1;
    return 0;
}

static void _free_vnode(struct vnode *vn, short occurrence)
{
    (void)vn;
    (void)occurrence;
    vnode_free_calls += 1;
}

static void _free_gnode(struct gnode *gn, short occurrence)
{
    (void)gn;
    (void)occurrence;
    gnode_free_calls += 1;
}

static void usfs_vnode_release_locked(struct vnode *vn)
{
    vn->v_count -= 1;
}

void usfs_instrumentation_node_reclaimed(uint64_t nodeid, int linked)
{
    (void)nodeid;
    (void)linked;
}

void abort_connection(struct usfs_connection *connection)
{
    abort_calls += 1;
    connection->state = USFS_CONN_DEAD;
    invoke_once(&abort_hook);
}

void usfs_lifecycle_release_mount(struct usfs_connection *connection)
{
    (void)connection;
    lifecycle_release_calls += 1;
}
void usfs_lifecycle_finish_mount(struct usfs_connection *connection)
{
    usfs_lifecycle_release_mount(connection);
    if (connection->state == USFS_CONN_ACTIVE) connection->state = USFS_CONN_CLOSED;
}

void release_connection(struct usfs_connection *connection)
{
    (void)connection;
    connection_release_calls += 1;
}

void on_forced_recovery (const struct usfs_connection *connection, int should_defer_mount_cleanup)
{
    (void)connection;
    forced_recovery_calls += 1;
    if (should_defer_mount_cleanup != 0) {
        deferred_recovery_calls += 1;
    }
}

void on_stale_mount_released(void)
{
    stale_mount_release_calls += 1;
}

int vfsrele(struct vfs *vfsp)
{
    (void)vfsp;
    vfsrele_calls += 1;
    return 0;
}

static int flush_cached_pages(struct vnode *vp, int range, uint64_t offset, uint64_t length, int discard)
{ (void)vp; (void)range; (void)offset; (void)length; (void)discard; return 0; }
static void dispose_cache_resources(struct usfs_node *node, struct usfs_mount_data *mount)
{
    (void)node;
    ++dispose_calls;
    invoke_once(&dispose_hook);
    if (mount == watched_mount && (mount_frees != 0 || mount->conn == NULL)) abort();
    if (dispose_error) abort_connection(mount->conn);
}
static void usfs_vnode_hold_locked(struct vnode *vn, enum usfs_instrumentation_node_reuse why)
{ (void)why; ++vn->v_count; }
#include "fs/operations/vfs_unmount.h"
#include "fs/operations/vnode_rele.h"

static void release_other(void)
{
    if (lock_depth != 0) abort();
    gn_rele(other_vnode);
    if (mount_frees != 0 || watched_vfs->vfs_data != (caddr_t)watched_mount) abort();
}

static void force_during_disposal(void)
{
    if (usfs_unmount(watched_vfs, UVMNT_FORCE, NULL) != 0 ||
        mount_frees != 0 || watched_mount->reclaiming_nodes != 1) abort();
}

static void ordinary_during_disposal(void)
{
    if (usfs_unmount(watched_vfs, 0, NULL) != EBUSY || mount_frees != 0 ||
        watched_vfs->vfs_data != (caddr_t)watched_mount || lifecycle_release_calls != 0) abort();
}

static void reset_counters(void)
{
    lock_depth = 0;
    mutation_lock_frees = 0;
    abort_calls = 0;
    lifecycle_release_calls = 0;
    connection_release_calls = 0;
    vnode_free_calls = 0;
    gnode_free_calls = 0;
    node_free_calls = 0;
    vfsrele_calls = 0;
    forced_recovery_calls = 0;
    deferred_recovery_calls = 0;
    stale_mount_release_calls = 0;
    connection_acquire_calls = forget_calls = 0;
    forget_error = 0; forget_id = forget_count = 0;
    dispose_hook = abort_hook = forget_hook = NULL;
    watched_mount = NULL; watched_vfs = NULL; other_vnode = NULL;
    mount_frees = dispose_calls = 0; dispose_error = 0;
}

static void initialize_node(struct usfs_node *node, struct vnode *vn,
                            struct gnode *gn, struct vfs *vfsp,
                            uint64_t nodeid)
{
    memset(node, 0, sizeof(*node));
    memset(vn, 0, sizeof(*vn));
    memset(gn, 0, sizeof(*gn));
    node->nodeid = nodeid;
    node->vn = vn;
    node->gn = gn;
    vn->v_count = 1;
    vn->v_vfsp = vfsp;
    vn->v_gnode = gn;
    gn->gn_data = (caddr_t)node;
}

int main(void)
{
    struct tap_state tap;
    struct usfs_connection conn;
    struct usfs_mount_data mount;
    struct usfs_node first;
    struct usfs_node second;
    struct vnode first_vnode;
    struct vnode second_vnode;
    struct gnode first_gnode;
    struct gnode second_gnode;
    struct vfs vfs;

    tap_plan(&tap, 29);

    reset_counters();
    memset(&conn, 0, sizeof(conn));
    memset(&mount, 0, sizeof(mount));
    memset(&vfs, 0, sizeof(vfs));
    conn.state = USFS_CONN_ACTIVE;
    mount.conn = &conn;
    mount.state = USFS_MOUNT_ACTIVE;
    vfs.vfs_data = (caddr_t)&mount;
    tap_ok(&tap, usfs_unmount(&vfs, 0, NULL) == 0 &&
                     vfs.vfs_data == NULL,
           "ordinary unmount detaches mount data immediately");
    tap_ok(&tap, abort_calls == 0 && lifecycle_release_calls == 1 &&
                     connection_release_calls == 1 && vfsrele_calls == 0 && conn.state == USFS_CONN_CLOSED,
           "ordinary unmount closes the channel after cleanup without forced recovery");

    reset_counters();
    memset(&conn, 0, sizeof(conn));
    memset(&mount, 0, sizeof(mount));
    memset(&vfs, 0, sizeof(vfs));
    conn.state = USFS_CONN_ACTIVE;
    mount.conn = &conn;
    mount.state = USFS_MOUNT_ACTIVE;
    vfs.vfs_data = (caddr_t)&mount;
    tap_ok(&tap, usfs_unmount(&vfs, UVMNT_FORCE, NULL) == 0 &&
                     vfs.vfs_data == NULL &&
                     mount.state == USFS_MOUNT_STALE,
           "forced unmount without vnodes completes immediately");
    tap_ok(&tap, abort_calls == 1 && conn.state == USFS_CONN_DEAD &&
                     lifecycle_release_calls == 1 &&
                     connection_release_calls == 1 && vfsrele_calls == 0 &&
                     forced_recovery_calls == 1 &&
                     deferred_recovery_calls == 0,
           "immediate forced unmount quarantines and releases the channel");

    reset_counters();
    memset(&conn, 0, sizeof(conn));
    memset(&mount, 0, sizeof(mount));
    memset(&vfs, 0, sizeof(vfs));
    conn.state = USFS_CONN_ACTIVE;
    mount.conn = &conn;
    mount.state = USFS_MOUNT_ACTIVE;
    initialize_node(&first, &first_vnode, &first_gnode, &vfs, 2);
    initialize_node(&second, &second_vnode, &second_gnode, &vfs, 3);
    first.next = &second;
    mount.nodes = &first;
    vfs.vfs_data = (caddr_t)&mount;

    tap_ok(&tap, usfs_unmount(&vfs, UVMNT_FORCE, NULL) == 0 &&
                     vfs.vfs_data == (caddr_t)&mount &&
                     mount.state == USFS_MOUNT_STALE,
           "forced unmount retains mount data while stale vnodes exist");
    tap_ok(&tap, abort_calls == 1 && lifecycle_release_calls == 1 &&
                     connection_release_calls == 0 &&
                     forced_recovery_calls == 1 &&
                     deferred_recovery_calls == 1,
           "forced recovery wakes requests but retains the mount reference");
    tap_ok(&tap, usfs_unmount(&vfs, UVMNT_FORCE, NULL) == EIO &&
                     abort_calls == 1 && lifecycle_release_calls == 1,
           "a stale mount cannot be force-released twice");

    tap_ok(&tap, gn_rele(&first_vnode) == 0 && mount.nodes == &second &&
                     vfs.vfs_data == (caddr_t)&mount,
           "non-final stale vnode release reclaims only that node");
    tap_ok(&tap, vnode_free_calls == 1 && gnode_free_calls == 1 &&
                     node_free_calls == 1 && mutation_lock_frees == 2 && connection_release_calls == 0 &&
                     vfsrele_calls == 0,
           "non-final release keeps mount and VFS ownership");

    tap_ok(&tap, gn_rele(&second_vnode) == 0 && mount.nodes == NULL &&
                     vfs.vfs_data == NULL,
           "final stale vnode release detaches retained mount data");
    tap_ok(&tap, vnode_free_calls == 2 && gnode_free_calls == 2 &&
                     node_free_calls == 3 && mutation_lock_frees == 5,
           "final release reclaims both nodes and the mount");
    tap_ok(&tap, connection_release_calls == 1 && vfsrele_calls == 1,
           "final stale release drops the connection and completes the VFS");
    tap_ok(&tap, lock_depth == 0 && stale_mount_release_calls == 1,
           "all ordinary and forced lifecycle paths release the global lock");

    {
        unsigned failure;
        const int errors[] = { 0, ENOMEM, EIO, EINVAL, ETIMEDOUT };
        for (failure = 0; failure < sizeof(errors) / sizeof(errors[0]); ++failure) {
            reset_counters();
            memset(&mount, 0, sizeof(mount));
            memset(&vfs, 0, sizeof(vfs));
            conn.state = USFS_CONN_ACTIVE; mount.conn = &conn;
            mount.state = USFS_MOUNT_ACTIVE; vfs.vfs_data = (caddr_t)&mount;
            initialize_node(&first, &first_vnode, &first_gnode, &vfs, 42);
            first.lookup_refs = 123; first_vnode.v_count = 2; mount.nodes = &first;
            forget_error = errors[failure];
            gn_rele(&first_vnode);
            if (forget_calls != 0 || node_free_calls != 0) abort();
            gn_rele(&first_vnode);
            tap_ok(&tap, forget_calls == 1 && forget_id == 42 && forget_count == 123 &&
                   lock_depth == 0 && connection_acquire_calls == 1 && connection_release_calls == 1 &&
                   node_free_calls == 1 && abort_calls == (failure != 0) &&
                   conn.state == (failure ? USFS_CONN_DEAD : USFS_CONN_ACTIVE),
                   "final vnode release acknowledges accumulated lookups outside locks and failed cleanup terminates the channel");
        }
        reset_counters(); conn.state = USFS_CONN_DEAD;
        tap_ok(&tap, usfs_forget(&conn, 42, 123) == 0 && forget_calls == 0 &&
               usfs_forget(&conn, 42, 0) == 0 && usfs_forget(NULL, 42, 1) == 0,
               "terminal connection cleanup needs no further exchange or allocation");
        reset_counters(); conn.state = USFS_CONN_ACTIVE;
        memset(&mount, 0, sizeof(mount)); memset(&vfs, 0, sizeof(vfs));
        mount.conn = &conn; mount.state = USFS_MOUNT_ACTIVE; vfs.vfs_data = (caddr_t)&mount;
        initialize_node(&first, &first_vnode, &first_gnode, &vfs, USFS_ROOT_ID);
        first.lookup_refs = 19; mount.nodes = mount.root = &first;
        tap_ok(&tap, usfs_unmount(&vfs, 0, NULL) == 0 && forget_calls == 1 &&
               forget_id == USFS_ROOT_ID && forget_count == 19 && lock_depth == 0 &&
               connection_release_calls == 1 && vfs.vfs_data == NULL && conn.state == USFS_CONN_CLOSED,
               "ordinary unmount releases anchored root lookup counts before connection ownership");
        reset_counters(); conn.state = USFS_CONN_ACTIVE; forget_error = EIO;
        memset(&mount, 0, sizeof(mount)); memset(&vfs, 0, sizeof(vfs));
        mount.conn = &conn; mount.state = USFS_MOUNT_ACTIVE; vfs.vfs_data = (caddr_t)&mount;
        initialize_node(&first, &first_vnode, &first_gnode, &vfs, USFS_ROOT_ID);
        first.lookup_refs = 19; mount.nodes = mount.root = &first;
        tap_ok(&tap, usfs_unmount(&vfs, 0, NULL) == 0 && forget_calls == 1 &&
               conn.state == USFS_CONN_DEAD && abort_calls == 1 && vfs.vfs_data == NULL &&
               lifecycle_release_calls == 1 && connection_release_calls == 1,
               "failed ordinary-unmount cleanup preserves the failed channel instead of reporting orderly EOF");
    }
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        reset_counters();
        memset(&mount, 0, sizeof(mount)); memset(&vfs, 0, sizeof(vfs));
        memset(&conn, 0, sizeof(conn)); conn.state = USFS_CONN_ACTIVE;
        mount.conn = &conn; mount.state = USFS_MOUNT_ACTIVE;
        vfs.vfs_data = (caddr_t)&mount;
        initialize_node(&first, &first_vnode, &first_gnode, &vfs, 2);
        initialize_node(&second, &second_vnode, &second_gnode, &vfs, 3);
        watched_mount = &mount; watched_vfs = &vfs;
        if (scenario < 4) {
            struct vnode *initial = scenario & 1 ? &second_vnode : &first_vnode;
            other_vnode = scenario & 1 ? &first_vnode : &second_vnode;
            mount.nodes = &first; first.next = &second;
            usfs_unmount(&vfs, UVMNT_FORCE, NULL);
            dispose_error = scenario >= 2;
            dispose_hook = release_other;
            gn_rele(initial);
        } else {
            mount.nodes = &first;
            if (scenario == 4) dispose_hook = force_during_disposal;
            if (scenario == 5) dispose_hook = ordinary_during_disposal;
            if (scenario == 6) {
                other_vnode = &first_vnode;
                abort_hook = release_other;
                usfs_unmount(&vfs, UVMNT_FORCE, NULL);
            } else {
                if (scenario == 7) {
                    first.lookup_refs = 1;
                    forget_hook = force_during_disposal;
                }
                gn_rele(&first_vnode);
                if (scenario == 5) usfs_unmount(&vfs, UVMNT_FORCE, NULL);
            }
        }
        const unsigned expected_nodes = scenario < 4 ? 2 : 1;
        tap_ok(&tap, mount_frees == 1 && vnode_free_calls == expected_nodes &&
               gnode_free_calls == expected_nodes && dispose_calls == expected_nodes &&
               node_free_calls == expected_nodes + 1 && mutation_lock_frees == 2 * expected_nodes + 1 &&
               connection_acquire_calls == (scenario == 7) &&
               connection_release_calls == 1 + (scenario == 7) &&
               mount.reclaiming_nodes == 0 && vfs.vfs_data == NULL && lock_depth == 0 &&
               lifecycle_release_calls == 1 && stale_mount_release_calls == (scenario != 5) &&
               vfsrele_calls == (scenario != 5),
               "interleaved reclamation retains mount through disposal, FORGET and unmount completion");
    }
    return tap_finish(&tap);
}
