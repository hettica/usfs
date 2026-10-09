/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include "fs/fid_format.h"

Complex_lock global_lock;

static struct usfs_connection connection;
static struct usfs_mount_data mount_data;
static struct vfs file_system;
static struct gnode existing_gnode;
static struct vnode existing_vnode;
static struct usfs_node existing_node;
static struct vnode created_vnode;
static struct gnode created_gnode;
static struct usfs_node created_node;
static struct usfs_request request;
static union
{
    struct usfs_in_hdr alignment;
    char bytes[sizeof (struct usfs_in_hdr) + sizeof (struct usfs_vget_in)];
} request_storage;
static union
{
    struct usfs_vget_out alignment;
    char bytes[sizeof (struct usfs_vget_out)];
} reply_storage;
static struct usfs_fid_out fid_reply;
static struct usfs_vget_out vget_reply;
static int transport_error;
static int allocation_error;
static int daemon_error;
static int create_error;
static int stale_during_call;
static unsigned abort_calls;
static unsigned forget_calls;
static unsigned free_request_calls;
static unsigned create_calls;
static unsigned hold_calls;
static unsigned vfs_hold_calls;
static unsigned vfs_unhold_calls;
static uint64_t forgotten_node_id;
static uint64_t requested_token;
static uint16_t requested_opcode;

void lock_write (complex_lock_t lock)
{
    (void)lock;
}

void lock_read (complex_lock_t lock)
{
    (void)lock;
}

void lock_done (complex_lock_t lock)
{
    (void)lock;
}

void simple_lock (simple_lock_t lock)
{
    (void)lock;
}

void simple_unlock (simple_lock_t lock)
{
    (void)lock;
}

void vfs_hold (struct vfs * held_file_system)
{
    (void)held_file_system;
    vfs_hold_calls++;
}

int vfs_unhold (struct vfs * held_file_system)
{
    (void)held_file_system;
    vfs_unhold_calls++;
    return 1;
}

static struct usfs_mount_data * claim_stale_mount (struct vfs * candidate)
{
    (void)candidate;
    return NULL;
}

static struct usfs_mount_data * _mount_of (struct vnode * file_vnode)
{
    return (struct usfs_mount_data *)file_vnode->v_vfsp->vfs_data;
}

static struct usfs_node * _node_of (struct vnode * file_vnode)
{
    return (struct usfs_node *)file_vnode->v_gnode->gn_data;
}

static void finish_stale_mount (struct vfs * candidate, struct usfs_mount_data * stale_mount)
{
    (void)candidate;
    (void)stale_mount;
}

static int usfs_forget (struct usfs_connection * owner, const uint64_t node_id, const uint64_t lookup_count)
{
    (void)owner;
    forgotten_node_id = node_id;
    forget_calls += (unsigned)lookup_count;
    return 0;
}

static void usfs_vnode_hold_locked (struct vnode * file_vnode, const enum usfs_instrumentation_node_reuse source)
{
    (void)source;
    file_vnode->v_count++;
    hold_calls++;
}

static int _create_node (
    struct vfs * destination,
    struct usfs_mount_data * destination_mount,
    const uint64_t node_id,
    const uint64_t parent_id,
    const int vnode_type,
    const int is_root,
    struct usfs_node ** result
)
{
    (void)destination;
    (void)parent_id;
    create_calls++;

    if (create_error != 0)
        return create_error;

    created_node.nodeid = node_id;
    created_node.vn = &created_vnode;
    created_node.next = destination_mount->nodes;
    destination_mount->nodes = &created_node;
    created_vnode.v_gnode = &created_gnode;
    created_vnode.v_vfsp = destination;
    created_gnode.gn_data = (caddr_t)&created_node;
    created_vnode.v_vntype = vnode_type;
    created_vnode.v_count = 1;

    if (is_root)
        destination_mount->root = &created_node;

    *result = &created_node;
    return 0;
}

int usfs_vtype_from_mode (const uint32_t mode)
{
    return (mode & S_IFMT) == S_IFDIR ? VDIR : VREG;
}

int allocate_request (const struct usfs_request_allocation_spec * specification, struct usfs_request ** output)
{
    if (allocation_error != 0)
        return allocation_error;

    memset (&request, 0, sizeof (request));
    memset (&request_storage, 0, sizeof (request_storage));
    memset (&reply_storage, 0, sizeof (reply_storage));

    struct usfs_in_hdr * header = (struct usfs_in_hdr *)request_storage.bytes;
    header->opcode = specification->opcode;
    header->nodeid = specification->node_id;
    requested_opcode = specification->opcode;

    request.request_buffer = request_storage.bytes;
    request.request_buffer_size = sizeof (*header) + specification->opcode_specific_body.length;
    request.reply_buffer = reply_storage.bytes;

    if (specification->opcode_specific_body.length != 0)
    {
        memcpy (request_storage.bytes + sizeof (*header), specification->opcode_specific_body.bytes, specification->opcode_specific_body.length);
        requested_token = ((const struct usfs_vget_in *)specification->opcode_specific_body.bytes)->token;
    }

    *output = &request;
    return 0;
}

void free_request (struct usfs_request * completed)
{
    (void)completed;
    free_request_calls++;
}

int usfs_call (struct usfs_connection * owner, struct usfs_request * pending)
{
    (void)owner;

    if (stale_during_call)
        mount_data.state = USFS_MOUNT_STALE;

    if (transport_error != 0)
        return transport_error;

    pending->error = daemon_error;
    if (daemon_error != 0)
        return 0;

    if (requested_opcode == USFS_OP_FID)
    {
        memcpy (pending->reply_buffer, &fid_reply, sizeof (fid_reply));
        pending->reply_buffer_size = sizeof (fid_reply);
    }
    else
    {
        memcpy (pending->reply_buffer, &vget_reply, sizeof (vget_reply));
        pending->reply_buffer_size = sizeof (vget_reply);
    }

    return 0;
}

void abort_connection (struct usfs_connection * owner)
{
    owner->state = USFS_CONN_DEAD;
    abort_calls++;
}

int usfs_checkpoint (const enum usfs_checkpoint checkpoint)
{
    (void)checkpoint;
    return 0;
}

#include "fs/operations/vfs_vget.h"
#include "fs/operations/vnode_fid.h"

static void reset_fixture (void)
{
    memset (&connection, 0, sizeof (connection));
    memset (&mount_data, 0, sizeof (mount_data));
    memset (&file_system, 0, sizeof (file_system));
    memset (&existing_gnode, 0, sizeof (existing_gnode));
    memset (&existing_vnode, 0, sizeof (existing_vnode));
    memset (&existing_node, 0, sizeof (existing_node));
    memset (&created_vnode, 0, sizeof (created_vnode));
    memset (&created_gnode, 0, sizeof (created_gnode));
    memset (&created_node, 0, sizeof (created_node));
    memset (&fid_reply, 0, sizeof (fid_reply));
    memset (&vget_reply, 0, sizeof (vget_reply));

    connection.state = USFS_CONN_ACTIVE;
    mount_data.conn = &connection;
    mount_data.state = USFS_MOUNT_ACTIVE;
    mount_data.nodes = &existing_node;
    file_system.vfs_data = (caddr_t)&mount_data;
    file_system.vfs_number = 77;
    existing_node.nodeid = 2;
    existing_node.vn = &existing_vnode;
    existing_vnode.v_vfsp = &file_system;
    existing_vnode.v_gnode = &existing_gnode;
    existing_vnode.v_vntype = VREG;
    existing_vnode.v_count = 1;
    existing_gnode.gn_data = (caddr_t)&existing_node;
    fid_reply.token = UINT64_C (0x100000007);
    vget_reply.token = fid_reply.token;
    vget_reply.entry.nodeid = existing_node.nodeid;
    vget_reply.entry.attr.ino = 2;
    vget_reply.entry.attr.mode = S_IFREG | 0600;
    vget_reply.entry.attr.nlink = 1;
    vget_reply.entry.attr.blksize = 4096;

    transport_error = 0;
    allocation_error = 0;
    daemon_error = 0;
    create_error = 0;
    stale_during_call = false;
    abort_calls = 0;
    forget_calls = 0;
    free_request_calls = 0;
    create_calls = 0;
    hold_calls = 0;
    vfs_hold_calls = 0;
    vfs_unhold_calls = 0;
    forgotten_node_id = 0;
    requested_token = 0;
    requested_opcode = 0;
}

static void test_fid_operation (struct tap_state * state)
{
    struct fileid file_id;
    uint64_t decoded_token = 0;

    reset_fixture ();
    memset (&file_id, 0, sizeof (file_id));
    tap_ok (
        state,
        gn_fid (&existing_vnode, &file_id, NULL) == 0 && requested_opcode == USFS_OP_FID,
        "fid asks the daemon for the vnode's backend identity"
    );
    tap_ok (
        state,
        decode_file_identifier (&file_id, file_system.vfs_number, &decoded_token) == 0 && decoded_token == fid_reply.token &&
            existing_node.fid_token == fid_reply.token,
        "fid records a full-width stable identity and mount serial"
    );

    daemon_error = EOPNOTSUPP;
    tap_ok (state, gn_fid (&existing_vnode, &file_id, NULL) == EOPNOTSUPP, "backend without export capability reports EOPNOTSUPP");

    reset_fixture ();
    fid_reply.token = 0;
    tap_ok (state, gn_fid (&existing_vnode, &file_id, NULL) == EIO && abort_calls == 1, "zero daemon file identity terminates the connection");

    reset_fixture ();
    transport_error = EIO;
    connection.state = USFS_CONN_DEAD;
    tap_ok (state, gn_fid (&existing_vnode, &file_id, NULL) == ESTALE, "lost daemon makes fid stale");

    reset_fixture ();
    transport_error = ETIMEDOUT;
    connection.state = USFS_CONN_DEAD;
    tap_ok (state, gn_fid (&existing_vnode, &file_id, NULL) == ETIMEDOUT, "fid preserves the original timeout error");

    reset_fixture ();
    allocation_error = ENOMEM;
    tap_ok (
        state,
        gn_fid (&existing_vnode, &file_id, NULL) == ENOMEM && requested_opcode == 0,
        "fid propagates request allocation failure without entering transport"
    );
}

static void test_vget_operation (struct tap_state * state)
{
    struct fileid file_id = { 0 };
    struct vnode * result = NULL;

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    tap_ok (state, usfs_vget (&file_system, &result, &file_id, NULL) == 0 && result == &existing_vnode, "vget returns the canonical live vnode");
    tap_ok (
        state,
        existing_vnode.v_count == 2 && existing_node.lookup_refs == 1 && hold_calls == 1 && forget_calls == 0 && vfs_hold_calls == 1 &&
            vfs_unhold_calls == 1 && mount_data.inflight_vgets == 0 && requested_opcode == USFS_OP_VGET && requested_token == fid_reply.token &&
            free_request_calls == 1,
        "vget transfers exactly one lookup and caller vnode reference"
    );

    reset_fixture ();
    mount_data.nodes = NULL;
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == 0 && result == &created_vnode && create_calls == 1 && created_node.lookup_refs == 1 &&
            created_node.fid_token == fid_reply.token,
        "vget rebuilds a vnode after cache eviction"
    );

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number + 1u, fid_reply.token);
    result = &existing_vnode;
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == ESTALE && result == NULL && requested_opcode == 0,
        "fid from another mount is rejected before transport"
    );

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    daemon_error = EOPNOTSUPP;
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == EOPNOTSUPP && result == NULL,
        "backend without resolver capability reports EOPNOTSUPP"
    );

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    daemon_error = ENOENT;
    tap_ok (state, usfs_vget (&file_system, &result, &file_id, NULL) == ESTALE && result == NULL, "deleted backend identity is stale");

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    transport_error = ETIMEDOUT;
    connection.state = USFS_CONN_DEAD;
    tap_ok (state, usfs_vget (&file_system, &result, &file_id, NULL) == ETIMEDOUT, "vget preserves timeout after connection loss");

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    allocation_error = ENOMEM;
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == ENOMEM && mount_data.inflight_vgets == 0 && vfs_hold_calls == 1 &&
            vfs_unhold_calls == 1 && requested_opcode == 0,
        "vget releases its mount reference after request allocation failure"
    );

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    stale_during_call = true;
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == ESTALE && result == NULL && forget_calls == 1 &&
            forgotten_node_id == existing_node.nodeid && mount_data.inflight_vgets == 0,
        "unmount after reply rejects vnode publication and forgets ownership"
    );

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    create_error = ENOMEM;
    mount_data.nodes = NULL;
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == ENOMEM && forget_calls == 1,
        "vnode creation failure forgets transferred lookup ownership"
    );

    reset_fixture ();
    encode_file_identifier (&file_id, file_system.vfs_number, fid_reply.token);
    existing_node.fid_token = fid_reply.token + 1u;
    tap_ok (
        state,
        usfs_vget (&file_system, &result, &file_id, NULL) == EIO && abort_calls == 1 && forget_calls == 1,
        "conflicting live-vnode identity quarantines the daemon and forgets ownership"
    );
}

int main (void)
{
    struct tap_state state;

    tap_plan (&state, 18);
    test_fid_operation (&state);
    test_vget_operation (&state);

    return tap_finish (&state);
}
