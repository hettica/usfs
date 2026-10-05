// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_MAP_LLOFF_H
#define USFS_FS_OPERATIONS_VNODE_MAP_LLOFF_H

int gn_map_lloff (
    struct vnode * file_vnode,
    caddr_t address,
    const offset_t length,
    const offset_t offset,
    const uint32long64_t mapping_flags,
    const uint32long64_t file_flags,
    struct ucred * credentials
)
{
    ignore_parameter address;
    ignore_parameter file_flags;

    if (length <= 0 || offset < 0)
        return EINVAL;

    const int rc = validate_mapping (file_vnode, length, offset, mapping_flags, credentials);
    if (rc != 0)
        return rc;

    return map_validated_file (file_vnode, mapping_flags, credentials);
}

#endif
