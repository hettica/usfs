// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_REQUEST_WAIT_H
#define USFS_DEVICE_REQUEST_WAIT_H

/*
 * All storage reached by the timer interrupt must remain pinned until tstop
 * confirms that the handler and the timer service have finished with it.
 */
struct wait_control_block
{
    volatile int timed_out;
    int noninterruptible;
    tid_t event;
};

int allocate_wait_control_block (struct wait_control_block ** wait_out);
void free_wait_control_block (struct wait_control_block * wait);
void configure_timer (struct trb * timer, struct wait_control_block * wait, uint32_t timeout_ms);
int wait_for_request_event (struct usfs_connection * connection, struct wait_control_block * wait);
void wake_request_up (const struct usfs_request * request);

#endif
