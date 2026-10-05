// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_KERNEL_GCOV_RUNTIME_H
#define USFS_KERNEL_GCOV_RUNTIME_H

/* Kernel coverage runtime interface. */

#include "usfs_gcov.h"

int usfs_gcov_runtime_initialize (const unsigned expected_units);
int usfs_gcov_runtime_prepare_export (void);
void usfs_gcov_runtime_shutdown (void);
unsigned usfs_gcov_unit_count (void);
int usfs_gcov_snapshot_size (unsigned * size_out);
int usfs_gcov_snapshot_create (void ** output_buffer, unsigned * size_out);
void usfs_gcov_snapshot_destroy (void * output_buffer);
int usfs_gcov_reset_counters (void);

void usfs_gcov_register_all (void);
unsigned usfs_gcov_expected_units (void);

#endif
