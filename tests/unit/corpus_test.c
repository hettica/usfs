/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "usfs_validate.h"

#include <string.h>

int main(void)
{
    struct tap_state state;
    struct usfs_reply_header header;
    unsigned opcode;

    tap_plan(&state, 25);
    memset(&header, 0, sizeof(header));
    header.len = sizeof(header);
    header.version = USFS_PROTOCOL_VERSION;

    for (opcode = USFS_OP_LOOKUP; opcode <= USFS_OP_VGET; opcode++) {
        header.opcode = (uint16_t)opcode;
        tap_ok(&state, usfs_out_header_valid(&header, sizeof(header)),
               "known opcode corpus entry");
    }
    header.opcode = 0xffffu;
    tap_ok(&state, !usfs_out_header_valid(&header, sizeof(header)),
           "unknown opcode corpus entry");
    return tap_finish(&state);
}
