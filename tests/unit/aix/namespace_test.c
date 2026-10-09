/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

Complex_lock global_lock;
static struct usfs_mount_data mount_data;
static struct usfs_node parent_node, prepared_node;
static struct vnode directory_vnode, result_vnode;
static struct usfs_open_state open_state;
static struct usfs_request request_value;
static struct usfs_create_out reply_value;
static struct usfs_create_attr_in sent;
static int node_balance, state_balance, request_balance, callbacks, attached;
static int fail_node, fail_state, fail_request, backend_error, lock_depth, namespace_depth;
static int reply_fault, aborted;
void abort_connection(struct usfs_connection *connection) { (void)connection; ++aborted; }

void lock_write(complex_lock_t lock) { if (lock == &mount_data.namespace_lock) ++namespace_depth; else ++lock_depth; }
void lock_done(complex_lock_t lock) { if (lock == &mount_data.namespace_lock) --namespace_depth; else --lock_depth; }
int groupmember_cr(gid_t group, struct ucred *crp) { return group == crp->cr_gid; }
int privcheck_cr(int privilege, struct ucred *crp)
{ (void)privilege; return crp->cr_uid == 0 ? 0 : EPERM; }
static struct usfs_mount_data *_mount_of(struct vnode *vp)
{ (void)vp; return &mount_data; }
static struct usfs_node *_node_of(struct vnode *vp)
{ (void)vp; return &parent_node; }
int gn_access(struct vnode *vp, int32long64_t mode, int32long64_t who, struct ucred *crp)
{ (void)vp; (void)mode; (void)who; (void)crp; return 0; }
static int allocate_open_state(int32long64_t flags, struct usfs_open_state **state)
{
    if (fail_state) return ENOMEM;
    memset(&open_state, 0, sizeof(open_state));
    open_state.flags = flags; *state = &open_state; ++state_balance; return 0;
}
static void usfs_open_state_discard(struct usfs_open_state *state)
{ if (state != NULL) --state_balance; }
static void attach_open_state(struct usfs_node *node, struct usfs_open_state *state)
{ (void)node; (void)state; ++attached; }
static int _allocate_node(struct vfs *vfsp, int type, int root, struct usfs_node **node)
{
    (void)vfsp; (void)type; (void)root;
    if (fail_node) return ENOMEM;
    *node = &prepared_node; ++node_balance; return 0;
}
static void _discard_unpublished_node(struct usfs_node *node)
{ if (node != NULL) --node_balance; }
static int commit_created_node(struct usfs_mount_data *mount,
    const struct usfs_node *dir,
    const struct usfs_create_out *out, struct usfs_node *prepared, struct usfs_node **node)
{
    (void)mount; (void)dir;
    prepared->vn = &result_vnode; prepared->nodeid = out->nodeid; *node = prepared;
    return 0;
}
static int usfs_forget(struct usfs_connection *conn, uint64_t id, uint64_t count)
{ (void)conn; (void)id; (void)count; return 0; }
static void _send_release(struct usfs_mount_data *mount, const uint64_t id, const uint64_t fh,
                         const int32long64_t flags, const uint32_t isdir, struct ucred *crp)
{ (void)mount; (void)id; (void)fh; (void)flags; (void)isdir; (void)crp; }
int allocate_request(const struct usfs_request_allocation_spec *spec, struct usfs_request **request)
{
    if (fail_request) return ENOMEM;
    if (spec->opcode != USFS_OP_CREATE_ATTR || lock_depth != 0 || namespace_depth != 1) return EIO;
    memcpy(&sent, spec->opcode_specific_body.bytes, sizeof(sent));
    *request = &request_value; ++request_balance; return 0;
}
void free_request(struct usfs_request *request) { (void)request; --request_balance; }
int usfs_call(struct usfs_connection *conn, struct usfs_request *request)
{
    (void)conn;
    ++callbacks;
    memset(&reply_value, 0, sizeof(reply_value));
    reply_value.nodeid = sent.activation == USFS_CREATE_DEFAULT ? 0 : 42;
    reply_value.fh = sent.activation == USFS_CREATE_OPEN ? 77 : 0;
    reply_value.attr.mode = S_IFREG | 0600;
    reply_value.attr.nlink = 1;
    reply_value.attr.ino = 42;
    reply_value.attr.blksize = 4096;
    request->error = backend_error;
    request->reply_buffer = (char *)&reply_value;
    request->reply_buffer_size = sizeof(reply_value);
    if (reply_fault == 1) --request->reply_buffer_size;
    if (reply_fault == 2) reply_value.attr.pad = 1;
    return 0;
}

#include "fs/operations/vnode_create_attr.h"

static void test_create_entry(struct tap_state *tap)
{
    unsigned scenario;
    for (scenario = 0; scenario < 18; ++scenario) {
        struct vnode *result = NULL;
        caddr_t info = NULL;
        struct ucred credential = { 0 };
        struct vattr attr = { 0 };
        int activation = scenario < 3 ? (int)scenario : VC_OPEN;
        int expected = 0, rc, valid;
        attr.va_mask = AT_TYPE | AT_MODE;
        attr.va_type = VREG; attr.va_mode = 0600;
        memset(&sent, 0, sizeof(sent));
        mount_data.writable = 1;
        parent_node.nodeid = 1;
        node_balance = state_balance = request_balance = callbacks = attached = 0;
        fail_node = fail_state = fail_request = backend_error = 0;
        reply_fault = aborted = 0;
        switch (scenario) {
        case 3: attr.va_mask |= AT_SIZE; attr.va_size = -1; expected = EINVAL; break;
        case 4: attr.va_mask |= AT_ATIME; attr.va_atime.tv_nsec = 1000000000L; expected = EINVAL; break;
        case 5: attr.va_mask |= AT_MTIME; attr.va_mtime.tv_nsec = -1; expected = EINVAL; break;
        case 6: attr.va_mask |= AT_NLINK; expected = EOPNOTSUPP; break;
        case 7: fail_state = 1; expected = ENOMEM; break;
        case 8: fail_node = 1; expected = ENOMEM; break;
        case 9: fail_request = 1; expected = ENOMEM; break;
        case 10: backend_error = EOPNOTSUPP; expected = EOPNOTSUPP; break;
        case 11: attr.va_mask |= AT_UID; attr.va_uid = 1002; credential.cr_uid = 1001; expected = EPERM; break;
        case 12:
            attr.va_mask |= AT_UID | AT_GID | AT_SIZE | AT_ATIME | AT_MTIME | AT_CTIME;
            attr.va_uid = 1001; attr.va_gid = 1002; attr.va_size = 17;
            attr.va_atime.tv_sec = 11; attr.va_atime.tv_nsec = 12;
            attr.va_mtime.tv_sec = 21; attr.va_mtime.tv_nsec = 22;
            attr.va_ctime.tv_sec = 31; attr.va_ctime.tv_nsec = 32;
            break;
        case 13: attr.va_mask |= AT_EXT; attr.va_ext = 0; break;
        case 14: attr.va_mask |= AT_EXT; attr.va_ext = 1; expected = EOPNOTSUPP; break;
        case 15: attr.va_mask |= (long)1 << 40; expected = EOPNOTSUPP; break;
        case 16: reply_fault = 1; expected = EIO; break;
        case 17: reply_fault = 2; expected = EIO; break;
        }
        rc = gn_create_attr(&directory_vnode, &result, FREAD | FWRITE, "new", &attr,
                            activation, &info, &credential);
        valid = rc == expected && request_balance == 0 && lock_depth == 0 && namespace_depth == 0 &&
                aborted == (scenario >= 16);
        if (expected != 0)
            valid &= result == NULL && info == NULL && node_balance == 0 && state_balance == 0 &&
                     callbacks == (scenario == 10 || scenario >= 16 ? 1 : 0);
        else {
            valid &= callbacks == 1 && node_balance == (activation != VC_DEFAULT) &&
                     state_balance == (activation == VC_OPEN) && attached == (activation == VC_OPEN) &&
                     (result != NULL) == (activation != VC_DEFAULT) &&
                     (info != NULL) == (activation == VC_OPEN) && sent.activation == (uint32_t)activation;
            if (activation == VC_OPEN) valid &= open_state.fh == 77;
            if (scenario == 12) valid &= sent.attr.valid == 127 && sent.attr.size == 17 &&
                sent.attr.uid == 1001 && sent.attr.gid == 1002 && sent.attr.atimensec == 12 &&
                sent.attr.mtimensec == 22 && sent.attr.ctimensec == 32;
        }
        tap_ok(tap, valid, expected == 0 ? "real CREATE_ATTR entry honors activation and initial attributes without temporary opens" :
               "real CREATE_ATTR entry rejects invalid attributes or preparation failure before publication and releases reservations");
    }
}

static struct vattr sticky_parent, sticky_target;
static int sticky_lookup_error, sticky_releases;
int gn_getattr(struct vnode *vp, struct vattr *attr, struct ucred *crp)
{ (void)vp; (void)crp; *attr = sticky_parent; return namespace_depth == 1 ? 0 : EIO; }
int gn_lookup(struct vnode *vp, struct vnode **out, char *name, int32long64_t flags,
              struct vattr *attr, struct ucred *crp)
{
    (void)vp; (void)name; (void)flags; (void)crp;
    if (sticky_lookup_error) return sticky_lookup_error;
    *attr = sticky_target; *out = &result_vnode; return 0;
}
int gn_rele(struct vnode *vp) { (void)vp; ++sticky_releases; return 0; }
#include "fs/namespace_security.h"

static void test_sticky(struct tap_state *tap)
{
    struct ucred cr;
    int scenario;
    const int expected[] = { EPERM, 0, 0, 0, EIO, 0, ENOENT };
    memset(&cr, 0, sizeof(cr));
    for (scenario = 0; scenario < 7; ++scenario) {
        int rc;
        cr.cr_uid = scenario == 3 ? 0 : 1001;
        memset(&sticky_parent, 0, sizeof(sticky_parent));
        memset(&sticky_target, 0, sizeof(sticky_target));
        sticky_parent.va_mode = S_IFDIR | 01777;
        sticky_parent.va_uid = scenario == 1 ? 1001 : 2001;
        sticky_target.va_uid = scenario == 2 ? 1001 : 2002;
        sticky_lookup_error = scenario == 4 ? EIO : scenario >= 5 ? ENOENT : 0;
        sticky_releases = 0;
        write_synchronized_with (mount_data.namespace_lock) {
            rc = usfs_check_sticky_entry(&directory_vnode, "current-name", &cr, scenario == 5);
        }
        tap_ok(tap, rc == expected[scenario] && namespace_depth == 0 &&
               sticky_releases == (scenario == 0 || scenario == 2),
               "sticky authorization uses current owner, privileges and checked lookup errors");
    }
}

int main(void)
{
    struct tap_state tap;
    struct vnode *result = NULL;
    struct vattr attributes;
    struct ucred credential;

    tap_plan(&tap, 34);
    memset(&attributes, 0, sizeof(attributes));
    memset(&credential, 0, sizeof(credential));
    attributes.va_mask = AT_TYPE | AT_MODE;
    attributes.va_type = VREG;
    tap_ok(&tap, !usfs_create_attr_arguments_valid(
                     NULL, &attributes, VC_OPEN, &credential),
           "create_attr rejects null result storage");
    tap_ok(&tap, !usfs_create_attr_arguments_valid(
                     &result, NULL, VC_OPEN, &credential),
           "create_attr rejects null attributes");
    tap_ok(&tap, !usfs_create_attr_arguments_valid(
                     &result, &attributes, VC_OPEN, NULL),
           "create_attr rejects null credentials");
    attributes.va_mask = AT_TYPE;
    tap_ok(&tap, !usfs_create_attr_arguments_valid(
                     &result, &attributes, VC_OPEN, &credential),
           "create_attr requires mode and type attributes");
    attributes.va_mask = AT_TYPE | AT_MODE;
    attributes.va_type = VDIR;
    tap_ok(&tap, !usfs_create_attr_arguments_valid(
                     &result, &attributes, VC_OPEN, &credential),
           "create_attr rejects nonregular objects");
    attributes.va_type = VREG;
    tap_ok(&tap, !usfs_create_attr_arguments_valid(
                     &result, &attributes, 999, &credential),
           "create_attr rejects unknown activation control");
    tap_ok(&tap, usfs_create_attr_arguments_valid(
                     &result, &attributes, VC_OPEN, &credential),
           "create_attr accepts open activation");
    tap_ok(&tap, usfs_create_attr_arguments_valid(
                     &result, &attributes, VC_LOOKUP, &credential),
           "create_attr accepts lookup activation");
    tap_ok(&tap, usfs_create_attr_arguments_valid(
                     &result, &attributes, VC_DEFAULT, &credential),
           "create_attr accepts default activation");
    test_create_entry(&tap);
    test_sticky(&tap);
    return tap_finish(&tap);
}
