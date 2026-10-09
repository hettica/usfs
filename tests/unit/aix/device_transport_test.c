/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

int main(void)
{
    struct tap_state tap;
    struct usfs_reply_header header;
    uint32_t body_length = 0;

    tap_plan(&tap, 10);
    memset(&header, 0, sizeof(header));
    header.version = USFS_PROTOCOL_VERSION;
    header.len = sizeof(header);
    tap_ok(&tap, !is_usfs_reply_header_valid(NULL),
           "transport rejects null reply header");
    tap_ok(&tap, get_usfs_reply_body_length(&header) == 0,
           "transport calculates empty reply body length");
    header.version += 1;
    tap_ok(&tap, !is_usfs_reply_header_valid(&header),
           "transport rejects protocol version mismatch");
    header.version = USFS_PROTOCOL_VERSION;
    header.len = sizeof(header) - 1u;
    tap_ok(&tap, !is_usfs_reply_header_valid(&header),
           "transport rejects header length underflow");
    header.len = sizeof(header);
    header.pad = 1;
    tap_ok(&tap, !is_usfs_reply_header_valid(&header),
           "transport rejects nonzero padding");
    header.pad = 0;
    header.error = -1;
    tap_ok(&tap, !is_usfs_reply_header_valid(&header),
           "transport rejects negative daemon errno");
    header.error = USFS_AIX_ERRNO_MAX + 1;
    tap_ok(&tap, !is_usfs_reply_header_valid(&header),
           "transport rejects errno outside the AIX ABI");
    header.error = 0;
    header.len = sizeof(header) + 4u;
    body_length = get_usfs_reply_body_length(&header);
    tap_ok(&tap, !is_usfs_reply_body_valid(&header, body_length, 3),
           "transport rejects body length differing from residual");
    header.error = EIO;
    tap_ok(&tap, !is_usfs_reply_body_valid(&header, body_length, 4),
           "transport rejects an error reply with a body");
    header.error = 0;
    tap_ok(&tap, is_usfs_reply_header_valid(&header) &&
                  is_usfs_reply_body_valid(&header, body_length, 4) &&
                  body_length == 4,
           "transport accepts a matching success reply body");
    return tap_finish(&tap);
}
