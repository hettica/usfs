/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */
#include "tap.h"
#include "support/fake_kernel.h"

#include <string.h>

enum
{
    TEST_SNAPSHOT_ID = 7,
    TEST_DIRECTORY_NODE_ID = 19,
    TEST_FIRST_HANDLE = 101,
    TEST_SECOND_HANDLE = 202
};

static struct usfs_mount_data test_mount;
static struct usfs_node test_node;
static struct usfs_request test_request;
static struct usfs_readdir_out test_reply = { .snapshot_id = TEST_SNAPSHOT_ID };
static struct usfs_readdir_in observed_request;
static uint64_t observed_nodeid;
static unsigned request_count;
static unsigned release_count;

static struct usfs_mount_data * _mount_of (struct vnode * vnode)
{
    (void)vnode;
    return &test_mount;
}

static struct usfs_node * _node_of (struct vnode * vnode)
{
    (void)vnode;
    return &test_node;
}

int gn_access (struct vnode * vnode, int32long64_t access, int32long64_t subject, struct ucred * credentials)
{
    (void)vnode;
    (void)access;
    (void)subject;
    (void)credentials;
    return 0;
}

int allocate_request (const struct usfs_request_allocation_spec * spec, struct usfs_request ** request)
{
    observed_nodeid = spec->node_id;
    memcpy (&observed_request, spec->opcode_specific_body.bytes, sizeof (observed_request));
    memset (&test_request, 0, sizeof (test_request));
    test_request.reply_buffer = (char *)&test_reply;
    test_request.reply_buffer_size = sizeof (test_reply);
    request_count++;
    *request = &test_request;
    return 0;
}

void free_request (struct usfs_request * request)
{
    (void)request;
    release_count++;
}

int usfs_call (struct usfs_connection * connection, struct usfs_request * request)
{
    (void)connection;
    (void)request;
    return 0;
}

int uiomove (caddr_t data, int32long64_t count, enum uio_rw direction, struct uio * request)
{
    (void)data;
    (void)count;
    (void)direction;
    (void)request;
    return EIO;
}

#include "fs/operations/vnode_readdir.h"
#include "fs/operations/vnode_readdir_eofp.h"

static void test_directory_request (struct tap_state * tap, const int vector_count)
{
    char data[sizeof (struct dirent)] = { 0 };
    struct iovec vectors[2] = { 0 };
    struct uio user_io = { 0 };
    struct gnode gnode = { 0 };
    struct vnode vnode = { 0 };
    struct ucred credentials = { 0 };
    struct usfs_open_state first_open = { .fh = TEST_FIRST_HANDLE, .active = 1 };
    struct usfs_open_state second_open = { .fh = TEST_SECOND_HANDLE, .active = 1 };

    first_open.next = &second_open;
    test_node.opens = &first_open;
    test_node.nodeid = TEST_DIRECTORY_NODE_ID;
    gnode.gn_data = (caddr_t)&test_node;
    vnode.v_gnode = &gnode;
    vectors[0].iov_base = data;
    vectors[0].iov_len = sizeof (data);
    vectors[1].iov_base = (caddr_t)1;
    vectors[1].iov_len = 0;
    user_io.uio_iov = vectors;
    user_io.uio_iovcnt = vector_count;
    user_io.uio_resid = sizeof (data);

    int end_of_directory = 0;
    const int rc = gn_readdir_eofp (&vnode, &user_io, &end_of_directory, &credentials);

    tap_ok (
        tap,
        rc == 0 && end_of_directory == 1 && observed_nodeid == TEST_DIRECTORY_NODE_ID && observed_request.fh == 0 && observed_request.cookie == 0 &&
            user_io.uio_offset == (offset_t)(TEST_SNAPSHOT_ID << USFS_DIRECTORY_CURSOR_INDEX_BITS),
        vector_count == 1 ? "one-vector readdir identifies the directory without an open handle"
                          : "two-vector readdir ignores the side channel and still sends object identity"
    );
}

int main (void)
{
    struct tap_state tap;
    tap_plan (&tap, 3);

    test_directory_request (&tap, 1);
    test_directory_request (&tap, 2);
    tap_ok (&tap, request_count == 2 && release_count == 2, "both object requests release transport ownership");

    return tap_finish (&tap);
}
