// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_REQUEST_STATE_H
#define USFS_DEVICE_REQUEST_STATE_H

#include "definitions.h"
#include <sys/types.h>

/**
 * Specifies how to handle a request when its caller's waiting thread receives an interrupt.
 */
enum usfs_request_interrupt_disposition
{
    USFS_REQUEST_INTERRUPT_RETRY = 0,   // Continue waiting; the request cannot be interrupted now.
    USFS_REQUEST_INTERRUPT_RELEASE = 1, // Remove and release the request immediately.
    USFS_REQUEST_INTERRUPT_COMPLETE = 2 // Complete delivery within the original deadline.
};

int remove_request_from_pending_queue (struct usfs_connection * connection, struct usfs_request * request);
void release_request_slot (struct usfs_connection * connection, struct usfs_request * request);
enum usfs_request_interrupt_disposition select_interrupt_strategy (int current_request_state);
int is_channel_valid (chan_t channel);

#endif
