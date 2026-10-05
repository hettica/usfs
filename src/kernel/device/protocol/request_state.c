// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "device/protocol/request_state.h"
#include "definitions.h"
#include "device/protocol/request_wait.h"

int is_channel_valid (const chan_t channel)
{
    if (channel < 0)
        return false;

    return channel < USFS_MAX_CONNECTIONS;
}

static struct usfs_request ** find_link_to_request (struct usfs_connection * connection, const struct usfs_request * request)
{
    struct usfs_request ** current_link = &connection->pending_requests_head;

    while (*current_link != NULL && *current_link != request)
        current_link = &(*current_link)->next;

    return current_link;
}

static struct usfs_request * find_queue_tail (struct usfs_request * first_request)
{
    struct usfs_request * last_request = first_request;

    while (last_request != NULL && last_request->next != NULL)
        last_request = last_request->next;

    return last_request;
}

int remove_request_from_pending_queue (struct usfs_connection * connection, struct usfs_request * request)
{
    if (connection == NULL)
        return false;

    if (request == NULL)
        return false;

    struct usfs_request ** const request_link = find_link_to_request (connection, request);
    if (*request_link != request)
        return false;

    const int removing_tail = connection->pending_requests_tail == request;

    *request_link = request->next;
    request->next = NULL;

    if (removing_tail)
        connection->pending_requests_tail = find_queue_tail (connection->pending_requests_head);

    return true;
}

void release_request_slot (struct usfs_connection * connection, struct usfs_request * request)
{
    if (!request->counted)
        return;

    request->counted = 0;

    if (connection->outstanding_requests > 0)
        connection->outstanding_requests -= 1;

    if (!request->cleanup)
        return;

    connection->cleanup_outstanding = 0;

    for (const struct usfs_request * waiter = connection->cleanup_waiters; waiter != NULL; waiter = waiter->next)
        wake_request_up (waiter);
}

enum usfs_request_interrupt_disposition select_interrupt_strategy (const int current_request_state)
{
    if (current_request_state == USFS_REQ_PENDING)
        return USFS_REQUEST_INTERRUPT_RELEASE;

    if (current_request_state == USFS_REQ_SENT || current_request_state == USFS_REQ_SENDING || current_request_state == USFS_REQ_REPLYING)
        return USFS_REQUEST_INTERRUPT_COMPLETE;

    return USFS_REQUEST_INTERRUPT_RETRY;
}
