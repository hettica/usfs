/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "support/fake_kernel.h"

#include <string.h>

static struct usfs_mount_data fake_mount;
Complex_lock global_lock;
static unsigned namespace_depth;
void lock_read(complex_lock_t lock) { (void)lock; ++namespace_depth; }
void lock_write(complex_lock_t lock) { (void)lock; ++namespace_depth; }
void lock_done(complex_lock_t lock) { (void)lock; --namespace_depth; }
static int fake_mount_available;
static int real_getattr;
static struct usfs_node attribute_node;
static struct usfs_request attribute_request;
static struct usfs_attr_out attribute_reply;
static int allocation_error, transport_error, request_balance;
static int handle_available, operation_balance, close_during_getattr, releases;
static uint64_t getattr_handle, setattr_handle;

static struct usfs_node *_node_of(struct vnode *vp)
{
    (void)vp;
    return &attribute_node;
}


int allocate_request(const struct usfs_request_allocation_spec *spec,
                      struct usfs_request **request)
{
    getattr_handle = ((const struct usfs_getattr_in *)spec->opcode_specific_body.bytes)->fh;
    if (allocation_error) return allocation_error;
    ++request_balance;
    *request = &attribute_request;
    return 0;
}

void free_request(struct usfs_request *request)
{
    (void)request;
    --request_balance;
}

int usfs_call(struct usfs_connection *connection, struct usfs_request *request)
{
    (void)connection;
    if (close_during_getattr) handle_available = 0;
    if (transport_error) free_request(request);
    return transport_error;
}

static struct usfs_mount_data *_mount_of(struct vnode *vp)
{
    (void)vp;
    return fake_mount_available ? &fake_mount : NULL;
}

static int _setattr(struct vnode *vp, const struct usfs_setattr_in *body,
                    uint64_t fh, struct ucred *crp)
{
    (void)vp;
    setattr_handle = fh;
    (void)crp;
    usfs_fake_kernel.setattr_calls += 1;
    usfs_fake_kernel.setattr_body = *body;
    usfs_fake_kernel_event(USFS_FAKE_EVENT_SETATTR);
    return usfs_fake_kernel.setattr_rc;
}

#define gn_getattr real_node_getattr
#include "fs/operations/vnode_getattr.h"
#undef gn_getattr

int gn_getattr(struct vnode *vp, struct vattr *attr, struct ucred *crp)
{
    if (real_getattr) return real_node_getattr(vp, attr, crp);
    (void)vp;
    (void)crp;
    usfs_fake_kernel.getattr_calls += 1;
    usfs_fake_kernel_event(USFS_FAKE_EVENT_GETATTR);
    if (usfs_fake_kernel.getattr_rc == 0)
        *attr = usfs_fake_kernel.attr;
    return usfs_fake_kernel.getattr_rc;
}

#include "fs/operations/vnode_access.h"
#include "fs/operations/vnode_setattr.h"

static int test_prepare_attribute_change (
    struct vnode * file_vnode,
    const int32long64_t command,
    const int32long64_t argument1,
    const int32long64_t argument2,
    const int32long64_t argument3,
    const struct vattr * current_attributes,
    const int is_owner,
    const int can_set_dac,
    struct ucred * credentials,
    struct usfs_setattr_in * request_body
)
{
    struct attribute_change_context context = {
        .file_vnode = file_vnode,
        .credentials = credentials,
        .current_attributes = *current_attributes,
        .command = command,
        .argument1 = argument1,
        .argument2 = argument2,
        .argument3 = argument3,
        .is_owner = is_owner,
        .can_set_dac = can_set_dac
    };

    const int rc = prepare_attribute_change (&context);

    *request_body = context.request_body;

    return rc;
}

static int test_prepare_owner_change (
    struct usfs_setattr_in * request_body,
    const int32long64_t flags,
    const int32long64_t user_id,
    const int32long64_t group_id,
    const struct vattr * current_attributes,
    const int is_owner,
    const int can_set_dac,
    struct ucred * credentials
)
{
    struct attribute_change_context context = {
        .credentials = credentials,
        .current_attributes = *current_attributes,
        .request_body = *request_body,
        .argument1 = flags,
        .argument2 = user_id,
        .argument3 = group_id,
        .is_owner = is_owner,
        .can_set_dac = can_set_dac
    };

    const int rc = prepare_owner_change (&context);

    *request_body = context.request_body;

    return rc;
}

static void reset_fixture(struct vnode *vnode, struct ucred *credential)
{
    real_getattr = 0;
    memset(&attribute_node, 0, sizeof(attribute_node));
    handle_available = operation_balance = close_during_getattr = releases = 0;
    getattr_handle = setattr_handle = 0;
    usfs_fake_kernel_reset();
    memset(vnode, 0, sizeof(*vnode));
    memset(credential, 0, sizeof(*credential));
    credential->cr_uid = 1000;
    fake_mount_available = 1;
    memset(&fake_mount, 0, sizeof(fake_mount));
    fake_mount.writable = 1;
}

static void test_access(struct tap_state *tap)
{
    struct vnode vnode;
    struct ucred credential;

    reset_fixture(&vnode, &credential);
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, NULL) == EIO,
           "access rejects null credentials");
    fake_mount_available = 0;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, &credential) == EIO,
           "access rejects vnode outside a mount");
    fake_mount_available = 1;
    tap_ok(tap, gn_access(&vnode, 0x4000, ACC_SELF, &credential) == EINVAL,
           "access rejects unknown mode bits");
    fake_mount.writable = 0;
    tap_ok(tap, gn_access(&vnode, W_ACC, ACC_SELF, &credential) == EROFS,
           "access rejects writes on a read-only mount");
    tap_ok(tap, gn_access(&vnode, E_ACC, ACC_SELF, &credential) == 0,
           "existence check needs no daemon request");
    fake_mount.writable = 1;
    tap_ok(tap, gn_access(&vnode, R_ACC | W_ACC, ACC_ANY,
                          &credential) == EINVAL,
           "non-self access accepts only one requested class");

    usfs_fake_kernel.attr.va_mode = 0640;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, &credential) == 0,
           "owner permission permits access");
    credential.cr_uid = 2000;
    usfs_fake_kernel.group_member = 1;
    usfs_fake_kernel.member_gid = usfs_fake_kernel.attr.va_gid;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, &credential) == 0,
           "group permission permits access");
    usfs_fake_kernel.group_member = 0;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, &credential) == EACCES,
           "missing discretionary permission is denied");
    usfs_fake_kernel.bypass_read = 1;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, &credential) == 0,
           "read bypass privilege permits access");
    usfs_fake_kernel.bypass_read = 0;
    usfs_fake_kernel.attr.va_mode = 0040;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_ANY, &credential) == 0,
           "ACC_ANY accepts any permitted class");
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_OTHERS, &credential) == 0,
           "ACC_OTHERS includes group permission");
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_ALL, &credential) == EACCES,
           "ACC_ALL requires every permission class");
    tap_ok(tap, access_for_subject(&usfs_fake_kernel.attr, R_ACC, 9999,
                                        &credential) == EINVAL,
           "access subject helper rejects an unknown selector");
    usfs_fake_kernel.getattr_rc = ESTALE;
    tap_ok(tap, gn_access(&vnode, R_ACC, ACC_SELF, &credential) == ESTALE,
           "attribute request errors propagate");
}

static void test_setattr(struct tap_state *tap)
{
    struct vnode vnode;
    struct ucred credential;
    struct usfs_setattr_in body;
    struct timestruc_t atime = { 11, 12 };
    struct timestruc_t mtime = { 21, 22 };
    struct timestruc_t ctime = { 31, 32 };
    int rc;

    reset_fixture(&vnode, &credential);
    memset(&body, 0, sizeof(body));
    rc = test_prepare_attribute_change(&vnode, V_MODE, 0600, 0, 0,
                                       &usfs_fake_kernel.attr, 0, 0,
                                       &credential, &body);
    tap_ok(tap, rc == EPERM, "non-owner mode change is rejected");
    rc = test_prepare_attribute_change(&vnode, V_MODE, 0600, 0, 0,
                                       &usfs_fake_kernel.attr, 1, 0,
                                       &credential, &body);
    tap_ok(tap, rc == 0 && body.valid == USFS_SET_MODE && body.mode == 0600,
           "owner mode change is encoded");

    memset(&body, 0, sizeof(body));
    rc = test_prepare_owner_change(&body, 0, 2000, 300,
                               &usfs_fake_kernel.attr, 1, 0, &credential);
    tap_ok(tap, rc == EPERM, "owner cannot assign an unrelated group");
    memset(&body, 0, sizeof(body));
    rc = test_prepare_owner_change(&body, 0, 2000,
                               usfs_fake_kernel.attr.va_gid,
                               &usfs_fake_kernel.attr, 1, 0, &credential);
    tap_ok(tap, rc == EPERM,
           "owner cannot change uid without DAC privilege");
    memset(&body, 0, sizeof(body));
    rc = test_prepare_owner_change(&body, 0, 2000, 300,
                               &usfs_fake_kernel.attr, 0, 1, &credential);
    tap_ok(tap, rc == 0 &&
                 body.valid == (USFS_SET_UID | USFS_SET_GID) &&
                 body.uid == 2000 && body.gid == 300,
           "DAC privilege permits uid and gid changes");
    memset(&body, 0, sizeof(body));
    usfs_fake_kernel.group_member = 1;
    usfs_fake_kernel.member_gid = 300;
    rc = test_prepare_owner_change(&body, T_OWNER_AS_IS, 2000, 300,
                               &usfs_fake_kernel.attr, 1, 0, &credential);
    tap_ok(tap, rc == 0 && body.valid == USFS_SET_GID && body.gid == 300,
           "owner can assign a member group without changing uid");

    memset(&body, 0, sizeof(body));
    usfs_fake_kernel.attr.va_mode = 06755;
    rc = test_prepare_owner_change(&body, T_OWNER_AS_IS, 2000, 300,
                               &usfs_fake_kernel.attr, 1, 0, &credential);
    tap_ok(tap, rc == 0 && body.valid == (USFS_SET_GID | USFS_SET_MODE) &&
           body.mode == 0755, "unprivileged group change removes both set-ID bits");
    usfs_fake_kernel.attr.va_mode = 0644;

    for (unsigned field = 0; field < 3; ++field) {
        const int flags = field == 0 ? 0 : field == 1 ? T_OWNER_AS_IS : T_GROUP_AS_IS;
        memset(&body, 0, sizeof(body));
        usfs_fake_kernel.attr.va_mode = 06755;
        rc = test_prepare_owner_change(&body, flags, usfs_fake_kernel.attr.va_uid,
                                   usfs_fake_kernel.attr.va_gid,
                                   &usfs_fake_kernel.attr, 0, 0, &credential);
        tap_ok(tap, rc == EPERM && body.valid == 0 && body.mode == 0,
               "nonowner cannot turn unchanged ownership IDs into a set-ID mode mutation");
    }
    memset(&body, 0, sizeof(body));
    rc = test_prepare_owner_change(&body, T_OWNER_AS_IS | T_GROUP_AS_IS, 0, 0,
                               &usfs_fake_kernel.attr, 0, 0, &credential);
    tap_ok(tap, rc == 0 && body.valid == 0,
           "leaving both ownership fields as-is remains a mutation-free no-op");
    usfs_fake_kernel.attr.va_mode = 0644;

    memset(&body, 0, sizeof(body));
    rc = test_prepare_attribute_change(
        &vnode, V_STIME, (int32long64_t)&atime, (int32long64_t)&mtime,
        (int32long64_t)&ctime, &usfs_fake_kernel.attr, 1, 0,
        &credential, &body);
    tap_ok(tap, rc == EOPNOTSUPP && body.valid == 0,
           "explicit ctime rejects the whole change before mutation");
    memset(&body, 0, sizeof(body));
    rc = test_prepare_attribute_change(&vnode, V_UTIME, T_SETTIME, 0, 0,
                                       &usfs_fake_kernel.attr, 0, 0,
                                       &credential, &body);
    tap_ok(tap, rc == 0 &&
                 body.valid == (USFS_SET_TIMES_NOW | USFS_SET_ATIME |
                                USFS_SET_MTIME),
           "permitted current-time update is encoded");
    memset(&body, 0, sizeof(body));
    usfs_fake_kernel.attr.va_mode = 0444;
    rc = test_prepare_attribute_change(&vnode, V_UTIME, T_SETTIME, 0, 0,
                                       &usfs_fake_kernel.attr, 0, 0,
                                       &credential, &body);
    tap_ok(tap, rc == EACCES,
           "current-time update propagates failed write access");
    memset(&body, 0, sizeof(body));
    rc = test_prepare_attribute_change(&vnode, V_UTIME, 0, 0, 0,
                                       &usfs_fake_kernel.attr, 0, 0,
                                       &credential, &body);
    tap_ok(tap, rc == EPERM,
           "explicit time change requires ownership or DAC privilege");
    memset(&body, 0, sizeof(body));
    rc = test_prepare_attribute_change(&vnode, V_UTIME, 0, 0, 0,
                                       &usfs_fake_kernel.attr, 1, 0,
                                       &credential, &body);
    tap_ok(tap, rc == 0 && body.valid == 0,
           "null explicit timestamps leave all times unchanged");
    memset(&body, 0, sizeof(body));
    rc = test_prepare_attribute_change(&vnode, 9999, 0, 0, 0,
                                       &usfs_fake_kernel.attr, 1, 0,
                                       &credential, &body);
    tap_ok(tap, rc == EINVAL, "unknown setattr command is rejected");
    fake_mount.writable = 0;
    tap_ok(tap, gn_setattr(&vnode, V_MODE, 0600, 0, 0, &credential) == EROFS,
           "setattr rejects a read-only mount before getattr");
    fake_mount.writable = 1;
    usfs_fake_kernel.getattr_rc = EIO;
    tap_ok(tap, gn_setattr(&vnode, V_MODE, 0600, 0, 0, &credential) == EIO,
           "setattr propagates getattr failure");
}

static void test_root_metadata(struct tap_state *tap)
{
    struct vnode vnode;
    struct ucred credential;
    struct vattr output, unchanged;
    unsigned scenario;
    const int errors[] = { ENOMEM, ETIMEDOUT, EACCES, EIO, EIO, EIO };
    const char *names[] = {
        "root allocation error fails stat and access without synthetic permissions",
        "root transport error fails stat and access without synthetic permissions",
        "root daemon error fails stat and access without synthetic permissions",
        "short root reply fails stat and access without synthetic permissions",
        "invalid root attributes fail stat and access without synthetic permissions",
        "invalid root parent fails stat and access without synthetic permissions"
    };
    reset_fixture(&vnode, &credential);
    real_getattr = 1;
    memset(&attribute_node, 0, sizeof(attribute_node));
    attribute_node.nodeid = USFS_ROOT_ID;
    attribute_node.vn = &vnode;
    fake_mount.root = &attribute_node;
    memset(&attribute_reply, 0, sizeof(attribute_reply));
    attribute_reply.attr.ino = USFS_ROOT_ID;
    attribute_reply.attr.mode = S_IFDIR | 0700;
    attribute_reply.attr.uid = 1000;
    attribute_reply.attr.nlink = 2;
    attribute_reply.attr.blksize = 4096;
    attribute_reply.parent = USFS_ROOT_ID;
    memset(&attribute_request, 0, sizeof(attribute_request));
    attribute_request.reply_buffer = (char *)&attribute_reply;
    attribute_request.reply_buffer_size = sizeof(attribute_reply);
    credential.cr_uid = 2000;
    tap_ok(tap, gn_getattr(&vnode, &output, &credential) == 0 &&
           output.va_mode == (S_IFDIR | 0700) && output.va_uid == 1000 &&
           gn_access(&vnode, R_ACC | X_ACC, ACC_SELF, &credential) == EACCES,
           "authoritative restrictive root attributes deny another user");
    memset(&unchanged, 0x5a, sizeof(unchanged));
    for (scenario = 0; scenario < 6; ++scenario) {
        int stat_rc, access_rc;
        allocation_error = scenario == 0 ? ENOMEM : 0;
        transport_error = scenario == 1 ? ETIMEDOUT : 0;
        attribute_request.error = scenario == 2 ? EACCES : 0;
        attribute_request.reply_buffer_size = sizeof(attribute_reply) - (scenario == 3);
        attribute_reply.attr.atimensec = scenario == 4 ? 1000000000u : 0;
        attribute_reply.parent = scenario == 5 ? 0 : USFS_ROOT_ID;
        output = unchanged;
        stat_rc = gn_getattr(&vnode, &output, &credential);
        access_rc = gn_access(&vnode, R_ACC | X_ACC, ACC_SELF, &credential);
        tap_ok(tap, stat_rc == errors[scenario] && access_rc == errors[scenario] &&
               memcmp(&output, &unchanged, sizeof(output)) == 0 && request_balance == 0,
               names[scenario]);
    }
    allocation_error = transport_error = attribute_request.error = 0;
    attribute_request.reply_buffer_size = sizeof(attribute_reply);
    attribute_reply.attr.mode = S_IFDIR | 0700;
    attribute_reply.parent = USFS_ROOT_ID;
    tap_ok(tap, gn_getattr(&vnode, &output, &credential) == 0 &&
           gn_access(&vnode, X_ACC, ACC_SELF, &credential) == EACCES && request_balance == 0,
           "root metadata recovers without broadening authorization");
}

static void test_mapped_setid(struct tap_state *tap)
{
    const unsigned modes[] = { 04755, 02755, 06755, 0777, 06666 };
    unsigned i;
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        struct vnode vnode;
        struct ucred credential;
        int rc;
        reset_fixture(&vnode, &credential);
        handle_available = 1;
        attribute_node.cache_shared_writable = 1;
        usfs_fake_kernel.set_obj_dac = 1;
        rc = gn_setattr(&vnode, V_MODE, modes[i], 0, 0, &credential);
        tap_ok(tap, rc == (i < 3 ? EBUSY : 0) &&
               usfs_fake_kernel.setattr_calls == (i < 3 ? 0 : 1) &&
               operation_balance == 0 && namespace_depth == 0,
               "shared writable cache rejects executable set-ID atomically and permits other modes");
    }
}

static void test_retained_attributes(struct tap_state *tap)
{
    unsigned scenario;
    const int expected[] = { 0, EPERM, EIO, ENOMEM, 0, EINVAL, 0 };
    for (scenario = 0; scenario < sizeof(expected) / sizeof(expected[0]); ++scenario) {
        struct vnode vnode;
        struct ucred credential;
        int rc;
        reset_fixture(&vnode, &credential);
        real_getattr = handle_available = close_during_getattr = 1;
        attribute_node.vn = &vnode;
        attribute_node.nodeid = 2;
        memset(&attribute_reply, 0, sizeof(attribute_reply));
        attribute_reply.parent = 1;
        attribute_reply.attr.ino = 2;
        attribute_reply.attr.mode = S_IFREG | 0666;
        attribute_reply.attr.uid = scenario == 1 || scenario == 6 ? 2000 : 1000;
        attribute_reply.attr.blksize = 4096;
        memset(&attribute_request, 0, sizeof(attribute_request));
        attribute_request.reply_buffer = (char *)&attribute_reply;
        attribute_request.reply_buffer_size = sizeof(attribute_reply);
        allocation_error = scenario == 3 ? ENOMEM : 0;
        usfs_fake_kernel.setattr_rc = scenario == 2 ? EIO : 0;
        rc = gn_setattr(&vnode, scenario == 4 || scenario == 6 ? V_UTIME :
                       scenario == 5 ? 9999 : V_MODE,
                       scenario == 6 ? T_SETTIME : scenario == 4 ? 0 : 0600,
                       0, 0, &credential);
        tap_ok(tap, rc == expected[scenario] && operation_balance == 0 &&
               request_balance == 0 && getattr_handle == 0 && releases == 0 && setattr_handle == 0,
               "attribute authorization uses object identity without borrowing an open handle");
    }
    allocation_error = 0;
}

int main(void)
{
    struct tap_state tap;

    tap_plan(&tap, 55);
    test_access(&tap);
    test_setattr(&tap);
    test_root_metadata(&tap);
    test_retained_attributes(&tap);
    test_mapped_setid(&tap);
    tap_ok(&tap, usfs_fake_kernel_clean() && namespace_depth == 0,
           "access and setattr leave fake ownership balanced");
    return tap_finish(&tap);
}
