// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"
#include "device/protocol/connection_refs.h"
#include "device/protocol/request_state.h"
#include "device/protocol/request_wait.h"

/* Request transport between kernel callers and user-space daemons. */

static void notify_reader (struct usfs_connection * connection)
{
    ushort readiness_events = POLLIN | POLLPRI | POLLHUP | POLLERR;

    if (connection->state == USFS_CONN_ACTIVE)
        readiness_events = POLLIN;
    else if (connection->state == USFS_CONN_CLOSED)
        readiness_events = POLLIN | POLLHUP;

    const ushort ready = connection->select_events & readiness_events;

    if (ready != 0)
    {
        connection->select_events &= ~ready;
        selnotify ((long)connection->device_number, connection->channel, ready);
    }
}

struct usfs_connection * get_connection_by_channel (const chan_t chan)
{
    if (!is_channel_valid (chan))
        return NULL;

    return g_connections[chan];
}

static uint32_t abort_queued_requests (
    struct usfs_connection * requests_owner,
    struct usfs_request * request,
    struct usfs_request ** requests_to_free
)
{
    uint32_t aborted_requests_count = 0;

    while (request != NULL)
    {
        struct usfs_request * const next_request = request->next;

        aborted_requests_count += 1u;

        if (request->abandoned)
        {
            release_request_slot (requests_owner, request);
            request->next = *requests_to_free;
            *requests_to_free = request;
        }
        else
        {
            request->state = USFS_REQ_ABORTED;
            wake_request_up (request);
        }

        request = next_request;
    }

    return aborted_requests_count;
}

static struct usfs_request * abort_connection_queues (struct usfs_connection * connection, const int state)
{
    const int old_state = connection->state;
    struct usfs_request * to_free_later_requests = NULL;

    connection->state = state;
    on_connection_failed (connection, old_state, state);

    // Abort pending requests
    const uint32_t num_of_aborted_pending_requests = abort_queued_requests (connection, connection->pending_requests_head, &to_free_later_requests);
    connection->pending_requests_head = NULL;
    connection->pending_requests_tail = NULL;

    // Abort delivered requests
    const uint32_t num_of_aborted_delivered_requests = abort_queued_requests (connection, connection->delivered_requests_head, &to_free_later_requests);
    connection->delivered_requests_head = NULL;

    // Report trace metrics and notify waiters in user space, so their 'read' on device file will get unblocked
    if (state == USFS_CONN_CLOSED)
    {
        USFS_TRACE_CONTROL5 (USFS_TRACE_CONNECTION, USFS_TRACE_ACTION_CLOSE, connection->channel, state, connection->outstanding_requests, 0);
    }
    else
    {
        USFS_TRACE_REQUEST5 (
            USFS_TRACE_CONNECTION_ABORT,
            connection->channel,
            old_state,
            state,
            num_of_aborted_pending_requests,
            num_of_aborted_delivered_requests
        );
    }

    e_wakeup (&connection->read_queue_event);
    notify_reader (connection);

    for (const struct usfs_request * waiter = connection->cleanup_waiters; waiter != NULL; waiter = waiter->next)
        wake_request_up (waiter);

    return to_free_later_requests;
}

static void free_requests_list (struct usfs_request * head)
{
    while (head != NULL)
    {
        struct usfs_request * request = head;
        head = head->next;
        request->next = NULL;
        free_request (request);
    }
}

struct usfs_call_wait_result
{
    int request_must_be_released;             // Caller owns final request deallocation.
    int timeout_fault_occurred;               // A timeout must be reported after unlocking.
    uint16_t timeout_opcode;                  // Opcode retained for timeout reporting.
    uint64_t timeout_unique;                  // Request identifier retained for timeout reporting.
    struct usfs_request * abandoned_requests; // Detached requests freed after unlocking.
};

struct request_call
{
    struct usfs_connection * connection;   // Connection held until all call cleanup finishes.
    struct usfs_request * request;         // Request whose final owner is recorded in result.
    struct usfs_in_hdr * header;           // Header in the request buffer, retained through the locked exchange.
    enum usfs_request_class request_class; // Admission and timeout policy for this call.
    struct trb * timer;                    // Allocated timer, stopped before freeing the pinned wait state.
    struct wait_control_block * wait;      // Pinned state reached by the timer interrupt.
    int timer_started;                     // Whether final cleanup must stop the timer.
    struct usfs_call_wait_result result;   // Cleanup ownership and fault reporting retained after unlocking.
};

static void on_call_finished (const struct request_call * call, const enum usfs_request_outcome outcome, const int error)
{
    on_request_processing_finished (call->request, outcome);
    USFS_TRACE_REQUEST5 (USFS_TRACE_REQUEST_COMPLETED, call->request->id, call->header->opcode, outcome, error, call->request->reply_buffer_size);
    USFS_TRACE_FS5 (USFS_TRACE_VNODE_RESULT, call->header->opcode, call->connection->channel, call->header->nodeid, call->request->id, error);
}

static void enqueue_request (const struct request_call * call)
{
    call->connection->outstanding_requests += 1;

    if (call->request->cleanup)
        call->connection->cleanup_outstanding = 1;

    call->request->counted = 1;
    on_request_processing_started (call->request, call->request_class, call->connection->outstanding_requests);

    call->request->id = call->connection->next_request_number++;
    call->header->unique = call->request->id;

    USFS_TRACE_REQUEST5 (
        USFS_TRACE_REQUEST_QUEUED,
        call->request->id,
        call->header->opcode,
        call->header->nodeid,
        call->request_class,
        call->connection->outstanding_requests
    );

    USFS_TRACE_FS5 (
        USFS_TRACE_VNODE_ENTRY,
        call->header->opcode,
        call->connection->channel,
        call->header->nodeid,
        call->request->id,
        call->request_class
    );

    call->request->state = USFS_REQ_PENDING;
    call->request->next = NULL;

    if (call->connection->pending_requests_tail != NULL)
        call->connection->pending_requests_tail->next = call->request;
    else
        call->connection->pending_requests_head = call->request;

    call->connection->pending_requests_tail = call->request;

    e_wakeup_one (&call->connection->read_queue_event);
    notify_reader (call->connection);
}

static int finish_answered_call (const struct request_call * call)
{
    const enum usfs_request_outcome outcome = call->request->error == 0 ? USFS_REQUEST_SUCCESS : USFS_REQUEST_DAEMON_ERROR;

    on_call_finished (call, outcome, call->request->error);
    release_request_slot (call->connection, call->request);

    return 0;
}

static int finish_aborted_call (struct request_call * call)
{
    on_call_finished (call, USFS_REQUEST_TRANSPORT_FAILURE, EIO);
    release_request_slot (call->connection, call->request);
    call->result.request_must_be_released = true;

    return EIO;
}

static int finish_timed_out_call (struct request_call * call)
{
    if (call->connection->state == USFS_CONN_ACTIVE)
        call->result.abandoned_requests = abort_connection_queues (call->connection, USFS_CONN_UNHEALTHY);

    /* Queued requests were detached above. A device copy runs outside the
     * queue lock and retains ownership until it observes this terminal state. */
    if (call->request->state == USFS_REQ_SENDING || call->request->state == USFS_REQ_REPLYING)
    {
        call->request->abandoned = true;
    }
    else
    {
        (void)remove_request_from_pending_queue (call->connection, call->request);
        release_request_slot (call->connection, call->request);
        call->result.request_must_be_released = true;
    }

    on_call_finished (call, USFS_REQUEST_TIMEOUT, ETIMEDOUT);
    call->result.timeout_fault_occurred = true;
    call->result.timeout_opcode = call->header->opcode;
    call->result.timeout_unique = call->request->id;

    return ETIMEDOUT;
}

static int finish_interrupted_call (struct request_call * call)
{
    const enum usfs_request_interrupt_disposition disposition = select_interrupt_strategy (call->request->state);

    if (disposition == USFS_REQUEST_INTERRUPT_RETRY)
        return 0;

    if (disposition == USFS_REQUEST_INTERRUPT_COMPLETE)
    {
        /* A successful reply may transfer an open handle or commit a mutation.
         * Retain its waiter and original timer; repeated signals must neither
         * discard that result nor keep an interruptible wait spinning. */
        call->request->wait->noninterruptible = true;
        return 0;
    }

    (void)remove_request_from_pending_queue (call->connection, call->request);
    release_request_slot (call->connection, call->request);
    call->result.request_must_be_released = true;
    on_call_finished (call, USFS_REQUEST_INTERRUPTED, EINTR);

    return EINTR;
}

static int wait_for_reply (struct request_call * call)
{
    while (true)
    {
        if (call->request->state == USFS_REQ_ANSWERED)
            return finish_answered_call (call);

        if (call->request->state == USFS_REQ_ABORTED)
            return finish_aborted_call (call);

        if (call->wait->timed_out)
            return finish_timed_out_call (call);

        if (wait_for_request_event (call->connection, call->wait) != THREAD_INTERRUPTED)
            continue;

        const int rc = finish_interrupted_call (call);
        if (rc != 0)
            return rc;
    }
}

static int allocate_call_wait_resources (struct request_call * call)
{
    call->timer = talloc ();
    if (call->timer == NULL)
        return ENOMEM;

    const int rc = allocate_wait_control_block (&call->wait);
    if (rc != 0)
    {
        tfree (call->timer);
        call->timer = NULL;
        return ENOMEM;
    }

    return 0;
}

static void enqueue_cleanup_waiter (const struct request_call * call)
{
    struct usfs_request ** tail = &call->connection->cleanup_waiters;

    while (*tail != NULL)
        tail = &(*tail)->next;

    call->request->next = NULL;
    *tail = call->request;
}

static int should_wait_for_cleanup_turn (const struct request_call * call)
{
    if (call->connection->state != USFS_CONN_ACTIVE)
        return false;

    if (call->wait->timed_out)
        return false;

    if (call->connection->cleanup_outstanding != 0)
        return true;

    return call->connection->cleanup_waiters != call->request;
}

static void remove_cleanup_waiter (const struct request_call * call)
{
    struct usfs_request ** link = &call->connection->cleanup_waiters;

    while (*link != call->request)
        link = &(*link)->next;

    *link = call->request->next;
    call->request->next = NULL;
}

static int wait_for_cleanup_turn (struct request_call * call)
{
    enqueue_cleanup_waiter (call);

    while (should_wait_for_cleanup_turn (call))
        (void)wait_for_request_event (call->connection, call->wait);

    remove_cleanup_waiter (call);

    if (!call->wait->timed_out && call->connection->state == USFS_CONN_ACTIVE)
        return 0;

    call->result.request_must_be_released = true;

    if (call->wait->timed_out && call->connection->state == USFS_CONN_ACTIVE)
    {
        call->result.abandoned_requests = abort_connection_queues (call->connection, USFS_CONN_UNHEALTHY);
        call->result.timeout_fault_occurred = true;
        call->result.timeout_opcode = call->header->opcode;
    }

    return call->wait->timed_out ? ETIMEDOUT : EIO;
}

static int validate_call_admission (struct request_call * call)
{
    if (call->connection->state != USFS_CONN_ACTIVE)
    {
        const int rc = EIO;
        call->result.request_must_be_released = true;

        USFS_TRACE_REQUEST5 (
            USFS_TRACE_REQUEST_REJECTED,
            call->header->opcode,
            call->header->nodeid,
            call->request_class,
            rc,
            call->connection->outstanding_requests
        );

        return rc;
    }

    if (call->request->cleanup)
        return 0;

    if (call->connection->outstanding_requests - call->connection->cleanup_outstanding < call->connection->max_outstanding_requests)
        return 0;

    const int rc = EAGAIN;

    call->result.request_must_be_released = true;
    on_request_rejected ();

    USFS_TRACE_REQUEST5 (
        USFS_TRACE_REQUEST_REJECTED,
        call->header->opcode,
        call->header->nodeid,
        call->request_class,
        rc,
        call->connection->outstanding_requests
    );

    return rc;
}

static void start_call_timer (struct request_call * call)
{
    const uint32_t timeout_ms = call->request_class == USFS_REQUEST_PAGER ? call->connection->pager_timeout_ms : call->connection->request_timeout_ms;

    configure_timer (call->timer, call->wait, timeout_ms);
    tstart (call->timer);
    call->timer_started = true;
}

static int run_call (struct request_call * call)
{
    call->request->wait = call->wait;

    /* Teardown has no surviving caller that could retry these ownership
     * releases. A pending process signal must not cancel their exchange. */
    call->request->cleanup = call->header->opcode == USFS_OP_FORGET || call->header->opcode == USFS_OP_RELEASE;
    call->wait->noninterruptible = call->request->cleanup;

    const int rc = validate_call_admission (call);
    if (rc != 0)
        return rc;

    start_call_timer (call);

    if (call->request->cleanup)
    {
        const int cleanup_wait_rc = wait_for_cleanup_turn (call);
        if (cleanup_wait_rc != 0)
            return cleanup_wait_rc;
    }

    enqueue_request (call);

    return wait_for_reply (call);
}

static void finish_call (const struct request_call * call)
{
    if (call->result.timeout_fault_occurred)
        report_timeout (call->connection, call->result.timeout_opcode, call->result.timeout_unique);

    if (call->timer_started)
    {
        while (tstop (call->timer) != 0)
            (void)delay (1); // todo: check this
    }

    tfree (call->timer);
    free_wait_control_block (call->wait);
    free_requests_list (call->result.abandoned_requests);

    if (call->result.request_must_be_released)
        free_request (call->request);
}

static int execute_call (struct request_call * call)
{
    acquire_connection (call->connection);

    int rc = EFAULT;

    synchronized_with (call->connection->lock)
    {
        rc = run_call (call);

        /* Device-side owners examine and wake this pointer only while holding
         * connection->lock. Detach it before tstop and before the pinned context is
         * freed; abandoned requests can then safely outlive their waiter. */
        call->request->wait = NULL;
    }

    finish_call (call);
    release_connection (call->connection);

    return rc;
}

int usfs_call_with_specific_request_class (
    struct usfs_connection * connection,
    struct usfs_request * request,
    const enum usfs_request_class request_class
)
{
    struct request_call call = { 0 };

    call.connection = connection;
    call.request = request;
    call.header = (struct usfs_in_hdr *)request->request_buffer;
    call.request_class = request_class;

    const int rc = allocate_call_wait_resources (&call);

    if (rc != 0)
    {
        USFS_TRACE_REQUEST5 (USFS_TRACE_REQUEST_REJECTED, call.header->opcode, call.header->nodeid, request_class, ENOMEM, 0);

        free_request (request);
        return rc;
    }

    return execute_call (&call);
}

int usfs_call (struct usfs_connection * connection, struct usfs_request * request)
{
    return usfs_call_with_specific_request_class (connection, request, USFS_REQUEST_NORMAL);
}

void abort_connection (struct usfs_connection * connection)
{
    struct usfs_request * free_head = NULL;

    synchronized_with (connection->lock)
    {
        if (connection->state != USFS_CONN_CLOSED)
            free_head = abort_connection_queues (connection, USFS_CONN_DEAD);
    }

    free_requests_list (free_head);
}

void wake_closed_connection (struct usfs_connection * connection)
{
    struct usfs_request * free_head = NULL;

    synchronized_with (connection->lock)
    {
        if (connection->state == USFS_CONN_CLOSED)
            free_head = abort_connection_queues (connection, USFS_CONN_CLOSED);
    }

    free_requests_list (free_head);
}
