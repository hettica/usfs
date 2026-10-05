// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_REQUEST_BUILD_H
#define USFS_DEVICE_REQUEST_BUILD_H

#include <sys/types.h>

#ifndef _KERNEL
    #include <stdint.h>
#endif

struct ucred;
struct usfs_request;

struct usfs_buffer_view
{
    const void * bytes;
    uint32_t length;
};

struct usfs_request_allocation_spec
{
    uint16_t opcode;
    uint64_t node_id;
    struct usfs_buffer_view opcode_specific_body;
    const char * opcode_specific_argument;
    struct usfs_buffer_view opcode_specific_payload;
    uint32_t max_allowed_reply_buffer_size;
    const struct ucred * credentials;
};

void free_request (struct usfs_request * request);
int allocate_request (const struct usfs_request_allocation_spec * spec, struct usfs_request ** output_value);
caddr_t usfs_msg_data (const struct usfs_request * request, uint32_t body_length, uint32_t name_length);

#endif
