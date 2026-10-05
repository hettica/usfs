// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"

static const int memory_alignment_in_bytes = 4;

void free_request (struct usfs_request * request)
{
    if (request == NULL)
        return;

    if (request->request_buffer != NULL)
        xmfree (request->request_buffer, kernel_heap);

    if (request->reply_buffer != NULL)
        xmfree (request->reply_buffer, kernel_heap);

    xmfree (request, kernel_heap);
}

static int validate_request_allocation_spec (const struct usfs_request_allocation_spec * spec, const uint32_t opcode_specific_argument_length, const uint64_t request_buffer_size)
{
    if (spec == NULL)
        return EINVAL;

    if (!usfs_opcode_valid (spec->opcode))
        return EINVAL;

    if (!usfs_nodeid_valid (spec->node_id))
        return EINVAL;

    if (spec->opcode_specific_body.length > 0 && spec->opcode_specific_body.bytes == NULL)
        return EINVAL;

    if (spec->opcode_specific_argument != NULL && opcode_specific_argument_length == 0)
        return ENAMETOOLONG;

    if (!usfs_request_sizes_valid (opcode_specific_argument_length, spec->opcode_specific_payload.length, spec->max_allowed_reply_buffer_size, request_buffer_size))
        return EINVAL;

    return 0;
}

static uint32_t calculate_opcode_specific_argument_length (const struct usfs_request_allocation_spec * spec)
{
    if (spec == NULL)
        return 0u;

    if (spec->opcode_specific_argument == NULL)
        return 0u;

    return calculate_bounded_opcode_specific_argument_length (spec->opcode_specific_argument, USFS_MAX_NAME, USFS_MAX_NAME);
}

static int allocate_request_buffers (const struct usfs_request_allocation_spec * spec, struct usfs_request * const request, const uint32_t request_buffer_size)
{
    request->request_buffer = usfs_kmalloc (
        USFS_ALLOC_REQUEST_MESSAGE,
        request_buffer_size,
        memory_alignment_in_bytes,
        kernel_heap
    );

    if (request->request_buffer == NULL)
        return ENOMEM;

    request->request_buffer_size = request_buffer_size;

    if (spec->max_allowed_reply_buffer_size > 0)
    {
        request->reply_buffer = usfs_kmalloc (
            USFS_ALLOC_REQUEST_REPLY,
            spec->max_allowed_reply_buffer_size,
            memory_alignment_in_bytes,
            kernel_heap
        );

        if (request->reply_buffer == NULL)
            return ENOMEM;
    }

    request->max_allowed_reply_buffer_size = spec->max_allowed_reply_buffer_size;

    return 0;
}

static void initialize_request_header (const struct usfs_request_allocation_spec * spec, const struct usfs_request * const request, const uint32_t request_buffer_size)
{
    struct usfs_in_hdr * const header = (struct usfs_in_hdr *)request->request_buffer;

    memset (header, 0, sizeof (*header));

    header->len = request_buffer_size;
    header->version = (uint16_t)USFS_PROTOCOL_VERSION;
    header->opcode = spec->opcode;
    header->nodeid = spec->node_id;

    if (spec->credentials != NULL)
    {
        header->uid = (uint32_t)spec->credentials->cr_uid;
        header->gid = (uint32_t)spec->credentials->cr_gid;
        const pid_t caller_process_id = getpid ();
        if (caller_process_id > 0)
            header->pid = (uint32_t)caller_process_id;
    }
}

static void add_request_payload (const struct usfs_request_allocation_spec * spec, struct usfs_request * request, const uint32_t opcode_specific_argument_length)
{
    if (spec->opcode_specific_body.length > 0)
    {
        memcpy (
            request->request_buffer + sizeof (struct usfs_in_hdr),
            spec->opcode_specific_body.bytes,
            spec->opcode_specific_body.length
        );
    }

    if (opcode_specific_argument_length > 0)
    {
        memcpy (
            request->request_buffer + sizeof (struct usfs_in_hdr) + spec->opcode_specific_body.length,
            spec->opcode_specific_argument,
            opcode_specific_argument_length
        );
    }

    if (spec->opcode_specific_payload.length > 0 && spec->opcode_specific_payload.bytes != NULL)
    {
        memcpy (
            request->request_buffer + sizeof (struct usfs_in_hdr) + spec->opcode_specific_body.length + opcode_specific_argument_length,
            spec->opcode_specific_payload.bytes,
            spec->opcode_specific_payload.length
        );
    }
}

int allocate_request (const struct usfs_request_allocation_spec * spec, struct usfs_request ** output_value)
{
    // Validate output contract
    if (output_value == NULL)
        return EINVAL;

    *output_value = NULL;

    if (spec == NULL)
        return EINVAL;

    const uint32_t opcode_specific_argument_length = calculate_opcode_specific_argument_length (spec);
    const uint64_t calculated_request_size = calculate_request_buffer_size (
        spec->opcode_specific_body.length,
        opcode_specific_argument_length,
        spec->opcode_specific_payload.length
    );

    // Make sure all parameters are valid
    {
        const int rc = validate_request_allocation_spec (spec, opcode_specific_argument_length, calculated_request_size);
        if (rc != 0)
            return rc;
    }

    const uint32_t request_buffer_size = (uint32_t)calculated_request_size;

    struct usfs_request * request = usfs_kmalloc (
        USFS_ALLOC_REQUEST,
        sizeof (*request),
        memory_alignment_in_bytes,
        kernel_heap
    );

    if (request == NULL)
        return ENOMEM;

    memset (request, 0, sizeof (*request));

    {
        const int rc = allocate_request_buffers (spec, request, request_buffer_size);

        if (rc != 0)
        {
            free_request (request);
            return rc;
        }
    }

    initialize_request_header (spec, request, request_buffer_size);
    add_request_payload (spec, request, opcode_specific_argument_length);

    *output_value = request;
    return 0;
}

caddr_t usfs_msg_data (const struct usfs_request * request, const uint32_t body_length, const uint32_t name_length)
{
    return request->request_buffer + sizeof (struct usfs_in_hdr) + body_length + name_length;
}
