/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

Complex_lock global_lock;

static struct usfs_node fixture_node;
static struct vnode fixture_vnode;
static unsigned create_calls;
static unsigned hold_calls;
static unsigned lock_depth;
static unsigned hold_lock_depth;
static unsigned create_lock_depth;

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

static int _create_node(struct vfs *vfsp, struct usfs_mount_data *mnt,
                        uint64_t nodeid, uint64_t parent, int vtype,
                        int is_root, struct usfs_node **node_out)
{
    (void)vfsp;
    (void)mnt;
    (void)nodeid;
    (void)parent;
    (void)vtype;
    (void)is_root;
    create_calls += 1;
    create_lock_depth = lock_depth;
    fixture_vnode.v_count = 1;
    fixture_node.vn = &fixture_vnode;
    *node_out = &fixture_node;
    return 0;
}

static void usfs_vnode_hold_locked(
    struct vnode *vnode, enum usfs_instrumentation_node_reuse source)
{
    (void)source;
    hold_calls += 1;
    hold_lock_depth = lock_depth;
    vnode->v_count += 1;
}

#include "fs/operations/vfs_root.h"

static void reset_fixture(void)
{
    memset(&fixture_node, 0, sizeof(fixture_node));
    memset(&fixture_vnode, 0, sizeof(fixture_vnode));
    create_calls = 0;
    hold_calls = 0;
    lock_depth = 0;
    hold_lock_depth = 0;
    create_lock_depth = 0;
}

int main(void)
{
    struct tap_state tap;
    struct usfs_mount_data mount;
    struct vfs vfs;
    struct vnode *result = NULL;

    tap_plan(&tap, 8);

    reset_fixture();
    memset(&mount, 0, sizeof(mount));
    memset(&vfs, 0, sizeof(vfs));
    mount.state = USFS_MOUNT_ACTIVE;
    fixture_node.vn = &fixture_vnode;
    fixture_vnode.v_count = 1;
    mount.root = &fixture_node;
    vfs.vfs_data = (caddr_t)&mount;
    tap_ok(&tap, usfs_root(&vfs, &result, NULL) == 0 &&
                     result == &fixture_vnode,
           "cached root is returned");
    tap_ok(&tap, hold_calls == 1 && fixture_vnode.v_count == 2,
           "cached root receives a caller reference");
    tap_ok(&tap, hold_lock_depth == 1,
           "cached root is captured and held under the global lock");
    tap_ok(&tap, lock_depth == 0 && create_calls == 0,
           "cached-root path releases the lock without creating a vnode");

    reset_fixture();
    memset(&mount, 0, sizeof(mount));
    mount.state = USFS_MOUNT_ACTIVE;
    vfs.vfs_data = (caddr_t)&mount;
    result = NULL;
    tap_ok(&tap, usfs_root(&vfs, &result, NULL) == 0 &&
                     result == &fixture_vnode &&
                     mount.root == &fixture_node,
           "missing root is created and published");
    tap_ok(&tap, create_calls == 1 && create_lock_depth == 1 &&
                     hold_calls == 0 && fixture_vnode.v_count == 1 &&
                     lock_depth == 0,
           "new root transfers its initial reference under the global lock");

    vfs.vfs_data = NULL;
    result = NULL;
    tap_ok(&tap, usfs_root(&vfs, &result, NULL) == EIO && result == NULL,
           "root lookup rejects a vfs without mount data");

    mount.state = USFS_MOUNT_STALE;
    vfs.vfs_data = (caddr_t)&mount;
    tap_ok(&tap, usfs_root(&vfs, &result, NULL) == EIO && result == NULL,
           "root lookup rejects retained data from a stale mount");

    return tap_finish(&tap);
}
