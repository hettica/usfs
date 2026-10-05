// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_KERNEL_GCOV_CONTROL_H
#define USFS_KERNEL_GCOV_CONTROL_H

int usfs_gcov_control_ioctl (const int command, void * user_buffer, const chan_t collector_channel);
int usfs_gcov_config_export (struct uio * user_io_request, const int reset_after, const uint32_t expected_generation);

#endif
