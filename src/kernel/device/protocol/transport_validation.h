// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_DEVICE_TRANSPORT_VALIDATION_H
#define USFS_DEVICE_TRANSPORT_VALIDATION_H

#include "usfs_proto.h"
#include <stdint.h>

int is_usfs_reply_header_valid (const struct usfs_reply_header * header);
uint32_t get_usfs_reply_body_length (const struct usfs_reply_header * header);
int is_usfs_reply_body_valid (const struct usfs_reply_header * header, uint32_t expected_body_size, uint64_t actual_data_size);

#endif
