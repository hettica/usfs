// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"
#include "fs/helpers.h"

int usfs_vmt_text_field (const struct vmount * mount_record, const int field_index, const char ** field_text, uint32_t * field_length)
{
    if (mount_record == NULL)
        return EINVAL;

    if (field_text == NULL)
        return EINVAL;

    if (field_length == NULL)
        return EINVAL;

    if (field_index < 0)
        return EINVAL;

    if (field_index > VMT_LASTINDEX)
        return EINVAL;

    if (mount_record->vmt_revision != VMT_REVISION)
        return EINVAL;

    if (mount_record->vmt_length < (uint32_t)sizeof (*mount_record))
        return EINVAL;

    const struct vmt_data * descriptor = &mount_record->vmt_data[field_index];
    if (!usfs_bounded_region_valid (
            mount_record->vmt_length,
            (int32_t)descriptor->vmt_off,
            (int32_t)descriptor->vmt_size,
            (uint32_t)sizeof (*mount_record)
        ))
        return EINVAL;

    *field_text = (const char *)mount_record + (uint32_t)descriptor->vmt_off;
    const uint32_t text_length =
        calculate_bounded_opcode_specific_argument_length (*field_text, (uint32_t)descriptor->vmt_size, (uint32_t)descriptor->vmt_size);
    if (descriptor->vmt_size == 0)
        return EINVAL;

    if (text_length != (uint32_t)descriptor->vmt_size)
        return EINVAL;

    *field_length = text_length;

    return 0;
}

int usfs_allocate_lock_occurrence (uint32_t * occurrence_bitmap, short * occurrence_out)
{
    for (unsigned occurrence = 0; occurrence < USFS_LOCK_OCCURRENCE_COUNT; occurrence++)
    {
        const unsigned word_index = occurrence / USFS_LOCK_OCCURRENCE_BITS_PER_WORD;
        const uint32_t occurrence_mask = (uint32_t)1u << (occurrence % USFS_LOCK_OCCURRENCE_BITS_PER_WORD);

        if ((occurrence_bitmap[word_index] & occurrence_mask) != 0)
            continue;

        occurrence_bitmap[word_index] |= occurrence_mask;
        *occurrence_out = (short)occurrence;

        return 0;
    }

    return EAGAIN;
}

void usfs_free_lock_occurrence (uint32_t * occurrence_bitmap, const short occurrence)
{
    if (occurrence < 0)
        return;

    const unsigned occurrence_index = (unsigned)occurrence;
    if (occurrence_index >= USFS_LOCK_OCCURRENCE_COUNT)
        return;

    const unsigned word_index = occurrence_index / USFS_LOCK_OCCURRENCE_BITS_PER_WORD;
    const uint32_t occurrence_mask = (uint32_t)1u << (occurrence_index % USFS_LOCK_OCCURRENCE_BITS_PER_WORD);
    occurrence_bitmap[word_index] &= ~occurrence_mask;
}

int usfs_vtype_from_mode (const uint32_t mode)
{
    enum
    {
        WIRE_FILE_TYPE_MASK = 0170000u,
        WIRE_FIFO = 0010000u,
        WIRE_CHARACTER_DEVICE = 0020000u,
        WIRE_DIRECTORY = 0040000u,
        WIRE_BLOCK_DEVICE = 0060000u,
        WIRE_SYMBOLIC_LINK = 0120000u,
        WIRE_SOCKET = 0140000u
    };

    switch (mode & WIRE_FILE_TYPE_MASK)
    {
        case WIRE_FIFO:
            return VFIFO;
        case WIRE_CHARACTER_DEVICE:
            return VCHR;
        case WIRE_DIRECTORY:
            return VDIR;
        case WIRE_BLOCK_DEVICE:
            return VBLK;
        case WIRE_SYMBOLIC_LINK:
            return VLNK;
        case WIRE_SOCKET:
            return VSOCK;
        default:
            return VREG;
    }
}

void usfs_attr_to_vattr (const struct usfs_attr * source_attributes, struct vattr * attributes)
{
    static const uint32_t default_block_size_bytes = 4096u;

    memset (attributes, 0, sizeof (*attributes));
    attributes->va_type = usfs_vtype_from_mode (source_attributes->mode);
    attributes->va_mode = (mode_t)source_attributes->mode;
    attributes->va_uid = (uid_t)source_attributes->uid;
    attributes->va_gid = (gid_t)source_attributes->gid;
    attributes->va_serialno = (ino_t)source_attributes->ino;
    attributes->va_nlink = (short)source_attributes->nlink;
    attributes->va_size = (off_t)source_attributes->size;
    attributes->va_blocksize = (long)(source_attributes->blksize != 0 ? source_attributes->blksize : default_block_size_bytes);
    attributes->va_blocks = (long)source_attributes->blocks;
    attributes->va_rdev = (dev_t)source_attributes->rdev;
    attributes->va_atime.tv_sec = source_attributes->atime;
    attributes->va_atime.tv_nsec = source_attributes->atimensec;
    attributes->va_mtime.tv_sec = source_attributes->mtime;
    attributes->va_mtime.tv_nsec = source_attributes->mtimensec;
    attributes->va_ctime.tv_sec = source_attributes->ctime;
    attributes->va_ctime.tv_nsec = source_attributes->ctimensec;
}

int usfs_create_attr_arguments_valid (
    struct vnode ** created_vnode,
    const struct vattr * attributes,
    const int32long64_t create_control,
    const struct ucred * credentials
)
{
    if (created_vnode == NULL)
        return false;

    if (attributes == NULL)
        return false;

    if (credentials == NULL)
        return false;

    if ((attributes->va_mask & (AT_TYPE | AT_MODE)) != (AT_TYPE | AT_MODE))
        return false;

    if (attributes->va_type != VREG)
        return false;

    if (create_control == VC_OPEN || create_control == VC_LOOKUP)
        return true;

    return create_control == VC_DEFAULT;
}
