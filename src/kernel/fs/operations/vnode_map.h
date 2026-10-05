// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_MAP_H
#define USFS_FS_OPERATIONS_VNODE_MAP_H

static int validate_mapping (
    struct vnode * file_vnode,
    const uint64_t length,
    const uint64_t offset,
    const uint64_t mapping_flags,
    struct ucred * credentials
)
{
    int access_mode;
    const int validation_rc = usfs_validate_mapping_arguments (file_vnode, length, offset, mapping_flags, credentials, &access_mode);

    if (validation_rc != 0)
        return validation_rc;

    return gn_access (file_vnode, access_mode, ACC_SELF, credentials);
}

static int create_mapping_segment (const struct vnode * file_vnode, const vmsize_t file_size)
{
    vmid_t segment = 0;
    const int segment_type = V_CLIENT | V_LARGE;
    const vmsize_t segment_size = file_size / (vmsize_t)PAGESIZE + (file_size % (vmsize_t)PAGESIZE != 0);

    if (file_vnode->v_gnode->gn_seg != 0)
        return 0;

    /* vms_create (AIX 7.2 reference, p. 602) accepts the current_attributes size in
     * pages with V_LARGE. Use that form even for empty/small files: this
     * segment survives all later growth, including offsets above 2 GiB. */
    const int create_segment_rc = vms_create (&segment, segment_type, (dev_t)file_vnode->v_gnode, segment_size, 0, 0);
    if (create_segment_rc != 0)
        return create_segment_rc;

    write_synchronized_with (global_lock)
    {
        file_vnode->v_gnode->gn_seg = segment;
    }

    return 0;
}

static int retain_mapping (struct vnode * file_vnode, const uint64_t mapping_flags, const vmsize_t file_size)
{
    struct usfs_node * node = _node_of (file_vnode);
    struct usfs_open_state *open_state = NULL, *release = NULL;

    if (node == NULL)
        return EIO;

    int rc = usfs_retain_mapping_state (file_vnode, mapping_flags, &open_state);
    if (rc != 0)
        return rc;

    rc = ensure_cache_handle (node, 0, open_state);
    if (rc == 0 && usfs_mapping_is_write (mapping_flags))
        rc = ensure_cache_handle (node, 1, open_state);

    if (rc == 0 && file_vnode->v_gnode->gn_seg == 0)
    {
        write_synchronized_with (global_lock)
        {
            node->cache_size = (uint64_t)file_size;
            node->cache_extent = (uint64_t)file_size;
        }

        rc = create_mapping_segment (file_vnode, file_size);
    }

    if (rc != 0)
    {
        (void)usfs_release_mapping_state (file_vnode, (int32long64_t)mapping_flags, &release);

        if (release != NULL)
        {
            _send_release (_mount_of (file_vnode), node->nodeid, release->fh, release->flags, 0, NULL);
            usfs_open_state_discard (release);
        }

        VNOP_RELE (file_vnode);
    }

    return rc;
}

static int map_validated_file (struct vnode * file_vnode, const uint64_t mapping_flags, struct ucred * credentials)
{
    struct usfs_mount_data * mount = _mount_of (file_vnode);
    if (mount == NULL)
        return EIO;

    // todo: use raii-like macros
    struct usfs_complex_lock_guard namespace_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_read_synchronized_acquire (&mount->namespace_lock);

    struct usfs_complex_lock_guard io_guard __attribute__ ((cleanup (usfs_complex_synchronized_release))) =
        usfs_write_synchronized_acquire (&_node_of (file_vnode)->mutation_lock);

    const int access_rc = gn_access (file_vnode, usfs_mapping_is_write (mapping_flags) ? R_ACC | W_ACC : R_ACC, ACC_SELF, credentials);
    if (access_rc != 0)
        return access_rc;

    struct vattr attributes = { 0 };

    const int getattr_rc = gn_getattr (file_vnode, &attributes, credentials);
    if (getattr_rc != 0)
        return getattr_rc;

    if (attributes.va_size < 0)
        return EIO;

    if (usfs_mapping_is_write (mapping_flags))
    {
        /* Privileged mappings would retain set-ID bits, and can later be
         * inherited or used after dropping privilege. Never publish that
         * combination. Unprivileged mappings still strip bits below. */
        if ((attributes.va_mode & 0111) != 0 && (attributes.va_mode & (S_ISUID | S_ISGID)) != 0 && privcheck_cr (SET_OBJ_DAC, credentials) == 0)
            return EBUSY;

        const int rc = usfs_strip_write_privileges (file_vnode, 0, credentials);
        if (rc != 0)
            return rc;
    }

    const int rc = retain_mapping (file_vnode, mapping_flags, (vmsize_t)attributes.va_size);
    if (rc == 0 && usfs_mapping_is_write (mapping_flags))
        _node_of (file_vnode)->cache_shared_writable = 1;

    return rc;
}

int gn_map (
    struct vnode * file_vnode,
    caddr_t address,
    uint32long64_t length,
    uint32long64_t offset,
    uint32long64_t mapping_flags,
    struct ucred * credentials
)
{
    ignore_parameter address;

    const int validation_rc = validate_mapping (file_vnode, (uint64_t)length, (uint64_t)offset, (uint64_t)mapping_flags, credentials);
    if (validation_rc != 0)
        return validation_rc;

    return map_validated_file (file_vnode, (uint64_t)mapping_flags, credentials);
}

#endif
