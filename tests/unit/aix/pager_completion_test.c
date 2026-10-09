/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include <string.h>

static struct usfs_node node;
static struct vnode vnode;
static struct gnode gnode;
static struct usfs_mount_data mount_data;
static struct usfs_connection connection;
static struct usfs_open_state handle;
static struct usfs_request request;
static struct usfs_write_out write_reply;
static char page[PAGESIZE], reply[PAGESIZE];
static unsigned completed, released, premature, context_depth, lock_depth;
static unsigned expected_completions, error_completions, dropped_external;
static int backend_error, sticky_error;
static uint16_t opcode;
static uint64_t request_offset;
static uint32_t request_size;

void lock_write(complex_lock_t lock) { (void)lock; ++lock_depth; }
void lock_done(complex_lock_t lock) { (void)lock; --lock_depth; }
static struct usfs_node *_node_of(struct vnode *vp) { (void)vp; return &node; }
static struct usfs_mount_data *_mount_of(struct vnode *vp) { (void)vp; return &mount_data; }
static uint64_t get_cache_size(struct usfs_node *n) { return n->cache_size; }
static void record_writeback_error(struct usfs_node *n, int error) {
    (void)n; if (error && !sticky_error) sticky_error = error;
}
static int borrow_cache_handle(struct usfs_node *n, int writing,
                                     struct usfs_open_state **out) {
    (void)n; (void)writing; ++vnode.v_count; ++handle.refs; ++handle.operation_refs;
    *out = &handle; return 0;
}
static void usfs_put_open_reference(struct vnode *virtual_fs_node, struct usfs_open_state *state,
                                    int operation, struct ucred *credential) {
    (void)credential;
    if (!operation || context_depth != 1 || lock_depth) ++premature;
    if (state == NULL) return;
    /* Model the final-reclaim wait: every VMM buffer submitted in this
     * strategy invocation must already have completion queued. */
    if (virtual_fs_node->v_count == 1 && completed != expected_completions) ++premature;
    --virtual_fs_node->v_count; --state->refs; --state->operation_refs; ++released;
}
int allocate_request(const struct usfs_request_allocation_spec *spec,
                     struct usfs_request **out) {
    memset(&request, 0, sizeof(request)); opcode = spec->opcode;
    if (opcode == USFS_OP_WRITE) {
        const struct usfs_write_in *body = spec->opcode_specific_body.bytes;
        request_offset = body->offset; request_size = body->size;
    } else {
        const struct usfs_read_in *body = spec->opcode_specific_body.bytes;
        request_offset = body->offset; request_size = body->size;
    }
    *out = &request; return 0;
}
int usfs_call_with_specific_request_class(struct usfs_connection *conn,
    struct usfs_request *req, enum usfs_request_class request_class) {
    (void)conn;
    if (request_class != USFS_REQUEST_PAGER || context_depth != 1) ++premature;
    if (!dropped_external) { --vnode.v_count; dropped_external = 1; }
    req->error = backend_error;
    if (opcode == USFS_OP_WRITE) {
        memset(&write_reply, 0, sizeof(write_reply));
        write_reply.written = request_size; write_reply.offset = request_offset;
        req->reply_buffer = (char *)&write_reply; req->reply_buffer_size = sizeof(write_reply);
    } else {
        req->reply_buffer = reply; req->reply_buffer_size = request_size;
    }
    return 0;
}
void free_request(struct usfs_request *req) { (void)req; }
caddr_t vm_att(vmhandle_t handle_id, caddr_t address) {
    (void)handle_id; (void)address;
    if (context_depth != 1) ++premature;
    return page;
}
void vm_det(caddr_t address) { (void)address; }
void vm_thrpgio_push(ut_pgio_context_t *context) { (void)context; ++context_depth; }
void vm_thrpgio_pop(ut_pgio_context_t *context) { (void)context; --context_depth; }
void iodone(struct buf *bp) {
    if (context_depth || lock_depth) ++premature;
    if (bp->b_flags & B_ERROR) ++error_completions;
    ++completed;
    /* AIX forbids touching the buffer after iodone: poison even av_forw. */
    memset(bp, 0xa5, sizeof(*bp));
}
int vm_mounte(int flags, dev_t device, struct thrpginfo *info) {
    (void)flags; (void)device; (void)info; return 0;
}
int vm_umount(int flags, dev_t device) { (void)flags; (void)device; return 0; }
#include "fs/pager.h"

static struct buf buffers[USFS_PAGER_BUFFER_COUNT + 1];
static void reset(unsigned count, int reading) {
    memset(&node, 0, sizeof(node)); memset(&vnode, 0, sizeof(vnode));
    memset(&gnode, 0, sizeof(gnode)); memset(&handle, 0, sizeof(handle));
    memset(&mount_data, 0, sizeof(mount_data)); memset(buffers, 0, sizeof(buffers));
    node.vn = &vnode; node.gn = &gnode; node.nodeid = 2; node.cache_size = PAGESIZE;
    vnode.v_gnode = &gnode; vnode.v_count = 1; gnode.gn_vnode = &vnode;
    mount_data.conn = &connection; mount_data.writable = 1; handle.refs = 1;
    completed = released = premature = context_depth = lock_depth = 0;
    dropped_external = error_completions = 0; backend_error = sticky_error = 0;
    expected_completions = count;
    for (unsigned i = 0; i < count; ++i) {
        buffers[i].b_vp = (struct vnode *)&gnode; buffers[i].b_bcount = PAGESIZE;
        buffers[i].b_flags = reading ? B_READ : 0;
        buffers[i].av_forw = i + 1 < count ? &buffers[i + 1] : NULL;
    }
}
int main(void) {
    struct tap_state tap;
    tap_plan(&tap, 5);
    reset(1, 1); usfs_pager_strategy(buffers, 0, 0);
    tap_ok(&tap, completed == 1 && released == 1 && vnode.v_count == 0 &&
           handle.refs == 1 && !handle.operation_refs && !premature && !context_depth,
           "last page-in reference is released only after its buffer completes");
    reset(2, 0); usfs_pager_strategy(buffers, 0, 0);
    tap_ok(&tap, completed == 2 && released == 2 && vnode.v_count == 0 && !premature,
           "chained pageout retains references until every buffer completes");
    reset(2, 0); backend_error = EIO; usfs_pager_strategy(buffers, 0, 0);
    tap_ok(&tap, completed == 2 && released == 2 && error_completions == 2 &&
           sticky_error == EIO && vnode.v_count == 0 && !premature,
           "failed pageouts preserve errors and complete before final reclamation");
    reset(USFS_PAGER_BUFFER_COUNT + 1, 0); usfs_pager_strategy(buffers, 0, 0);
    tap_ok(&tap, completed == USFS_PAGER_BUFFER_COUNT + 1 &&
           released == USFS_PAGER_BUFFER_COUNT && error_completions == 1 && !premature,
           "out-of-contract chain overflow completes with error without unbounded storage");
    reset(1, 1); buffers[0].b_vp = NULL; usfs_pager_strategy(buffers, 0, 0);
    tap_ok(&tap, completed == 1 && !released && error_completions == 1 &&
           vnode.v_count == 1 && !premature && !context_depth,
           "invalid pager input completes without releasing an unowned reference");
    return tap_finish(&tap);
}
