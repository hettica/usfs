// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "tap.h"
#include "definitions.h"
#include <assert.h>

static struct vnode file_vnode;
static struct ucred credentials;
static struct usfs_connection connection;
static int backend_result;
static unsigned sync_calls;
static uint32_t last_mode;
static int last_range;
static offset_t last_offset;
static offset_t last_length;
static int attribute_result;
static unsigned attribute_calls;
static unsigned io_calls;

static int _sync_node (
    struct vnode * requested_vnode,
    const uint64_t handle,
    const int32long64_t flags,
    const int use_range,
    const offset_t offset,
    const offset_t length,
    struct ucred * requested_credentials
)
{
    assert (requested_vnode == &file_vnode && handle == 0 && flags == FWRITE);
    assert (requested_credentials == &credentials);
    sync_calls++;
    last_range = use_range;
    last_offset = offset;
    last_length = length;
    return backend_result;
}

static int _sync_all_mounts (const uint32_t mode, struct ucred * requested_credentials)
{
    assert (requested_credentials == NULL || requested_credentials == &credentials);
    sync_calls++;
    last_mode = mode;
    return backend_result;
}

static int _sync_connection (
    struct usfs_connection * requested_connection,
    const uint32_t mode,
    const struct ucred * requested_credentials
)
{
    assert (requested_connection == NULL || requested_connection == &connection);
    assert (requested_credentials == &credentials);
    sync_calls++;
    last_mode = mode;
    return backend_result;
}

int gn_getattr (struct vnode * requested_vnode, struct vattr * attributes, struct ucred * requested_credentials)
{
    assert (requested_vnode == &file_vnode && requested_credentials == &credentials);
    attribute_calls++;
    attributes->va_size = 123;
    return attribute_result;
}

int gn_rdwr (
    struct vnode * requested_vnode,
    const enum uio_rw operation,
    const int32long64_t flags,
    struct uio * request,
    const ext_t extension,
    caddr_t file_info,
    struct vattr * post_attributes,
    struct ucred * requested_credentials
)
{
    assert (requested_vnode == &file_vnode && operation == UIO_WRITE && flags == FWRITE);
    assert (request == NULL && extension == 0 && file_info == NULL);
    assert (requested_credentials == &credentials);
    io_calls++;
    post_attributes->va_size = 456;
    return backend_result;
}

#include "fs/operations/vnode_fsync.h"
#include "fs/operations/vnode_fsync_range.h"
#include "fs/operations/vfs_sync.h"
#include "fs/operations/vfs_syncvfs.h"
#include "fs/operations/vnode_rdwr_attr.h"

static void verify_file_sync (struct tap_state * tap)
{
    struct gnode node = { 0 };

    tap_ok (tap, gn_fsync (NULL, FWRITE, 0, &credentials) == EINVAL, "fsync rejects a missing vnode");
    tap_ok (tap, gn_fsync (&file_vnode, FWRITE, 0, &credentials) == EINVAL, "fsync rejects a missing gnode");
    tap_ok (tap, gn_fsync_range (NULL, FWRITE, 0, 10, 20, &credentials) == EINVAL, "range fsync rejects a missing vnode");
    tap_ok (tap, gn_fsync_range (&file_vnode, FWRITE, 0, 10, 20, &credentials) == EINVAL && sync_calls == 0, "invalid sync requests never reach the backend");

    file_vnode.v_gnode = &node;
    tap_ok (tap, gn_fsync (&file_vnode, FWRITE, 0, &credentials) == 0 && last_range == 0 && last_offset == 0 && last_length == 0,
            "whole-file fsync passes flags and credentials without a range");
    backend_result = EIO;
    tap_ok (tap, gn_fsync (&file_vnode, FWRITE, 0, &credentials) == EIO, "fsync propagates backend failure");
    tap_ok (tap, gn_fsync_range (&file_vnode, FWRITE, 0, 10, 20, &credentials) == EIO && last_range == 1 && last_offset == 10 && last_length == 20,
            "range fsync retains byte bounds and propagates failure");
    backend_result = 0;
    tap_ok (tap, gn_fsync_range (&file_vnode, FWRITE, 0, 10, 20, &credentials) == 0, "range fsync returns backend success");
    file_vnode.v_gnode = NULL;
}

static void verify_mount_sync (struct tap_state * tap)
{
    struct gfs global_file_system = { 0 };
    struct vfs file_system = { 0 };
    struct usfs_mount_data mount_data = { 0 };

    tap_ok (tap, usfs_sync (NULL) == EINVAL, "global sync requires a filesystem registration");
    tap_ok (tap, usfs_sync (&global_file_system) == 0 && last_mode == USFS_SYNCFS_TRY, "global sync uses nonblocking mode");
    backend_result = EBUSY;
    tap_ok (tap, usfs_sync (&global_file_system) == EBUSY, "global sync preserves a backend error");
    tap_ok (tap, usfs_syncvfs (NULL, NULL, FS_SYNCVFS_TRY, &credentials) == EINVAL, "syncvfs rejects missing registration without a mount");
    tap_ok (tap, usfs_syncvfs (NULL, &file_system, FS_SYNCVFS_TRY, &credentials) == EINVAL, "syncvfs rejects missing registration with a mount");
    tap_ok (tap, usfs_syncvfs (&global_file_system, NULL, 3, &credentials) == EINVAL, "syncvfs rejects unsupported mode without a mount");
    tap_ok (tap, usfs_syncvfs (&global_file_system, &file_system, 3, &credentials) == EINVAL, "syncvfs rejects unsupported mode with a mount");
    tap_ok (tap, usfs_syncvfs (&global_file_system, &file_system, FS_SYNCVFS_TRY, &credentials) == EIO, "syncvfs rejects a mount without private state");

    const int flags[] = { FS_SYNCVFS_TRY, FS_SYNCVFS_FORCE, FS_SYNCVFS_QUIESCE };
    const uint32_t modes[] = { USFS_SYNCFS_TRY, USFS_SYNCFS_FORCE, USFS_SYNCFS_QUIESCE };

    for (unsigned mode_index = 0; mode_index < sizeof (flags) / sizeof (flags[0]); mode_index++)
    {
        tap_ok (tap, usfs_syncvfs (&global_file_system, NULL, flags[mode_index], &credentials) == EBUSY && last_mode == modes[mode_index],
                "syncvfs maps each global synchronization mode and preserves failure");
        mount_data.conn = &connection;
        file_system.vfs_data = (caddr_t)&mount_data;
        tap_ok (tap, usfs_syncvfs (&global_file_system, &file_system, flags[mode_index], &credentials) == EBUSY && last_mode == modes[mode_index],
                "syncvfs routes each mount synchronization mode to its connection");
    }

    mount_data.conn = NULL;
    backend_result = ENOTCONN;
    tap_ok (tap, usfs_syncvfs (&global_file_system, &file_system, FS_SYNCVFS_TRY, &credentials) == ENOTCONN,
            "syncvfs preserves disconnected-mount failure");
}

static void verify_attribute_io (struct tap_state * tap)
{
    struct vattr before = { 0 };
    struct vattr after = { 0 };

    backend_result = 0;
    tap_ok (tap, gn_rdwr_attr (&file_vnode, UIO_WRITE, FWRITE, NULL, 0, NULL, NULL, &after, &credentials) == 0 && attribute_calls == 0 && after.va_size == 456,
            "I/O without requested pre-attributes skips getattr and returns post-attributes");
    tap_ok (tap, gn_rdwr_attr (&file_vnode, UIO_WRITE, FWRITE, NULL, 0, NULL, &before, &after, &credentials) == 0 && before.va_size == 123 && io_calls == 2,
            "I/O captures attributes before dispatch and returns the backend attributes");
    attribute_result = EACCES;
    tap_ok (tap, gn_rdwr_attr (&file_vnode, UIO_WRITE, FWRITE, NULL, 0, NULL, &before, &after, &credentials) == EACCES && io_calls == 2,
            "failed pre-attribute lookup prevents I/O");
    attribute_result = 0;
    backend_result = ENOSPC;
    tap_ok (tap, gn_rdwr_attr (&file_vnode, UIO_WRITE, FWRITE, NULL, 0, NULL, &before, &after, &credentials) == ENOSPC && io_calls == 3,
            "attribute wrapper preserves backend I/O errors");
}

int main (void)
{
    struct tap_state tap;

    tap_plan (&tap, 27);
    verify_file_sync (&tap);
    verify_mount_sync (&tap);
    verify_attribute_io (&tap);

    return tap_finish (&tap);
}
