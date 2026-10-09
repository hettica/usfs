/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"

int main(void)
{
    struct tap_state state;

    tap_plan(&state, 3);
    tap_ok(&state, 1, "true condition passes");
    TAP_EQ_U64(&state, 42, 42, "u64 equality passes");
    TAP_EQ_SIZE(&state, sizeof(uint64_t), 8, "expected uint64 width");
    return tap_finish(&state);
}
