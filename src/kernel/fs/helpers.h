// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_HELPERS_H
#define USFS_FS_HELPERS_H

#define USFS_LOCK_OCCURRENCE_COUNT         ((unsigned)SHRT_MAX)
#define USFS_LOCK_OCCURRENCE_BITS_PER_WORD ((unsigned)(sizeof (uint32_t) * CHAR_BIT))
#define USFS_LOCK_OCCURRENCE_WORDS         ((USFS_LOCK_OCCURRENCE_COUNT + USFS_LOCK_OCCURRENCE_BITS_PER_WORD - 1u) / USFS_LOCK_OCCURRENCE_BITS_PER_WORD)

int usfs_vmt_text_field (const struct vmount * mount_record, const int field_index, const char ** field_text, uint32_t * field_length);
int usfs_allocate_lock_occurrence (uint32_t * occurrence_bitmap, short * occurrence_out);
void usfs_free_lock_occurrence (uint32_t * occurrence_bitmap, const short occurrence);
int usfs_vtype_from_mode (const uint32_t mode);
void usfs_attr_to_vattr (const struct usfs_attr * source_attributes, struct vattr * attributes);
int usfs_create_attr_arguments_valid (
    struct vnode ** created_vnode,
    const struct vattr * attributes,
    const int32long64_t create_control,
    const struct ucred * credentials
);

#endif
