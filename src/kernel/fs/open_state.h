// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPEN_STATE_H
#define USFS_FS_OPEN_STATE_H

static int allocate_open_state (const int32long64_t open_flags, struct usfs_open_state ** retained_state_out)
{
    static const int memory_alignment_in_bytes = 4;

    if (retained_state_out == NULL)
        return EINVAL;

    *retained_state_out = NULL;

    struct usfs_open_state * open_state = usfs_kmalloc (USFS_ALLOC_OPEN_STATE, sizeof (*open_state), memory_alignment_in_bytes, kernel_heap);
    if (open_state == NULL)
        return ENOMEM;

    memset (open_state, 0, sizeof (*open_state));

    open_state->flags = open_flags;
    open_state->refs = 1;
    open_state->active = true;

    *retained_state_out = open_state;

    return 0;
}

static void usfs_open_state_discard (struct usfs_open_state * open_state)
{
    if (open_state != NULL)
        xmfree (open_state, kernel_heap);
}

static void attach_open_state (struct usfs_node * node, struct usfs_open_state * open_state)
{
    write_synchronized_with (global_lock)
    {
        open_state->next = node->opens;
        node->opens = open_state;
        usfs_vnode_hold_locked (node->vn, USFS_INSTRUMENT_NODE_REUSE_NONE);
    }
}

static struct usfs_open_state ** usfs_open_state_link_locked (struct usfs_node * node, const struct usfs_open_state * open_state)
{
    struct usfs_open_state ** open_state_link = &node->opens;

    while (*open_state_link != NULL)
    {
        if (*open_state_link == open_state)
            break;

        open_state_link = &(*open_state_link)->next;
    }

    return open_state_link;
}

static int usfs_retain_operation_locked (struct usfs_node * node, struct usfs_open_state * open_state, struct usfs_open_state ** retained_state_out)
{
    if (open_state->refs == UINT32_MAX)
        return EOVERFLOW;

    if (open_state->operation_refs == UINT32_MAX)
        return EOVERFLOW;

    open_state->refs += 1;
    open_state->operation_refs += 1;

    usfs_vnode_hold_locked (node->vn, USFS_INSTRUMENT_NODE_REUSE_NONE);

    *retained_state_out = open_state;

    return 0;
}

static int is_open_state_closable (const struct usfs_open_state * linked_state, const struct usfs_open_state * open_state)
{
    if (linked_state != open_state)
        return false;

    if (!open_state->active)
        return false;

    if (open_state->refs == 0)
        return false;

    return true;
}

static int usfs_borrow_description (struct usfs_node * node, const caddr_t file_info, struct usfs_open_state ** retained_state_out)
{
    struct usfs_open_state * open_state = (struct usfs_open_state *)file_info;
    int rc = EBADF;

    if (node == NULL)
        return EINVAL;

    if (open_state == NULL)
        return EINVAL;

    if (retained_state_out == NULL)
        return EINVAL;

    *retained_state_out = NULL;

    write_synchronized_with (global_lock)
    {
        struct usfs_open_state ** open_state_link = usfs_open_state_link_locked (node, open_state);

        if (*open_state_link != open_state)
            return rc;

        if (!open_state->active)
            return rc;

        rc = usfs_retain_operation_locked (node, open_state, retained_state_out);
    }

    return rc;
}

static int usfs_close_open_state (struct usfs_node * node, const caddr_t file_info)
{
    struct usfs_open_state * open_state = (struct usfs_open_state *)file_info;
    int rc = EBADF;

    if (node == NULL)
        return EINVAL;

    if (open_state == NULL)
        return EINVAL;

    write_synchronized_with (global_lock)
    {
        struct usfs_open_state ** open_state_link = usfs_open_state_link_locked (node, open_state);

        if (is_open_state_closable (*open_state_link, open_state))
        {
            open_state->active = false;
            open_state->closing = true;
            rc = 0;
        }
    }

    return rc;
}

static int usfs_release_open_reference (
    struct usfs_node * node,
    struct usfs_open_state * open_state,
    const int is_operation_reference,
    struct usfs_open_state ** released_state_out
)
{
    *released_state_out = NULL;

    write_synchronized_with (global_lock)
    {
        struct usfs_open_state ** open_state_link = usfs_open_state_link_locked (node, open_state);

        if (*open_state_link != open_state)
            return EINVAL;

        if (open_state->refs == 0)
            return EINVAL;

        if (is_operation_reference && open_state->operation_refs == 0)
            return EINVAL;

        if (!is_operation_reference && !open_state->closing)
            return EINVAL;

        if (is_operation_reference)
            open_state->operation_refs -= 1;
        else
            open_state->closing = false;

        open_state->refs -= 1;

        if (open_state->refs == 0)
        {
            *open_state_link = open_state->next;
            open_state->next = NULL;
            *released_state_out = open_state;
        }
    }

    return 0;
}

static int usfs_mapping_is_write (const uint64_t mapping_flags)
{
    return (mapping_flags & (SHM_RDONLY | SHM_COPY)) == 0;
}

static int has_mapping_reference (const struct usfs_open_state * open_state, const int is_write_mapping)
{
    return is_write_mapping ? open_state->write_mappings != 0 : open_state->read_mappings != 0;
}

static int can_retain_mapping_state (const struct usfs_node * node, const struct usfs_open_state * open_state, const int is_write_mapping)
{
    if (open_state->refs == UINT32_MAX)
        return false;

    if (node->mapping_count == UINT32_MAX)
        return false;

    if (is_write_mapping)
        return open_state->write_mappings != UINT32_MAX;

    return open_state->read_mappings != UINT32_MAX;
}

static int gnode_mapping_count_is_positive (const struct vnode * file_vnode, const int32long64_t mapping_flags)
{
    if ((mapping_flags & SHM_RDONLY) != 0)
        return file_vnode->v_gnode->gn_mrdcnt > 0;

    return file_vnode->v_gnode->gn_mwrcnt > 0;
}

static int can_release_mapping_state (
    const struct vnode * file_vnode,
    const struct usfs_node * node,
    const struct usfs_open_state * open_state,
    const int32long64_t mapping_flags
)
{
    if (open_state == NULL)
        return false;

    if (node->mapping_count == 0)
        return false;

    if (open_state->refs == 0)
        return false;

    if (!gnode_mapping_count_is_positive (file_vnode, mapping_flags))
        return false;

    return true;
}

static struct usfs_open_state * find_mapping_state (const struct usfs_node * node, const int32long64_t required_access, const int is_write_mapping)
{
    for (struct usfs_open_state * open_state = node->opens; open_state != NULL; open_state = open_state->next)
    {
        if (!open_state->active)
            continue;

        if ((open_state->flags & required_access) == 0)
            continue;

        return open_state;
    }

    for (struct usfs_open_state * open_state = node->opens; open_state != NULL; open_state = open_state->next)
    {
        if ((open_state->flags & required_access) == 0)
            continue;

        if (!has_mapping_reference (open_state, is_write_mapping))
            continue;

        return open_state;
    }

    return NULL;
}

static int usfs_retain_mapping_state (struct vnode * file_vnode, const uint64_t mapping_flags, struct usfs_open_state ** retained_state_out)
{
    struct usfs_node * node = _node_of (file_vnode);
    const int is_write_mapping = usfs_mapping_is_write (mapping_flags);
    const int32long64_t required_access = is_write_mapping ? FWRITE : FREAD;

    if (node == NULL)
        return EIO;

    if (retained_state_out != NULL)
        *retained_state_out = NULL;

    write_synchronized_with (global_lock)
    {
        struct usfs_open_state * open_state = find_mapping_state (node, required_access, is_write_mapping);
        if (open_state == NULL)
            return EBADF;

        if (!can_retain_mapping_state (node, open_state, is_write_mapping))
            return EOVERFLOW;

        open_state->refs += 1;

        if (is_write_mapping)
            open_state->write_mappings += 1;
        else
            open_state->read_mappings += 1;

        node->mapping_count += 1;

        usfs_vnode_hold_locked (node->vn, USFS_INSTRUMENT_NODE_REUSE_NONE);
        gn_mapcnt (file_vnode->v_gnode, (long)mapping_flags);

        if (retained_state_out != NULL)
            *retained_state_out = open_state;
    }

    return 0;
}

static struct usfs_open_state * find_mapped_handle (const struct usfs_node * node, const int is_write_mapping)
{
    for (struct usfs_open_state * open_state = node->opens; open_state != NULL; open_state = open_state->next)
    {
        if (has_mapping_reference (open_state, is_write_mapping))
            return open_state;
    }

    return NULL;
}

static struct usfs_open_state * drop_mapping_reference (
    struct vnode * file_vnode,
    struct usfs_node * node,
    struct usfs_open_state * open_state,
    const int32long64_t mapping_flags,
    const int is_write_mapping
)
{
    gn_unmapcnt (file_vnode->v_gnode, (long)mapping_flags);

    if (is_write_mapping)
        open_state->write_mappings -= 1;
    else
        open_state->read_mappings -= 1;

    open_state->refs -= 1;
    node->mapping_count -= 1;

    if (open_state->refs != 0)
        return NULL;

    struct usfs_open_state ** open_state_link = usfs_open_state_link_locked (node, open_state);

    *open_state_link = open_state->next;
    open_state->next = NULL;

    return open_state;
}

static int usfs_release_mapping_state (struct vnode * file_vnode, const int32long64_t mapping_flags, struct usfs_open_state ** state_to_release_out)
{
    struct usfs_node * node = _node_of (file_vnode);
    const int is_write_mapping = usfs_mapping_is_write ((uint64_t)mapping_flags);

    if (node == NULL)
        return EIO;

    if (state_to_release_out == NULL)
        return EIO;

    *state_to_release_out = NULL;

    write_synchronized_with (global_lock)
    {
        struct usfs_open_state * open_state = find_mapped_handle (node, is_write_mapping);
        if (!can_release_mapping_state (file_vnode, node, open_state, mapping_flags))
            return EINVAL;

        *state_to_release_out = drop_mapping_reference (file_vnode, node, open_state, mapping_flags, is_write_mapping);
    }

    return 0;
}

#endif
