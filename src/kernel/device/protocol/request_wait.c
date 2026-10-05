// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "definitions.h"
#include "device/protocol/request_wait.h"

static void timeout_handler (const struct trb * timer)
{
    struct wait_control_block * wait_control = (struct wait_control_block *)timer->func_data;

    wait_control->timed_out = 1;
    e_wakeup (&wait_control->event);
}

int allocate_wait_control_block (struct wait_control_block ** wait_control_out) // todo: return ptr instead of assigning it to output value
{
    static const int memory_alignment_in_bytes = 4;
    if (wait_control_out == NULL)
        return EINVAL;

    *wait_control_out = NULL;

    struct wait_control_block * wait_control = usfs_kmalloc (
        USFS_ALLOC_REQUEST_WAIT,
        sizeof (*wait_control),
        memory_alignment_in_bytes,
        pinned_heap
    );

    if (wait_control == NULL)
        return ENOMEM;

    memset (wait_control, 0, sizeof (*wait_control));
    wait_control->event = EVENT_NULL;
    *wait_control_out = wait_control;

    return 0;
}

void free_wait_control_block (struct wait_control_block * wait_control)
{
    if (wait_control != NULL)
        xmfree (wait_control, pinned_heap);
}

void configure_timer (struct trb * timer, struct wait_control_block * wait_control, const uint32_t timeout_ms)
{
    timer->flags = T_INCINTERVAL | T_LOWRES | T_MOVE_OK | T_MPSAFE;
    timer->timeout.it_value.tv_sec = (time_t)(timeout_ms / 1000u);
    timer->timeout.it_value.tv_nsec = (long)(timeout_ms % 1000u * 1000000u);
    timer->timeout.it_interval.tv_sec = 0;
    timer->timeout.it_interval.tv_nsec = 0;
    timer->func = timeout_handler;
    timer->func_data = (ulong)wait_control;
    timer->ipri = INTTIMER;
    timer->id = (ulong)-1;
}

int wait_for_request_event (struct usfs_connection * connection, struct wait_control_block * wait_control)
{
    /* Register before the final predicate check.  If the timer fires before
     * registration its flag is observed below; if it fires afterwards its
     * wakeup is retained for e_block_thread. */
    e_assert_wait (&wait_control->event, !wait_control->noninterruptible);

    if (wait_control->timed_out)
    {
        e_clear_wait (thread_self (), THREAD_AWAKENED);
        return THREAD_AWAKENED;
    }

    simple_unlock (&connection->lock);
    const int rc = e_block_thread ();
    simple_lock (&connection->lock);

    return rc;
}

void wake_request_up (const struct usfs_request * request)
{
    if (request->wait != NULL)
        e_wakeup (&request->wait->event);
}
