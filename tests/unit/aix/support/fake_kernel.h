/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#ifndef USFS_TEST_FAKE_KERNEL_H
#define USFS_TEST_FAKE_KERNEL_H

#include "definitions.h"

#define USFS_FAKE_EVENT_CAPACITY 64u

enum usfs_fake_event
{
    USFS_FAKE_EVENT_PRIVCHECK = 1,
    USFS_FAKE_EVENT_GROUPMEMBER = 2,
    USFS_FAKE_EVENT_GETATTR = 3,
    USFS_FAKE_EVENT_SETATTR = 4
};

struct usfs_fake_kernel_state
{
    int bypass_dac;
    int bypass_read;
    int bypass_write;
    int bypass_exec;
    int set_obj_dac;
    int group_member;
    gid_t member_gid;
    int getattr_rc;
    struct vattr attr;
    int setattr_rc;
    struct usfs_setattr_in setattr_body;
    unsigned privcheck_calls;
    unsigned groupmember_calls;
    unsigned getattr_calls;
    unsigned setattr_calls;
    unsigned allocation_balance;
    enum usfs_allocation_site allocation_fail_site;
    unsigned allocation_calls;
    unsigned allocation_errors;
    void *allocations[32];
    unsigned lock_depth;
    unsigned event_count;
    enum usfs_fake_event events[USFS_FAKE_EVENT_CAPACITY];
};

extern struct usfs_fake_kernel_state usfs_fake_kernel;

void usfs_fake_kernel_reset(void);
void usfs_fake_kernel_event(enum usfs_fake_event event);
int usfs_fake_kernel_clean(void);

#endif
