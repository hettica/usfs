/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Diagnostic compatibility hooks required by instrumented kernel code.
 */

int __fd_getdtablesize ()
{
    enum
    {
        DIAGNOSTIC_DESCRIPTOR_CAPACITY = 2000
    };

    return DIAGNOSTIC_DESCRIPTOR_CAPACITY;
}

int __fd_select ()
{
    return -1;
}
