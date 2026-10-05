// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "usfs_validate.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

int usfs_u32_add_checked (const uint32_t left, const uint32_t right, uint32_t * result)
{
    if (result == NULL)
        return false;

    if (right > UINT32_MAX - left)
        return false;

    *result = left + right;
    return true;
}

int usfs_u64_add_checked (const uint64_t left, const uint64_t right, uint64_t * result)
{
    if (result == NULL)
        return false;

    if (right > UINT64_MAX - left)
        return false;

    *result = left + right;
    return true;
}

int usfs_u32_sub_checked (const uint32_t left, const uint32_t right, uint32_t * result)
{
    if (result == NULL)
        return false;

    if (right > left)
        return false;

    *result = left - right;
    return true;
}

int usfs_u64_sub_checked (const uint64_t left, const uint64_t right, uint64_t * result)
{
    if (result == NULL)
        return false;

    if (right > left)
        return false;

    *result = left - right;
    return true;
}

int usfs_size_to_u32_checked (const size_t value, uint32_t * result)
{
    if (result == NULL)
        return false;

    if (value > UINT32_MAX)
        return false;

    *result = (uint32_t)value;
    return true;
}

int usfs_align8_checked (const uint32_t value, uint32_t * result)
{
    uint32_t rounded_size;

    if (!usfs_u32_add_checked (value, 7u, &rounded_size))
        return false;

    if (result == NULL)
        return false;

    *result = rounded_size & ~7u;
    return true;
}

int usfs_bounded_region_valid (const uint32_t total_length, const int32_t offset, const int32_t length, const uint32_t minimum_offset)
{
    if (offset < 0)
        return false;

    if (length <= 0)
        return false;

    const uint32_t unsigned_offset = (uint32_t)offset;
    const uint32_t unsigned_length = (uint32_t)length;
    if (unsigned_offset < minimum_offset)
        return false;

    if (unsigned_offset > total_length)
        return false;

    return unsigned_length <= total_length - unsigned_offset;
}

static int parse_info_digit (const char character, const uint32_t base, uint32_t * digit)
{
    if (character >= '0' && character <= '9')
        *digit = (uint32_t)(character - '0');
    else if (base == 16u && character >= 'a' && character <= 'f')
        *digit = (uint32_t)(character - 'a') + 10u;
    else if (base == 16u && character >= 'A' && character <= 'F')
        *digit = (uint32_t)(character - 'A') + 10u;
    else
        return false;

    return *digit < base;
}

static int parse_info_u64_value (const char * value, const uint32_t available, const uint32_t base, uint64_t * result)
{
    uint64_t parsed_number = 0;
    uint32_t character_index = 0;

    while (character_index < available && value[character_index] != '\0' &&
           value[character_index] != ',')
    {
        uint32_t digit;

        if (!parse_info_digit (value[character_index], base, &digit))
            return -1;

        if (parsed_number > (UINT64_MAX - (uint64_t)digit) / (uint64_t)base)
            return -1;

        parsed_number = parsed_number * (uint64_t)base + (uint64_t)digit;
        character_index += 1u;
    }

    if (character_index == 0)
        return -1;

    if (character_index == available)
        return -1;

    *result = parsed_number;
    return 1;
}

static int validate_info_field_input (const char * info, const uint32_t info_length, const char * key, const uint32_t key_length, const uint32_t base, const uint64_t * result)
{
    if (info == NULL)
        return -1;

    if (info_length == 0)
        return -1;

    if (key == NULL)
        return -1;

    if (result == NULL)
        return -1;

    if (key_length == 0)
        return -1;

    if (base != 10u && base != 16u)
        return -1;

    const uint32_t terminated_info_length = calculate_bounded_opcode_specific_argument_length (info, info_length, info_length);
    if (terminated_info_length != info_length)
        return -1;

    return 0;
}

/*
 * Returns 1 when a well-formed field is found, 0 when it is absent, and -1
 * when the input or matching field is malformed.
 */
int usfs_info_field_u64 (const char * info, const uint32_t info_length, const char * key, const uint32_t key_length, const uint32_t base, uint64_t * result)
{
    uint32_t character_index = 0;

    if (validate_info_field_input (info, info_length, key, key_length, base, result) != 0)
        return -1;

    while (character_index < info_length && info[character_index] != '\0')
    {
        const uint32_t remaining = info_length - character_index;

        if (key_length < remaining &&
            memcmp (info + character_index, key, key_length) == 0 &&
            info[character_index + key_length] == '=')
            return parse_info_u64_value (
                info + character_index + key_length + 1u,
                remaining - key_length - 1u,
                base,
                result
            );

        while (character_index < info_length && info[character_index] != '\0' &&
               info[character_index] != ',')
            character_index += 1u;
        if (character_index < info_length && info[character_index] == ',')
            character_index += 1u;
    }

    return 0;
}

uint64_t calculate_request_buffer_size (const uint32_t body_length, const uint32_t name_length, const uint32_t data_length)
{
    return (uint64_t)sizeof (struct usfs_in_hdr) + body_length + name_length + data_length;
}

int usfs_request_sizes_valid (const uint32_t name_length, const uint32_t data_length, const uint32_t reply_capacity, const uint64_t message_length)
{
    if (name_length > USFS_MAX_NAME)
        return false;

    if (data_length > USFS_MAX_DATA)
        return false;

    if (reply_capacity > USFS_MSG_MAX)
        return false;

    return message_length <= USFS_MSG_MAX;
}

int usfs_forget_in_valid (const struct usfs_forget_in * request, const uint64_t owned_references)
{
    if (request == NULL)
        return false;

    if (request->count == 0)
        return false;

    return request->count <= owned_references;
}

int usfs_opcode_valid (const uint16_t opcode)
{
    if (opcode < (uint16_t)USFS_OP_LOOKUP)
        return false;

    return opcode <= (uint16_t)USFS_OP_VGET;
}

int usfs_reply_errno_valid (const int32_t error)
{
    if (error < 0)
        return false;

    return error <= USFS_AIX_ERRNO_MAX;
}

int usfs_nsec_valid (const uint32_t nanoseconds)
{
    return nanoseconds < 1000000000u;
}

int usfs_nodeid_valid (const uint64_t node_id)
{
    return node_id != 0;
}

int usfs_data_size_valid (const uint32_t size)
{
    return size <= USFS_MAX_DATA;
}

int usfs_offset_count_valid (const uint64_t offset, const uint32_t count)
{
    if (!usfs_data_size_valid (count))
        return false;

    uint64_t end_offset;
    return usfs_u64_add_checked (offset, (uint64_t)count, &end_offset);
}

int usfs_file_offset_count_valid (const int64_t offset, const uint32_t count)
{
    if (offset < 0)
        return false;

    if (!usfs_data_size_valid (count))
        return false;

    return (uint64_t)count <= (uint64_t)INT64_MAX - (uint64_t)offset;
}

int usfs_write_flags_valid (const uint32_t flags)
{
    return (flags & ~USFS_WRITE_APPEND) == 0;
}

int usfs_fsync_in_valid (const struct usfs_fsync_in * request)
{
    const uint32_t allowed_flags = USFS_FSYNC_DATASYNC | USFS_FSYNC_RANGE | USFS_FSYNC_DIRECTORY;

    if (request == NULL)
        return false;

    if ((request->flags & ~allowed_flags) != 0)
        return false;

    if (request->pad != 0)
        return false;

    if ((request->flags & USFS_FSYNC_RANGE) == 0)
    {
        if (request->offset != 0)
            return false;

        return request->length == 0;
    }

    if (request->offset > (uint64_t)INT64_MAX)
        return false;

    if (request->length > (uint64_t)INT64_MAX)
        return false;

    if (request->length == 0)
        return true;

    uint64_t range_end;
    if (!usfs_u64_add_checked (request->offset, request->length, &range_end))
        return false;

    return range_end <= (uint64_t)INT64_MAX;
}

int usfs_syncfs_in_valid (const struct usfs_syncfs_in * request)
{
    if (request == NULL)
        return false;

    if (request->pad != 0)
        return false;

    return request->mode <= USFS_SYNCFS_QUIESCE;
}

int usfs_setattr_valid (const struct usfs_setattr_in * attributes)
{
    const uint32_t allowed_attributes = USFS_SET_MODE | USFS_SET_UID | USFS_SET_GID |
                                        USFS_SET_SIZE | USFS_SET_ATIME | USFS_SET_MTIME |
                                        USFS_SET_CTIME | USFS_SET_TIMES_NOW;

    if (attributes == NULL)
        return false;

    if ((attributes->valid & ~allowed_attributes) != 0)
        return false;

    if (attributes->pad != 0)
        return false;

    if ((attributes->valid & USFS_SET_ATIME) != 0 &&
        !usfs_nsec_valid (attributes->atimensec))
        return false;

    if ((attributes->valid & USFS_SET_MTIME) != 0 &&
        !usfs_nsec_valid (attributes->mtimensec))
        return false;

    if ((attributes->valid & USFS_SET_CTIME) != 0 &&
        !usfs_nsec_valid (attributes->ctimensec))
        return false;

    return true;
}

int usfs_attr_valid (const struct usfs_attr * attributes)
{
    if (attributes == NULL)
        return false;

    if (attributes->ino == 0)
        return false;

    if (attributes->size > (uint64_t)INT64_MAX)
        return false;

    if (attributes->nlink > (uint32_t)SHRT_MAX)
        return false;

    if (attributes->blocks > (uint64_t)LONG_MAX)
        return false;

    if (attributes->blksize == 0)
        return false;

    if (attributes->pad != 0)
        return false;

    if (!usfs_nsec_valid (attributes->atimensec))
        return false;

    if (!usfs_nsec_valid (attributes->mtimensec))
        return false;

    return usfs_nsec_valid (attributes->ctimensec);
}

uint32_t calculate_bounded_opcode_specific_argument_length (const char * name, uint32_t available, const uint32_t maximum)
{
    if (name == NULL)
        return 0u;

    if (available == 0)
        return 0u;

    if (maximum == 0)
        return 0u;

    if (available > maximum)
        available = maximum;

    for (uint32_t character_index = 0; character_index < available; character_index++)
    {
        if (name[character_index] == '\0')
            return character_index + 1u;
    }

    return 0u;
}

int usfs_out_header_valid (const struct usfs_reply_header * header, const uint32_t available)
{
    if (header == NULL)
        return false;

    if (available < (uint32_t)sizeof (*header))
        return false;

    if (header->version != (uint16_t)USFS_PROTOCOL_VERSION)
        return false;

    if (!usfs_opcode_valid (header->opcode))
        return false;

    if (!usfs_reply_errno_valid (header->error))
        return false;

    if (header->pad != 0)
        return false;

    if (header->len < (uint32_t)sizeof (*header))
        return false;

    if (header->len > available)
        return false;

    if (header->error != 0 && header->len != (uint32_t)sizeof (*header))
        return false;

    return true;
}

int usfs_dirent_valid (const struct usfs_dirent * entry, const uint32_t available)
{
    if (entry == NULL)
        return false;

    if (available < (uint32_t)sizeof (*entry))
        return false;

    if (entry->namelen == 0)
        return false;

    if (entry->namelen >= USFS_MAX_NAME)
        return false;

    uint32_t minimum_entry_size;
    if (!usfs_u32_add_checked ((uint32_t)sizeof (*entry), (uint32_t)entry->namelen + 1u, &minimum_entry_size))
        return false;

    uint32_t expected_entry_size;
    if (!usfs_align8_checked (minimum_entry_size, &expected_entry_size))
        return false;

    if (expected_entry_size > UINT16_MAX)
        return false;

    if (entry->reclen != (uint16_t)expected_entry_size)
        return false;

    if (expected_entry_size > available)
        return false;

    const uint32_t name_length = (uint32_t)entry->namelen;
    if (((const char *)(entry + 1))[name_length] != '\0')
        return false;

    return true;
}

int usfs_entry_out_valid (const struct usfs_entry_out * reply)
{
    if (reply == NULL)
        return false;

    if (!usfs_nodeid_valid (reply->nodeid))
        return false;

    return usfs_attr_valid (&reply->attr);
}

int usfs_fid_out_valid (const struct usfs_fid_out * reply)
{
    if (reply == NULL)
        return false;

    return reply->token != 0;
}

int usfs_vget_out_valid (const struct usfs_vget_out * reply, const uint64_t requested_token)
{
    if (reply == NULL)
        return false;

    if (requested_token == 0 || reply->token != requested_token)
        return false;

    return usfs_entry_out_valid (&reply->entry);
}

int usfs_attr_out_valid (const struct usfs_attr_out * reply)
{
    if (reply == NULL)
        return false;

    if (!usfs_attr_valid (&reply->attr))
        return false;

    return usfs_nodeid_valid (reply->parent);
}

int usfs_create_out_valid (const struct usfs_create_out * reply)
{
    if (reply == NULL)
        return false;

    if (!usfs_nodeid_valid (reply->nodeid))
        return false;

    return usfs_attr_valid (&reply->attr);
}

int usfs_open_out_valid (const struct usfs_open_out * reply)
{
    if (reply == NULL)
        return false;

    return reply->pad == 0;
}

int usfs_create_attr_in_valid (const struct usfs_create_attr_in * request)
{
    const uint32_t allowed_attributes = USFS_SET_MODE | USFS_SET_UID | USFS_SET_GID |
                                        USFS_SET_SIZE | USFS_SET_ATIME | USFS_SET_MTIME | USFS_SET_CTIME;

    if (request == NULL)
        return false;

    if (request->activation > USFS_CREATE_OPEN)
        return false;

    const struct usfs_setattr_in * attributes = &request->attr;
    if (attributes->fh != 0)
        return false;

    if (attributes->pad != 0)
        return false;

    if ((attributes->valid & ~allowed_attributes) != 0)
        return false;

    if ((attributes->valid & USFS_SET_MODE) == 0)
        return false;

    if ((attributes->mode & ~07777u) != 0)
        return false;

    if (attributes->size > (uint64_t)INT64_MAX)
        return false;

    if (attributes->atimensec >= 1000000000u)
        return false;

    if (attributes->mtimensec >= 1000000000u)
        return false;

    return attributes->ctimensec < 1000000000u;
}

int usfs_create_attr_out_valid (const struct usfs_create_out * reply, const uint32_t activation)
{
    if (reply == NULL)
        return false;

    if (activation > USFS_CREATE_OPEN)
        return false;

    if (!usfs_attr_valid (&reply->attr))
        return false;

    if ((reply->attr.mode & 0170000u) != 0100000u)
        return false;

    if (activation == USFS_CREATE_DEFAULT)
    {
        if (reply->nodeid != 0)
            return false;
    }
    else if (reply->nodeid <= USFS_ROOT_ID)
    {
        return false;
    }

    if (activation == USFS_CREATE_OPEN)
        return true;

    return reply->fh == 0;
}

int usfs_write_out_valid (const struct usfs_write_out * reply, const uint32_t requested_bytes)
{
    if (reply == NULL)
        return false;

    if (reply->pad != 0)
        return false;

    if (reply->written > requested_bytes)
        return false;

    if (reply->offset > (uint64_t)INT64_MAX)
        return false;

    return (uint64_t)reply->written <= (uint64_t)INT64_MAX - reply->offset;
}

int usfs_statfs_out_valid (const struct usfs_statfs_out * reply)
{
    if (reply == NULL)
        return false;

    if (reply->bsize == 0)
        return false;

    if (reply->bfree > reply->blocks)
        return false;

    if (reply->bavail > reply->bfree)
        return false;

    if (reply->ffree > reply->files)
        return false;

    if (reply->namemax <= 0)
        return false;

    return reply->namemax < USFS_MAX_NAME;
}

static int read_directory_entry_size (const unsigned char * cursor, const uint32_t remaining, uint32_t * entry_size)
{
    struct usfs_dirent entry;

    if (remaining < (uint32_t)sizeof (entry))
        return false;

    memcpy (&entry, cursor, sizeof (entry));
    if (!usfs_nodeid_valid (entry.ino))
        return false;

    if (entry.namelen == 0)
        return false;

    if (entry.namelen >= USFS_MAX_NAME)
        return false;

    uint32_t minimum_entry_size;
    if (!usfs_u32_add_checked ((uint32_t)sizeof (entry), (uint32_t)entry.namelen + 1u, &minimum_entry_size))
        return false;

    if (!usfs_align8_checked (minimum_entry_size, entry_size))
        return false;

    if (*entry_size > UINT16_MAX)
        return false;

    if (entry.reclen != (uint16_t)*entry_size)
        return false;

    if (*entry_size > remaining)
        return false;

    if (cursor[sizeof (entry) + entry.namelen] != '\0')
        return false;

    return true;
}

int usfs_readdir_reply_valid (const void * reply, const uint32_t length)
{
    struct usfs_readdir_out header;

    if (reply == NULL)
        return false;

    if (length < (uint32_t)sizeof (header))
        return false;

    memcpy (&header, reply, sizeof (header));
    if (header.pad != 0)
        return false;

    if (header.snapshot_id == 0 || header.snapshot_id > USFS_DIRECTORY_CURSOR_ID_MAX)
        return false;

    const unsigned char * cursor = (const unsigned char *)reply + sizeof (header);
    uint32_t remaining = length - (uint32_t)sizeof (header);
    for (uint32_t entry_index = 0; entry_index < header.count; entry_index++)
    {
        uint32_t expected_entry_size;
        if (!read_directory_entry_size (cursor, remaining, &expected_entry_size))
            return false;

        cursor += expected_entry_size;
        remaining -= expected_entry_size;
    }

    return remaining == 0;
}
