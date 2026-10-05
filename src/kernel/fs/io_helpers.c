// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"
#include "fs/io_helpers.h"

uint32_t usfs_rdwr_chunk_size (const struct uio * user_io_request)
{
    if ((uint64_t)user_io_request->uio_resid < (uint64_t)USFS_MAX_DATA)
        return (uint32_t)user_io_request->uio_resid;

    return USFS_MAX_DATA;
}

int usfs_validate_rdwr_arguments (struct vnode * file_vnode, const enum uio_rw operation, const struct uio * user_io_request)
{
    if (file_vnode == NULL)
        return EINVAL;

    if (user_io_request == NULL)
        return EINVAL;

    if (operation != UIO_READ && operation != UIO_WRITE)
        return EINVAL;

    if (user_io_request->uio_resid < 0)
        return EINVAL;

    if (user_io_request->uio_offset < 0)
        return EINVAL;

    if (user_io_request->uio_resid > 0)
    {
        if (user_io_request->uio_iov == NULL)
            return EINVAL;

        if (user_io_request->uio_iovcnt <= 0)
            return EINVAL;
    }

    return 0;
}

int usfs_validate_mapping_arguments (
    struct vnode * file_vnode,
    const uint64_t mapping_length,
    const uint64_t mapping_offset,
    const uint64_t flags,
    const struct ucred * credentials,
    int * required_access_out
)
{
    if (file_vnode == NULL)
        return EINVAL;

    if (file_vnode->v_gnode == NULL)
        return EINVAL;

    if (credentials == NULL)
        return EINVAL;

    if (required_access_out == NULL)
        return EINVAL;

    if (file_vnode->v_gnode->gn_type != VREG)
        return ENODEV;

    if (mapping_length == 0)
        return EINVAL;

    if (mapping_offset > (uint64_t)INT64_MAX)
        return EINVAL;

    if (mapping_length > (uint64_t)INT64_MAX - mapping_offset)
        return EINVAL;

    int required_access = R_ACC;

    if ((flags & (SHM_RDONLY | SHM_COPY)) == 0)
        required_access |= W_ACC;

    *required_access_out = required_access;

    return 0;
}

int usfs_pager_offset (const struct buf * buffer, uint64_t * offset_out)
{
    if (buffer == NULL)
        return EINVAL;

    if (offset_out == NULL)
        return EINVAL;

    if (buffer->b_blkno < 0)
        return EINVAL;

    const uint64_t block_number = (uint64_t)buffer->b_blkno;
    if (block_number > UINT64_MAX / (uint64_t)UBSIZE)
        return EFBIG;

    *offset_out = block_number * (uint64_t)UBSIZE;

    return 0;
}

int usfs_pager_count_valid (const void * page_data, const uint32_t byte_count)
{
    if (page_data == NULL)
        return false;

    if (byte_count == 0)
        return false;

    return byte_count <= USFS_MAX_DATA;
}
