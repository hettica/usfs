// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#include "device/protocol/transport_validation.h"
#include "definitions.h"

int is_usfs_reply_header_valid (const struct usfs_reply_header * header)
{
    // Validate reply header
    if (header == NULL)
        return false;

    if (header->version != (uint16_t)USFS_PROTOCOL_VERSION)
        return false;

    if (header->len < sizeof (*header))
        return false;

    if (header->pad != 0)
        return false;

    if (header->error < 0)
        return false;

    if (header->error > USFS_AIX_ERRNO_MAX)
        return false;

    return true;
}

uint32_t get_usfs_reply_body_length (const struct usfs_reply_header * header)
{
    return header->len - (uint32_t)sizeof (*header);
}

int is_usfs_reply_body_valid (const struct usfs_reply_header * header, const uint32_t expected_body_size, const uint64_t actual_data_size)
{
    if ((uint64_t)expected_body_size != actual_data_size)
        return false;

    if (header->error != 0 && expected_body_size != 0)
        return false;

    return true;
}
