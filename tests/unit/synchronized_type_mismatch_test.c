/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

typedef struct
{
    int unused;
} Complex_lock;

typedef struct
{
    int unused;
} Simple_lock;

static void lock_read(Complex_lock *lock)
{
    (void)lock;
}

static void lock_write(Complex_lock *lock)
{
    (void)lock;
}

static void lock_done(Complex_lock *lock)
{
    (void)lock;
}

static void simple_lock(Simple_lock *lock)
{
    (void)lock;
}

static void simple_unlock(Simple_lock *lock)
{
    (void)lock;
}

#include "synchronized.h"

int main(void)
{
    Simple_lock lock = {0};

    write_synchronized_with(lock)
    {
    }

    return 0;
}
