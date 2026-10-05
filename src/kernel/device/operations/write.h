// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_WRITE_H
#define USFS_DEVICE_WRITE_H

#include "definitions.h"

struct reply_reception
{
    struct usfs_connection * connection; // Borrowed from the reference held by usfs_dev_write.
    struct usfs_reply_header header;     // Daemon reply metadata, zeroed before receiving the header.
    uint32_t body_length;                // Validated reply payload length in bytes.
    struct usfs_request * request;       // Detached request, owned by the writer until completion.
    int request_abandoned;               // Abandonment snapshot taken while claiming the request.
    int consume_remaining_input;         // Clear uio_resid after releasing the connection reference.
};

static int abort_reply_reception (const struct reply_reception * reply, const int rc)
{
    report_protocol_error (reply->connection, rc, reply->header.opcode, reply->header.id);
    abort_connection (reply->connection);

    return rc;
}

static int read_reply_header (struct reply_reception * reply, struct uio * user_io_request)
{
    struct usfs_reply_header * const header = &reply->header;

    if ((uint64_t)user_io_request->uio_resid < sizeof (*header))
        return EINVAL;

    const int rc = uiomove ((caddr_t)header, sizeof (*header), UIO_WRITE, user_io_request);
    if (rc != 0)
        return rc;

    if (!is_usfs_reply_header_valid (header))
        return EINVAL;

    const uint32_t reply_body_length = get_usfs_reply_body_length (header);

    if (!is_usfs_reply_body_valid (header, reply_body_length, (uint64_t)user_io_request->uio_resid))
        return EINVAL;

    reply->body_length = reply_body_length;

    return 0;
}

static void remove_request_from_waiting_queue (
    struct usfs_connection * connection,
    struct usfs_request * previous_request,
    struct usfs_request * request
)
{
    if (previous_request == NULL)
        connection->delivered_requests_head = request->next;
    else
        previous_request->next = request->next;

    request->next = NULL;
}

static struct usfs_request * find_request_by_id (
    const struct usfs_connection * connection,
    const uint64_t request_id,
    struct usfs_request ** output_previous_request
)
{
    struct usfs_request * previous_request = NULL;
    struct usfs_request * current_request = connection->delivered_requests_head;

    while (current_request != NULL)
    {
        if (current_request->id == request_id)
            break;

        previous_request = current_request;
        current_request = current_request->next;
    }

    *output_previous_request = previous_request;

    return current_request;
}

static int is_reply_valid_for_request (const struct reply_reception * reply, const struct usfs_request * request)
{
    const struct usfs_in_hdr * const request_header = (const struct usfs_in_hdr *)request->request_buffer;

    if (reply->header.opcode != request_header->opcode)
        return false;

    if (reply->body_length > request->max_allowed_reply_buffer_size)
        return false;

    return true;
}

static int claim_reply_request (struct reply_reception * reply)
{
    struct usfs_connection * const connection = reply->connection;

    synchronized_with (connection->lock)
    {
        if (connection->state == USFS_CONN_CLOSED)
            return EIO;

        struct usfs_request * previous_request = NULL;
        struct usfs_request * const request = find_request_by_id (connection, reply->header.id, &previous_request);

        if (request == NULL)
            return EINVAL;

        if (!is_reply_valid_for_request (reply, request))
            return EINVAL;

        remove_request_from_waiting_queue (connection, previous_request, request);

        if (!request->abandoned)
            request->state = USFS_REQ_REPLYING;

        reply->request_abandoned = request->abandoned;
        reply->request = request;

        return 0;
    }

    return EIO;
}

static void abort_reply_request (struct usfs_request * request)
{
    request->state = USFS_REQ_ABORTED;
    wake_request_up (request);
}

static int finish_receiving_reply (const struct reply_reception * reply, const int copy_rc)
{
    struct usfs_connection * const connection = reply->connection;
    struct usfs_request * const request = reply->request;

    synchronized_with (connection->lock)
    {
        if (request->abandoned)
        {
            return true;
        }

        if (copy_rc != 0)
        {
            abort_reply_request (request);
            return false;
        }

        if (connection->state != USFS_CONN_ACTIVE)
        {
            abort_reply_request (request);
            return false;
        }

        request->reply_buffer_size = reply->body_length;
        request->error = (int)reply->header.error;
        request->state = USFS_REQ_ANSWERED;
        wake_request_up (request);
        return false;
    }

    return false;
}

static int reply_transfers_ownership (const uint16_t opcode)
{
    switch (opcode)
    {
        case USFS_OP_LOOKUP:
        case USFS_OP_OPEN:
        case USFS_OP_CREATE:
        case USFS_OP_CREATE_ATTR:
        case USFS_OP_VGET:
            return true;

        default:
            return false;
    }
}

static int ownership_reply_valid (const struct usfs_request * request, const uint32_t length)
{
    const struct usfs_in_hdr * const request_header = (const struct usfs_in_hdr *)request->request_buffer;

    switch (request_header->opcode)
    {
        case USFS_OP_LOOKUP:
            if (length != sizeof (struct usfs_entry_out))
                return false;

            return usfs_entry_out_valid ((const struct usfs_entry_out *)request->reply_buffer);

        case USFS_OP_OPEN:
            if (length != sizeof (struct usfs_open_out))
                return false;

            return usfs_open_out_valid ((const struct usfs_open_out *)request->reply_buffer);

        case USFS_OP_CREATE:
            if (length != sizeof (struct usfs_create_out))
                return false;

            return usfs_create_out_valid ((const struct usfs_create_out *)request->reply_buffer);

        case USFS_OP_CREATE_ATTR:
            if (length != sizeof (struct usfs_create_out))
                return false;

            return usfs_create_attr_out_valid (
                (const struct usfs_create_out *)request->reply_buffer,
                ((const struct usfs_create_attr_in *)(request_header + 1))->activation
            );

        case USFS_OP_VGET:
            if (length != sizeof (struct usfs_vget_out))
                return false;

            if (request->request_buffer_size != sizeof (struct usfs_in_hdr) + sizeof (struct usfs_vget_in))
                return false;

            return usfs_vget_out_valid (
                (const struct usfs_vget_out *)request->reply_buffer,
                ((const struct usfs_vget_in *)(request_header + 1))->token
            );

        default:
            return true;
    }
}

static void release_abandoned_request (struct reply_reception * reply)
{
    synchronized_with (reply->connection->lock)
    {
        release_request_slot (reply->connection, reply->request);
    }

    free_request (reply->request);
    reply->consume_remaining_input = true;
}

static int copy_and_validate_reply_body (const struct reply_reception * reply, struct uio * user_io_request)
{
    int rc = 0;

    if (reply->body_length > 0)
        rc = usfs_kuiomove (USFS_UIOMOVE_REPLY_BODY, reply->request->reply_buffer, (long)reply->body_length, UIO_WRITE, user_io_request);

    if (reply->header.error != 0)
        return rc;

    if (!reply_transfers_ownership (reply->header.opcode))
        return rc;

    if (rc == 0)
    {
        if (ownership_reply_valid (reply->request, reply->body_length))
            return 0;

        rc = EINVAL;
    }

    return abort_reply_reception (reply, rc);
}

static int receive_claimed_reply (struct reply_reception * reply, struct uio * user_io_request)
{
    if (reply->request_abandoned)
    {
        release_abandoned_request (reply);
        return 0;
    }

    const int copy_rc = copy_and_validate_reply_body (reply, user_io_request);
    const int request_abandoned = finish_receiving_reply (reply, copy_rc);

    if (request_abandoned)
    {
        release_abandoned_request (reply);
        return copy_rc;
    }

    reply->consume_remaining_input = copy_rc == 0;

    return copy_rc;
}

static int is_reply_connection_closed (struct usfs_connection * connection)
{
    int closed = false;

    synchronized_with (connection->lock)
    {
        closed = connection->state == USFS_CONN_CLOSED;
    }

    return closed;
}

static int process_daemon_reply (struct reply_reception * reply, struct uio * user_io_request, const chan_t channel)
{
    if (is_reply_connection_closed (reply->connection))
        return EIO;

    int rc = read_reply_header (reply, user_io_request);
    if (rc != 0)
        return abort_reply_reception (reply, rc);

    rc = claim_reply_request (reply);
    if (rc == EIO)
        return rc;

    if (rc != 0)
    {
        return abort_reply_reception (reply, EINVAL);
    }

    USFS_TRACE_REQUEST5 (USFS_TRACE_REPLY_ACCEPTED, reply->header.id, reply->header.opcode, channel, reply->header.error, reply->body_length);

    return receive_claimed_reply (reply, user_io_request);
}

static int usfs_dev_write (const dev_t device_number, struct uio * user_io_request, const chan_t channel, const int is_called_from_kernel_extension)
{
    ignore_parameter device_number;                   //! todo: validate this parameter, reject open for not owning devices
    ignore_parameter is_called_from_kernel_extension; //! todo: research what this affects, decide if we should handle this parameter

    if (user_io_request == NULL)
        return EINVAL;

    struct usfs_connection * const connection = get_connection_by_channel (channel);
    if (connection == NULL)
        return ENXIO;

    acquire_connection (connection);

    struct reply_reception reply = { 0 };

    reply.connection = connection;

    const int rc = process_daemon_reply (&reply, user_io_request, channel);

    release_connection (connection);

    if (reply.consume_remaining_input)
        user_io_request->uio_resid = 0;

    return rc;
}

#endif
