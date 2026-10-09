/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include "device/protocol/request_state.h"

#include <string.h>

static unsigned cleanup_wakes;
void wake_request_up(const struct usfs_request *request)
{ (void)request; ++cleanup_wakes; }

static void link_three(struct usfs_connection *connection, struct usfs_request *first,
                       struct usfs_request *second, struct usfs_request *third)
{
    first->next = second;
    second->next = third;
    third->next = NULL;
    connection->pending_requests_head = first;
    connection->pending_requests_tail = third;
}

int main(void)
{
    struct tap_state tap;
    struct usfs_connection connection;
    struct usfs_request first;
    struct usfs_request second;
    struct usfs_request third;
    struct usfs_request absent;

    tap_plan(&tap, 19);
    memset(&connection, 0, sizeof(connection));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&third, 0, sizeof(third));
    memset(&absent, 0, sizeof(absent));

    link_three(&connection, &first, &second, &third);
    tap_ok(&tap, remove_request_from_pending_queue(&connection, &second) == 1 &&
                 first.next == &third && connection.pending_requests_tail == &third &&
                 second.next == NULL,
           "pending middle request is unlinked");
    link_three(&connection, &first, &second, &third);
    tap_ok(&tap, remove_request_from_pending_queue(&connection, &first) == 1 &&
                 connection.pending_requests_head == &second &&
                 connection.pending_requests_tail == &third,
           "pending head request is unlinked");
    link_three(&connection, &first, &second, &third);
    tap_ok(&tap, remove_request_from_pending_queue(&connection, &third) == 1 &&
                 connection.pending_requests_tail == &second && second.next == NULL,
           "pending tail removal rebuilds the tail");
    memset(&connection, 0, sizeof(connection));
    connection.pending_requests_head = &first;
    connection.pending_requests_tail = &first;
    first.next = NULL;
    tap_ok(&tap, remove_request_from_pending_queue(&connection, &first) == 1 &&
                 connection.pending_requests_head == NULL &&
                 connection.pending_requests_tail == NULL,
           "only pending request removal empties the queue");
    link_three(&connection, &first, &second, &third);
    tap_ok(&tap, remove_request_from_pending_queue(&connection, &absent) == 0 &&
                 connection.pending_requests_head == &first &&
                 connection.pending_requests_tail == &third,
           "absent request leaves the queue unchanged");

    tap_ok(&tap, select_interrupt_strategy(USFS_REQ_PENDING) ==
                 USFS_REQUEST_INTERRUPT_RELEASE,
           "interrupted pending request returns to its waiter");
    tap_ok(&tap, select_interrupt_strategy(USFS_REQ_SENDING) ==
                 USFS_REQUEST_INTERRUPT_COMPLETE,
           "interrupted sending request retains its waiter until completion");
    tap_ok(&tap, select_interrupt_strategy(USFS_REQ_SENT) ==
                 USFS_REQUEST_INTERRUPT_COMPLETE,
           "interrupted sent request retains its waiter until completion");
    tap_ok(&tap, select_interrupt_strategy(USFS_REQ_REPLYING) ==
                 USFS_REQUEST_INTERRUPT_COMPLETE,
           "interrupted replying request retains its waiter until completion");
    tap_ok(&tap, select_interrupt_strategy(USFS_REQ_ANSWERED) ==
                 USFS_REQUEST_INTERRUPT_RETRY,
           "answered race is re-evaluated");
    tap_ok(&tap, select_interrupt_strategy(USFS_REQ_ABORTED) ==
                 USFS_REQUEST_INTERRUPT_RETRY,
           "aborted race is re-evaluated");
    tap_ok(&tap, select_interrupt_strategy(999) ==
                 USFS_REQUEST_INTERRUPT_RETRY,
           "unknown state is never released or abandoned");
    tap_ok(&tap, !is_channel_valid(-1),
           "negative device channel is rejected");
    tap_ok(&tap, is_channel_valid(USFS_MAX_CONNECTIONS - 1),
           "last device channel is accepted");
    tap_ok(&tap, !is_channel_valid(USFS_MAX_CONNECTIONS),
           "device channel beyond the table is rejected");
    connection.outstanding_requests = 2;
    first.counted = 1;
    release_request_slot(&connection, &first);
    tap_ok(&tap, connection.outstanding_requests == 1 && !first.counted,
           "counted request releases exactly one queue slot");
    release_request_slot(&connection, &first);
    tap_ok(&tap, connection.outstanding_requests == 1,
           "request queue slot release is idempotent");
    connection.outstanding_requests = 0;
    second.counted = 1;
    release_request_slot(&connection, &second);
    tap_ok(&tap, connection.outstanding_requests == 0 && !second.counted,
           "defensive slot release cannot underflow the queue count");
    connection.outstanding_requests = 2;
    connection.cleanup_outstanding = 1;
    connection.cleanup_waiters = &second;
    second.next = NULL;
    first.cleanup = first.counted = 1;
    release_request_slot(&connection, &first);
    tap_ok(&tap, connection.outstanding_requests == 1 &&
                 connection.cleanup_outstanding == 0 && cleanup_wakes == 1,
           "cleanup completion releases its reserve and wakes admission waiters");
    return tap_finish(&tap);
}
