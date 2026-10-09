/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include "fs/operations/not_implemented/vfs_cntl.h"
#include "fs/operations/not_implemented/vfs_quotactl.h"
#include "fs/operations/not_implemented/vfs_aclxcntl.h"
#include "fs/operations/not_implemented/vfs_statfsvp.h"
#include "fs/operations/not_implemented/vnode_ioctl.h"
#include "fs/operations/not_implemented/vnode_select.h"
#include "fs/operations/not_implemented/vnode_revoke.h"
#include "fs/operations/not_implemented/vnode_getacl.h"
#include "fs/operations/not_implemented/vnode_setacl.h"
#include "fs/operations/not_implemented/vnode_getpcl.h"
#include "fs/operations/not_implemented/vnode_setpcl.h"
#include "fs/operations/vnode_seek.h"
#include "fs/operations/not_implemented/vnode_finfo.h"
#include "fs/operations/not_implemented/vnode_fclear.h"
#include "fs/operations/not_implemented/vnode_getea.h"
#include "fs/operations/not_implemented/vnode_setea.h"
#include "fs/operations/not_implemented/vnode_listea.h"
#include "fs/operations/not_implemented/vnode_removeea.h"
#include "fs/operations/not_implemented/vnode_statea.h"
#include "fs/operations/not_implemented/vnode_getxacl.h"
#include "fs/operations/not_implemented/vnode_setxacl.h"
#include "fs/operations/not_implemented/vnode_memcntl.h"
#include "fs/operations/not_implemented/vnode_lockctl.h"
#include "fs/operations/not_implemented/vnode_mknod.h"
#include "fs/operations/not_implemented/vnode_erdwr_attr.h"

int main(void)
{
    struct tap_state tap;
    offset_t offset = 123;

    tap_plan(&tap, 25);
    tap_ok(&tap, usfs_cntl(NULL, 0, NULL, 0, NULL) == ENOSYS,
           "vfs control is explicitly unsupported");
    tap_ok(&tap, usfs_quotactl(NULL, 0, 0, NULL, NULL) == ENOSYS,
           "quota control is explicitly unsupported");
    tap_ok(&tap, usfs_aclxcntl(NULL, NULL, 0, NULL, NULL, NULL) == ENOSYS,
           "vfs ACL control is explicitly unsupported");
    tap_ok(&tap, usfs_statfsvp(NULL, NULL, NULL, NULL) == ENOSYS,
           "per-vnode statfs is explicitly unsupported");
    tap_ok(&tap, gn_ioctl(NULL, 0, NULL, 0, 0, NULL) == ENOSYS,
           "vnode ioctl is explicitly unsupported");
    tap_ok(&tap, gn_select(NULL, 0, 0, NULL, NULL, NULL, NULL) == ENOSYS,
           "vnode select is explicitly unsupported");
    tap_ok(&tap, gn_revoke(NULL, 0, 0, NULL, NULL) == ENOSYS,
           "vnode revoke is explicitly unsupported");
    tap_ok(&tap, gn_getacl(NULL, NULL, NULL) == ENOSYS,
           "ACL read is explicitly unsupported");
    tap_ok(&tap, gn_setacl(NULL, NULL, NULL) == ENOSYS,
           "ACL write is explicitly unsupported");
    tap_ok(&tap, gn_getpcl(NULL, NULL, NULL) == ENOSYS,
           "PCL read is explicitly unsupported");
    tap_ok(&tap, gn_setpcl(NULL, NULL, NULL) == ENOSYS,
           "PCL write is explicitly unsupported");
    tap_ok(&tap, gn_seek(NULL, &offset, NULL) == 0 && offset == 123,
           "seek accepts the caller offset without mutation");
    tap_ok(&tap, gn_getea(NULL, NULL, NULL, NULL) == ENOSYS,
           "extended attribute read is explicitly unsupported");
    tap_ok(&tap, gn_setea(NULL, NULL, NULL, 0, NULL) == ENOSYS,
           "extended attribute write is explicitly unsupported");
    tap_ok(&tap, gn_finfo(NULL, 0, NULL, 0, NULL) == ENOSYS,
           "file information queries are explicitly unsupported");
    tap_ok(&tap, gn_fclear(NULL, 0, 0, 0, NULL, NULL) == ENOSYS,
           "file range clearing is explicitly unsupported");
    tap_ok(&tap, gn_listea(NULL, NULL, NULL) == ENOSYS,
           "extended attribute listing is explicitly unsupported");
    tap_ok(&tap, gn_removeea(NULL, NULL, NULL) == ENOSYS,
           "extended attribute removal is explicitly unsupported");
    tap_ok(&tap, gn_statea(NULL, NULL, NULL, NULL) == ENOSYS,
           "extended attribute status is explicitly unsupported");
    tap_ok(&tap, gn_getxacl(NULL, 0, NULL, NULL, NULL, NULL, NULL) == ENOSYS,
           "extended ACL read is explicitly unsupported");
    tap_ok(&tap, gn_setxacl(NULL, 0, (acl_type_t)0, NULL, 0, NULL) == ENOSYS,
           "extended ACL write is explicitly unsupported");
    tap_ok(&tap, gn_memcntl(NULL, 0, NULL, NULL) == ENOSYS,
           "memory attachment control is explicitly unsupported");
    tap_ok(&tap, gn_lockctl(NULL, 0, NULL, 0, NULL, NULL, NULL) == ENOSYS,
           "record locking is explicitly unsupported");
    tap_ok(&tap, gn_mknod(NULL, NULL, 0, 0, NULL) == ENOSYS,
           "special-file creation is explicitly unsupported");
    tap_ok(&tap,
           gn_erdwr_attr(NULL, UIO_READ, 0, NULL, 0, NULL, NULL, NULL,
                         NULL, NULL) == ENOSYS,
           "security-extended I/O is explicitly unsupported");
    return tap_finish(&tap);
}
