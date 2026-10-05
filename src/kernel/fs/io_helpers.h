// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_IO_HELPERS_H
#define USFS_FS_IO_HELPERS_H

uint32_t usfs_rdwr_chunk_size (const struct uio * user_io_request);
int usfs_validate_rdwr_arguments (struct vnode * file_vnode, const enum uio_rw operation, const struct uio * user_io_request);
int usfs_validate_mapping_arguments (
    struct vnode * file_vnode,
    const uint64_t mapping_length,
    const uint64_t mapping_offset,
    const uint64_t flags,
    const struct ucred * credentials,
    int * required_access_out
);
int usfs_pager_offset (const struct buf * buffer, uint64_t * offset_out);
int usfs_pager_count_valid (const void * page_data, const uint32_t byte_count);

#endif
