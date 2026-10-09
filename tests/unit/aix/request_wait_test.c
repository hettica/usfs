/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include "device/protocol/request_wait.h"
#include "device/protocol/request.c"

#include <string.h>

heapaddr_t pinned_heap;
heapaddr_t kernel_heap;
Simple_lock g_connections_table_lock;
struct usfs_connection *g_connections[USFS_MAX_CONNECTIONS];

static struct wait_control_block wait_storage;
static enum usfs_allocation_site allocation_site;
static heapaddr_t allocation_heap;
static heapaddr_t free_heap;
static int allocation_fails;
static unsigned wake_calls;
static tid_t *last_event;
static unsigned assert_calls;
static unsigned clear_calls;
static unsigned block_calls;
static unsigned unlock_calls;
static unsigned lock_calls;
static int lock_depth;
static int block_result;
static struct trb *fire_during_assert;
static boolean_t last_interruptible;
static struct trb call_timer;
static unsigned timer_starts, timer_stops, timer_frees, request_frees;
static unsigned finished_calls;
static enum usfs_request_outcome finished_outcome;
static struct usfs_connection *script_connection;
static struct usfs_request *script_request;
static int script_state, script_finish, script_ok;
static uint16_t cleanup_opcode;
static int reserve_scenario;
static struct usfs_request occupied_cleanup;
static ushort selected_events;

struct trb *talloc(void) { return &call_timer; }
void tfree(struct trb *timer) { (void)timer; timer_frees += 1; }
void tstart(struct trb *timer) { (void)timer; timer_starts += 1; }
int tstop(struct trb *timer) { (void)timer; timer_stops += 1; return 0; }
int delay(int ticks) { (void)ticks; return 0; }
void lock_free(void *lock) { (void)lock; }
void free_request(struct usfs_request *request)
{
    (void)request;
    request_frees += 1;
}
void e_wakeup_one(tid_t *event) { e_wakeup(event); }
void selnotify(long device, int channel, ushort events)
{ (void)device; (void)channel; selected_events |= events; }
void on_request_processing_started(struct usfs_request *request,
    enum usfs_request_class request_class, uint32_t outstanding)
{ (void)request; (void)request_class; (void)outstanding; }
void on_request_processing_finished(struct usfs_request *request,
    enum usfs_request_outcome outcome)
{ (void)request; finished_calls += 1; finished_outcome = outcome; }
void on_request_rejected(void) { }
void on_connection_failed(struct usfs_connection *connection, int old, int state)
{ (void)connection; (void)old; (void)state; }
void report_timeout(const struct usfs_connection *connection,
    uint16_t opcode, uint64_t id)
{ (void)connection; (void)opcode; (void)id; }

void *usfs_kmalloc(enum usfs_allocation_site site, uint size, int align,
                   heapaddr_t heap)
{
    (void)size;
    (void)align;
    allocation_site = site;
    allocation_heap = heap;
    if (allocation_fails)
        return NULL;
    return &wait_storage;
}

int xmfree(void *allocation, heapaddr_t heap)
{
    (void)allocation;
    free_heap = heap;
    return 0;
}

void e_wakeup(tid_t *event)
{
    wake_calls += 1;
    last_event = event;
}

void e_assert_wait(tid_t *event, boolean_t interruptible)
{
    (void)event;
    last_interruptible = interruptible;
    assert_calls += 1;
    if (fire_during_assert != NULL)
        fire_during_assert->func(fire_during_assert);
}

void e_clear_wait(tid_t tid, int result)
{
    (void)tid;
    (void)result;
    clear_calls += 1;
}

tid_t thread_self(void)
{
    return (tid_t)77;
}

int e_block_thread(void)
{
    block_calls += 1;
    if (reserve_scenario) {
        script_ok &= !last_interruptible && timer_starts == 1 && timer_stops == 0;
        if (!script_request->counted) {
            script_ok &= script_connection->cleanup_waiters == script_request;
            if (reserve_scenario == 3) call_timer.func(&call_timer);
            else if (reserve_scenario == 4) abort_connection(script_connection);
            else release_request_slot(script_connection, &occupied_cleanup);
        } else {
            script_ok &= script_connection->outstanding_requests == 2 &&
                         script_connection->cleanup_outstanding == 1;
            script_connection->pending_requests_head = NULL;
            script_connection->pending_requests_tail = NULL;
            script_request->state = USFS_REQ_ANSWERED;
        }
        return THREAD_AWAKENED;
    }
    if (script_request != NULL) {
        if (block_calls == 1) {
            script_ok &= cleanup_opcode ? !last_interruptible : last_interruptible != 0;
            if (script_state != USFS_REQ_PENDING) {
                script_connection->pending_requests_head = NULL;
                script_connection->pending_requests_tail = NULL;
                script_request->state = script_state;
                if (script_state == USFS_REQ_SENT)
                    script_connection->delivered_requests_head = script_request;
            }
            return cleanup_opcode ? THREAD_AWAKENED : THREAD_INTERRUPTED;
        }
        script_ok &= !last_interruptible &&
            script_request->wait == &wait_storage &&
            !script_request->abandoned && timer_starts == 1 &&
            timer_stops == 0 && request_frees == 0 &&
            script_connection->outstanding_requests == 1;
        if (block_calls < 4)
            return THREAD_AWAKENED;
        if (script_finish == USFS_REQ_ANSWERED) {
            script_connection->pending_requests_head = NULL;
            script_connection->pending_requests_tail = NULL;
            script_connection->delivered_requests_head = NULL;
            script_request->state = USFS_REQ_ANSWERED;
            script_request->reply_buffer_size = 8;
        } else if (script_finish == USFS_REQ_ABORTED) {
            abort_connection(script_connection);
            /* A device copy owner observes the disconnect and wakes its
             * attached waiter after relinquishing the buffer. */
            script_request->state = USFS_REQ_ABORTED;
        } else {
            call_timer.func(&call_timer);
        }
        return THREAD_AWAKENED;
    }
    return block_result;
}

void simple_unlock(simple_lock_t lock)
{
    (void)lock;
    unlock_calls += 1;
    lock_depth -= 1;
}

void simple_lock(simple_lock_t lock)
{
    (void)lock;
    lock_calls += 1;
    lock_depth += 1;
}

static void reset_state(void)
{
    memset(&wait_storage, 0, sizeof(wait_storage));
    allocation_site = 0;
    allocation_heap = NULL;
    free_heap = NULL;
    allocation_fails = 0;
    wake_calls = 0;
    last_event = NULL;
    assert_calls = 0;
    clear_calls = 0;
    block_calls = 0;
    unlock_calls = 0;
    lock_calls = 0;
    lock_depth = 0;
    block_result = THREAD_AWAKENED;
    fire_during_assert = NULL;
    last_interruptible = 0;
    memset(&call_timer, 0, sizeof(call_timer));
    timer_starts = timer_stops = timer_frees = request_frees = 0;
    finished_calls = 0;
    script_connection = NULL;
    script_request = NULL;
    script_state = script_finish = 0;
    script_ok = 1;
    reserve_scenario = 0;
}

static void check_call(struct tap_state *tap, int state, int finish,
                       int daemon_error, const char *name)
{
    struct usfs_connection connection;
    struct usfs_request request;
    struct usfs_in_hdr header;
    int expected, rc, copied;

    reset_state();
    memset(&connection, 0, sizeof(connection));
    memset(&request, 0, sizeof(request));
    memset(&header, 0, sizeof(header));
    connection.state = USFS_CONN_ACTIVE;
    connection.refs_counter = 1;
    connection.max_outstanding_requests = 1;
    connection.request_timeout_ms = 1234;
    header.opcode = cleanup_opcode ? cleanup_opcode : USFS_OP_OPEN;
    header.nodeid = 2;
    request.request_buffer = (char *)&header;
    request.request_buffer_size = sizeof(header);
    request.error = daemon_error;
    script_connection = &connection;
    script_request = &request;
    script_state = state;
    script_finish = finish;
    rc = usfs_call(&connection, &request);
    expected = state == USFS_REQ_PENDING && !cleanup_opcode ? EINTR :
        finish == USFS_REQ_ANSWERED ? 0 :
        finish == USFS_REQ_ABORTED ? EIO : ETIMEDOUT;
    copied = state != USFS_REQ_PENDING && finish == 0 &&
        (state == USFS_REQ_SENDING || state == USFS_REQ_REPLYING);
    script_ok &= rc == expected && lock_depth == 0 &&
        connection.refs_counter == 1 && request.wait == NULL &&
        timer_starts == 1 && timer_stops == 1 && timer_frees == 1 &&
        call_timer.timeout.it_value.tv_sec == 1 &&
        call_timer.timeout.it_value.tv_nsec == 234000000L &&
        finished_calls == 1 &&
        block_calls == (state == USFS_REQ_PENDING && !cleanup_opcode ? 1u : 4u);
    if (copied) {
        script_ok &= request.abandoned && request_frees == 0 &&
            connection.state == USFS_CONN_UNHEALTHY &&
            connection.outstanding_requests == 1;
    } else {
        script_ok &= !request.abandoned &&
            connection.outstanding_requests == 0 &&
            request_frees == (rc == 0 ? 0u : 1u);
    }
    if (rc == 0)
        script_ok &= request.reply_buffer_size == 8 &&
            request.error == daemon_error && finished_outcome ==
                (daemon_error ? USFS_REQUEST_DAEMON_ERROR : USFS_REQUEST_SUCCESS);
    if (rc == ETIMEDOUT)
        script_ok &= finished_outcome == USFS_REQUEST_TIMEOUT;
    tap_ok(tap, script_ok, name);
}

int main(void)
{
    struct tap_state tap;
    struct wait_control_block *wait = NULL;
    struct usfs_connection conn;
    struct usfs_request req;
    struct trb timer;
    int rc;

    tap_plan(&tap, 35);
    pinned_heap = (heapaddr_t)(ulong)0x1234;

    reset_state();
    tap_ok(&tap, allocate_wait_control_block(NULL) == EINVAL,
           "wait allocation requires output storage");
    allocation_fails = 1;
    tap_ok(&tap, allocate_wait_control_block(&wait) == ENOMEM && wait == NULL,
           "pinned wait allocation failure is reported");

    reset_state();
    tap_ok(&tap, allocate_wait_control_block(&wait) == 0 &&
                     wait == &wait_storage && wait->event == EVENT_NULL &&
                     allocation_site == USFS_ALLOC_REQUEST_WAIT &&
                     allocation_heap == pinned_heap,
           "wait context is initialized on the pinned heap");

    memset(&timer, 0, sizeof(timer));
    configure_timer(&timer, wait, 1234);
    tap_ok(&tap, timer.func_data == (ulong)wait && timer.func != NULL &&
                     timer.timeout.it_value.tv_sec == 1 &&
                     timer.timeout.it_value.tv_nsec == 234000000L &&
                     timer.ipri == INTTIMER,
           "timer references only the pinned wait context");
    timer.func(&timer);
    tap_ok(&tap, wait->timed_out == 1 && wake_calls == 1 &&
                     last_event == &wait->event,
           "timer interrupt publishes timeout through the pinned event");

    memset(&req, 0, sizeof(req));
    req.wait = wait;
    wake_request_up(&req);
    req.wait = NULL;
    wake_request_up(&req);
    tap_ok(&tap, wake_calls == 2,
           "device completion wakes only an attached waiter");

    reset_state();
    memset(&conn, 0, sizeof(conn));
    memset(&timer, 0, sizeof(timer));
    wait = &wait_storage;
    configure_timer(&timer, wait, 1);
    fire_during_assert = &timer;
    lock_depth = 1;
    rc = wait_for_request_event(&conn, wait);
    tap_ok(&tap, rc == THREAD_AWAKENED && wait->timed_out == 1,
           "timeout racing with wait registration is observed");
    tap_ok(&tap, assert_calls == 1 && clear_calls == 1 &&
                     block_calls == 0 && unlock_calls == 0 &&
                     lock_depth == 1,
           "registered timeout race clears the wait without blocking");

    reset_state();
    memset(&conn, 0, sizeof(conn));
    lock_depth = 1;
    rc = wait_for_request_event(&conn, &wait_storage);
    tap_ok(&tap, rc == THREAD_AWAKENED && assert_calls == 1 &&
                     unlock_calls == 1 && block_calls == 1 &&
                     lock_calls == 1 && lock_depth == 1,
           "ordinary wait registers, unlocks, blocks, and reacquires");

    reset_state();
    memset(&conn, 0, sizeof(conn));
    lock_depth = 1;
    block_result = THREAD_INTERRUPTED;
    tap_ok(&tap, wait_for_request_event(&conn, &wait_storage) ==
                     THREAD_INTERRUPTED && lock_depth == 1,
           "interruptible wait preserves signal return and lock ownership");

    free_wait_control_block(&wait_storage);
    tap_ok(&tap, free_heap == pinned_heap,
           "wait context is returned to the pinned heap");
    free_heap = NULL;
    free_wait_control_block(NULL);
    tap_ok(&tap, free_heap == NULL, "free accepts a null wait context");

    check_call(&tap, USFS_REQ_PENDING, 0, 0,
        "pending signal cancels immediately and releases the only queue slot");
    check_call(&tap, USFS_REQ_SENDING, USFS_REQ_ANSWERED, 0,
        "signal during delivery retains waiter and original timer through reply");
    check_call(&tap, USFS_REQ_SENT, USFS_REQ_ANSWERED, 0,
        "signal after delivery retains waiter and original timer through reply");
    check_call(&tap, USFS_REQ_REPLYING, USFS_REQ_ANSWERED, 0,
        "signal during reply copy returns the actual reply without abandonment");
    check_call(&tap, USFS_REQ_SENT, USFS_REQ_ANSWERED, EACCES,
        "signal does not replace the daemon error with EINTR");
    check_call(&tap, USFS_REQ_SENDING, 0, 0,
        "delivery timeout retains copy ownership and quarantines the connection");
    check_call(&tap, USFS_REQ_SENT, 0, 0,
        "delivered timeout releases the queue slot after terminal recovery");
    check_call(&tap, USFS_REQ_REPLYING, 0, 0,
        "reply-copy timeout retains copy ownership through terminal recovery");
    check_call(&tap, USFS_REQ_SENDING, USFS_REQ_ABORTED, 0,
        "disconnect after delivery signal completes with EIO");
    check_call(&tap, USFS_REQ_SENT, USFS_REQ_ABORTED, 0,
        "disconnect after sent signal completes with EIO");
    check_call(&tap, USFS_REQ_REPLYING, USFS_REQ_ABORTED, 0,
        "disconnect after reply-copy signal completes with EIO");

    for (cleanup_opcode = USFS_OP_RELEASE; cleanup_opcode != 0;
         cleanup_opcode = cleanup_opcode == USFS_OP_RELEASE ? USFS_OP_FORGET : 0) {
        check_call(&tap, USFS_REQ_PENDING, USFS_REQ_ANSWERED, 0,
            "pending ownership release ignores signals and acknowledges within the original deadline");
        check_call(&tap, USFS_REQ_PENDING, 0, 0,
            "unanswered pending ownership release still times out and quarantines the connection");
        check_call(&tap, USFS_REQ_PENDING, USFS_REQ_ABORTED, 0,
            "disconnect wakes noninterruptible ownership cleanup and releases its queue slot");
    }
    for (int scenario = 1; scenario <= 4; ++scenario) {
        struct usfs_in_hdr header;
        reset_state();
        memset(&conn, 0, sizeof(conn));
        memset(&req, 0, sizeof(req));
        memset(&header, 0, sizeof(header));
        memset(&occupied_cleanup, 0, sizeof(occupied_cleanup));
        conn.state = USFS_CONN_ACTIVE;
        conn.refs_counter = 1;
        conn.max_outstanding_requests = conn.outstanding_requests = 1;
        conn.request_timeout_ms = 1234;
        if (scenario > 1) {
            conn.cleanup_outstanding = 1;
            conn.outstanding_requests++;
            occupied_cleanup.counted = occupied_cleanup.cleanup = 1;
        }
        header.opcode = USFS_OP_RELEASE;
        header.nodeid = 2;
        req.request_buffer = (char *)&header;
        req.request_buffer_size = sizeof(header);
        script_connection = &conn;
        script_request = &req;
        reserve_scenario = scenario;
        rc = usfs_call(&conn, &req);
        tap_ok(&tap, script_ok && rc == (scenario == 3 ? ETIMEDOUT : scenario == 4 ? EIO : 0) &&
            conn.cleanup_waiters == NULL && conn.refs_counter == 1 && req.wait == NULL &&
            timer_starts == 1 && timer_stops == 1 && lock_depth == 0 &&
            (scenario > 2 || (conn.state == USFS_CONN_ACTIVE &&
                             conn.outstanding_requests == 1 && conn.cleanup_outstanding == 0)),
            scenario == 1 ? "ordinary saturation leaves one usable cleanup reserve" :
            scenario == 2 ? "concurrent cleanup waits for its reserve without resetting the deadline" :
            scenario == 3 ? "cleanup admission timeout performs terminal recovery" :
                            "disconnect wakes cleanup admission and retains no waiter");
    }
    reset_state();
    memset(&conn, 0, sizeof(conn)); memset(&req, 0, sizeof(req));
    conn.state = USFS_CONN_CLOSED; conn.refs_counter = 1;
    conn.select_events = POLLIN | POLLHUP | POLLERR;
    selected_events = 0;
    wake_closed_connection(&conn);
    abort_connection(&conn);
    tap_ok(&tap, conn.state == USFS_CONN_CLOSED && wake_calls == 1 &&
           last_event == &conn.read_queue_event && selected_events == (POLLIN | POLLHUP) && lock_depth == 0,
           "orderly completion wakes blocked readers and select without later close becoming an error");
    {
        struct usfs_in_hdr header = {0};
        header.opcode = USFS_OP_GETATTR; header.nodeid = 2;
        req.request_buffer = (char *)&header; req.request_buffer_size = sizeof(header);
        tap_ok(&tap, usfs_call(&conn, &req) == EIO && request_frees == 1 &&
               conn.refs_counter == 1 && conn.outstanding_requests == 0 &&
               conn.pending_requests_head == NULL && conn.state == USFS_CONN_CLOSED && lock_depth == 0,
               "orderly-closed connections reject new RPCs without reserving slots or leaking ownership");
    }
    return tap_finish(&tap);
}
