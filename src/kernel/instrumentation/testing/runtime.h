// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_TEST_RUNTIME_H
#define USFS_TEST_RUNTIME_H

/* Test-only kernel instrumentation interface. */

#include "usfs_test.h"

struct usfs_mount_data;

int usfs_test_initialize (const unsigned char * payload, const int enabled);
void usfs_test_shutdown (void);
int usfs_test_fault (const int fault);
int usfs_test_schedule_point (const unsigned point);
int usfs_test_control_ioctl (const int command, void * user_buffer, const chan_t collector_channel);
void usfs_test_node_created (struct usfs_mount_data * mount_data, const uint64_t node_id);
void usfs_test_node_reclaimed (const uint64_t node_id, const int is_linked);
void usfs_test_node_reused (const int source);
void usfs_test_parent_rebuilt (void);
void usfs_test_vnode_hold (const uint64_t reference_count);
void usfs_test_vnode_release (const uint64_t reference_count);
int usfs_test_kernel_selftest (const uint32_t case_id, struct usfs_test_selftest * result);
int usfs_test_fid_probe (void * user_buffer);

#endif
