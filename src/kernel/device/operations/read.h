// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_READ_H
#define USFS_DEVICE_READ_H

#include "device/protocol/request_state.h"
#include "device/protocol/request_wait.h"
#include "instrumentation/trace.h"
#include "synchronized.h"

#include <sys/sleep.h>
#include <sys/trcmacros.h>

static void enqueue_delivered_request (struct usfs_connection * connection, struct usfs_request * request)
{
    request->state = USFS_REQ_SENT;
    request->next = connection->delivered_requests_head;
    connection->delivered_requests_head = request;
}

static struct usfs_request * dequeue_pending_request (struct usfs_connection * connection)
{
    struct usfs_request * request = connection->pending_requests_head;

    connection->pending_requests_head = request->next;
    if (connection->pending_requests_head == NULL)
        connection->pending_requests_tail = NULL;

    request->next = NULL;
    request->state = USFS_REQ_SENDING;
    return request;
}

static int check_for_pending_requests (const struct usfs_connection * connection, const struct uio * user_io_request)
{
    if (connection->pending_requests_head == NULL)
        return EAGAIN;

    if ((uint64_t)user_io_request->uio_resid < (uint64_t)connection->pending_requests_head->request_buffer_size)
        return EMSGSIZE;

    return 0;
}

static int wait_for_pending_request (struct usfs_connection * connection)
{
    if (e_sleep_thread (&connection->read_queue_event, &connection->lock, LOCK_SIMPLE | INTERRUPTIBLE) == THREAD_INTERRUPTED)
        return EINTR;

    return 0;
}

static int take_pending_request (struct usfs_connection * connection, const struct uio * user_io_request, struct usfs_request ** usfs_request)
{
    synchronized_with (connection->lock)
    {
        while (true)
        {
            if (connection->state == USFS_CONN_CLOSED)
            {
                *usfs_request = NULL;
                return 0;
            }

            if (connection->state != USFS_CONN_ACTIVE)
                return EIO;

            const int check_result = check_for_pending_requests (connection, user_io_request);

            if (check_result == 0)
            {
                *usfs_request = dequeue_pending_request (connection);
                return 0;
            }

            if (check_result == EMSGSIZE)
                return EMSGSIZE;

            if (check_result == EAGAIN && user_io_request->uio_fmode & (FNDELAY | FNONBLOCK))
                return EAGAIN;

            const int wait_rc = wait_for_pending_request (connection);
            if (wait_rc != 0)
                return wait_rc;
        }
    }

    return EIO;
}

static int abort_delivery (struct usfs_request * request, const int copy_rc)
{
    request->state = USFS_REQ_ABORTED;
    wake_request_up (request);
    return copy_rc;
}

static int finish_abandoned_delivery (struct usfs_connection * connection, struct usfs_request * request, const int copy_rc)
{
    release_request_slot (connection, request);
    free_request (request);
    return copy_rc;
}

static int finish_request_delivery (struct usfs_connection * connection, struct usfs_request * request, const int copy_rc)
{
    synchronized_with (connection->lock)
    {
        if (request->abandoned)
            return finish_abandoned_delivery (connection, request, copy_rc);

        if (copy_rc != 0 || connection->state != USFS_CONN_ACTIVE)
            return abort_delivery (request, copy_rc);

        enqueue_delivered_request (connection, request);
        return 0;
    }

    return EIO;
}

static int usfs_dev_read (const dev_t device_number, struct uio * user_io_request, const chan_t channel, const int is_called_from_kernel_extension)
{
    ignore_parameter device_number;                   //! todo: validate this parameter, reject open for not owning devices
    ignore_parameter is_called_from_kernel_extension; //! todo: research what this affects, decide if we should handle this parameter

    if (user_io_request == NULL)
        return EINVAL;

    struct usfs_connection * const connection = get_connection_by_channel (channel);
    if (connection == NULL)
        return ENXIO;

    // todo: this method does not do allocation, but its pair 'release_connection' does deallocation; this is confusing; think how to make it clean
    acquire_connection (connection);

    struct usfs_request * usfs_request = NULL;
    const int rc = take_pending_request (connection, user_io_request, &usfs_request);
    if (rc != 0 || usfs_request == NULL)
    {
        release_connection (connection);
        return rc;
    }

    const struct usfs_in_hdr * const request_header = (const struct usfs_in_hdr *)usfs_request->request_buffer;
    const uint64_t request_id = usfs_request->id;
    const uint16_t request_opcode = request_header->opcode;
    const uint32_t request_length = usfs_request->request_buffer_size;
    const int copy_rc = uiomove (usfs_request->request_buffer, request_length, UIO_READ, user_io_request);

    USFS_TRACE_REQUEST5 (
        USFS_TRACE_REQUEST_DELIVERED,
        request_id,
        request_opcode,
        channel,
        request_length,
        copy_rc
    );

    const int delivery_rc = finish_request_delivery (connection, usfs_request, copy_rc);
    release_connection (connection);
    return delivery_rc;
}

#endif
