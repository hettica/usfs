/* Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

static int getattr_calls;
static int getattr_error;
static int reclock_calls;
static int reclock_error;
static offset_t observed_size;
static offset_t observed_offset;
static int observed_command;
static struct gnode * observed_gnode;
static struct eflock * observed_lock;

int gn_getattr (struct vnode * file_vnode, struct vattr * attributes, struct ucred * credentials)
{
    (void)file_vnode;
    (void)credentials;
    ++getattr_calls;
    if (getattr_error != 0)
        return getattr_error;
    attributes->va_size = 8192;
    return 0;
}

int common_reclock (
    struct gnode * gnode,
    offset_t size,
    offset_t offset,
    struct eflock * lock_data,
    int command,
    int (*retry_function) (),
    ulong * retry_id,
    int (*lock_function) (),
    int (*release_function) ()
)
{
    ++reclock_calls;
    observed_gnode = gnode;
    observed_size = size;
    observed_offset = offset;
    observed_lock = lock_data;
    observed_command = command;
    if (retry_function != NULL || retry_id != NULL || lock_function != NULL || release_function != NULL)
        return EINVAL;
    return reclock_error;
}

#include "fs/operations/vnode_lockctl.h"

int main (void)
{
    struct tap_state tap;
    struct gnode gnode = { .gn_type = VREG };
    struct vnode vnode = { .v_gnode = &gnode };
    struct eflock lock_data = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1 };

    tap_plan (&tap, 7);
    tap_ok (&tap, gn_lockctl (NULL, 0, &lock_data, SETFLCK, NULL, NULL, NULL) == EINVAL &&
                      gn_lockctl (&vnode, 0, NULL, SETFLCK, NULL, NULL, NULL) == EINVAL && reclock_calls == 0,
            "invalid lock inputs never enter AIX record locking");

    gnode.gn_type = VDIR;
    tap_ok (&tap, gn_lockctl (&vnode, 0, &lock_data, SETFLCK, NULL, NULL, NULL) == EINVAL && reclock_calls == 0,
            "directories cannot acquire record locks");
    gnode.gn_type = VREG;

    tap_ok (&tap, gn_lockctl (&vnode, 0, &lock_data, INOFLCK | SETFLCK, NULL, NULL, NULL) == EINVAL &&
                      gn_lockctl (&vnode, 0, &lock_data, SLPFLCK, NULL, NULL, NULL) == EINVAL && reclock_calls == 0,
            "unsupported inode-locked and incomplete wait commands are rejected");

    tap_ok (&tap, gn_lockctl (&vnode, 17, &lock_data, SETFLCK, NULL, NULL, NULL) == 0 && reclock_calls == 1 &&
                      getattr_calls == 0 && observed_gnode == &gnode && observed_lock == &lock_data &&
                      observed_offset == 17 && observed_size == 0 && observed_command == SETFLCK,
            "nonblocking lock uses the gnode without a backend metadata request");

    lock_data.l_whence = SEEK_END;
    tap_ok (&tap, gn_lockctl (&vnode, 31, &lock_data, SETFLCK | SLPFLCK, NULL, NULL, NULL) == 0 &&
                      getattr_calls == 1 && reclock_calls == 2 && observed_size == 8192 && observed_offset == 31 &&
                      observed_command == (SETFLCK | SLPFLCK),
            "end-relative blocking lock uses authoritative file size");

    getattr_error = EIO;
    tap_ok (&tap, gn_lockctl (&vnode, 0, &lock_data, SETFLCK, NULL, NULL, NULL) == EIO && reclock_calls == 2,
            "failed end-relative metadata lookup does not establish a lock");

    lock_data.l_whence = SEEK_SET;
    reclock_error = EAGAIN;
    tap_ok (&tap, gn_lockctl (&vnode, 0, &lock_data, SETFLCK, NULL, NULL, NULL) == EAGAIN && reclock_calls == 3,
            "AIX record-lock contention error is returned unchanged");

    return tap_finish (&tap);
}
