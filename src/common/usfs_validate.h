// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_VALIDATE_H
#define USFS_VALIDATE_H

#include "usfs_proto.h"
#include <stddef.h>

#define USFS_AIX_ERRNO_MAX 127

int usfs_u32_add_checked (uint32_t left, uint32_t right, uint32_t * result);
int usfs_u64_add_checked (uint64_t left, uint64_t right, uint64_t * result);
int usfs_u32_sub_checked (uint32_t left, uint32_t right, uint32_t * result);
int usfs_u64_sub_checked (uint64_t left, uint64_t right, uint64_t * result);
int usfs_size_to_u32_checked (size_t value, uint32_t * result);
int usfs_align8_checked (uint32_t value, uint32_t * result);
int usfs_bounded_region_valid (uint32_t total_length, int32_t offset, int32_t length, uint32_t minimum_offset);
int usfs_info_field_u64 (const char * info, uint32_t info_length, const char * key, uint32_t key_length, uint32_t base, uint64_t * result);
uint64_t calculate_request_buffer_size (uint32_t body_length, uint32_t name_length, uint32_t data_length);
int usfs_request_sizes_valid (uint32_t name_length, uint32_t data_length, uint32_t reply_capacity, uint64_t message_length);
int usfs_forget_in_valid (const struct usfs_forget_in * request, uint64_t owned);
int usfs_opcode_valid (uint16_t opcode);
int usfs_reply_errno_valid (int32_t error);
int usfs_nsec_valid (uint32_t nsec);
int usfs_nodeid_valid (uint64_t nodeid);
int usfs_data_size_valid (uint32_t size);
int usfs_offset_count_valid (uint64_t offset, uint32_t count);
int usfs_file_offset_count_valid (int64_t offset, uint32_t count);
int usfs_write_flags_valid (uint32_t flags);
int usfs_fsync_in_valid (const struct usfs_fsync_in * request);
int usfs_syncfs_in_valid (const struct usfs_syncfs_in * request);
int usfs_setattr_valid (const struct usfs_setattr_in * attributes);
int usfs_attr_valid (const struct usfs_attr * attributes);
uint32_t calculate_bounded_opcode_specific_argument_length (const char * name, uint32_t available, uint32_t maximum);
int usfs_out_header_valid (const struct usfs_reply_header * header, uint32_t available);
int usfs_dirent_valid (const struct usfs_dirent * entry, uint32_t available);
int usfs_entry_out_valid (const struct usfs_entry_out * reply);
int usfs_fid_out_valid (const struct usfs_fid_out * reply);
int usfs_vget_out_valid (const struct usfs_vget_out * reply, uint64_t requested_token);
int usfs_attr_out_valid (const struct usfs_attr_out * reply);
int usfs_create_out_valid (const struct usfs_create_out * reply);
int usfs_create_attr_in_valid (const struct usfs_create_attr_in * value);
int usfs_create_attr_out_valid (const struct usfs_create_out * reply, uint32_t activation);
int usfs_open_out_valid (const struct usfs_open_out * reply);
int usfs_write_out_valid (const struct usfs_write_out * reply, uint32_t requested);
int usfs_statfs_out_valid (const struct usfs_statfs_out * reply);
int usfs_readdir_reply_valid (const void * reply, uint32_t length);

#endif
