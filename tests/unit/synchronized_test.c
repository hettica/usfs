/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"

typedef struct
{
    int held;
    int acquisitions;
    int releases;
    int reads;
} Complex_lock;

typedef struct
{
    int held;
    int acquisitions;
    int releases;
} Simple_lock;

static void lock_write(Complex_lock *lock)
{
    lock->held += 1;
    lock->acquisitions += 1;
}

static void lock_read(Complex_lock *lock)
{
    lock->held += 1;
    lock->acquisitions += 1;
    lock->reads += 1;
}

static void lock_done(Complex_lock *lock)
{
    lock->held -= 1;
    lock->releases += 1;
}

static void simple_lock(Simple_lock *lock)
{
    lock->held += 1;
    lock->acquisitions += 1;
}

static void simple_unlock(Simple_lock *lock)
{
    lock->held -= 1;
    lock->releases += 1;
}

#include "synchronized.h"

static int return_from_block(Complex_lock *lock)
{
    write_synchronized_with(*lock)
    {
        return lock->held;
    }

    return 0;
}

static int goto_from_block(Complex_lock *lock)
{
    int held_in_block = 0;

    write_synchronized_with(*lock)
    {
        held_in_block = lock->held;
        goto outside;
    }

    held_in_block = -1;

outside:
    return held_in_block;
}

int main(void)
{
    struct tap_state state;
    Complex_lock lock = {0};
    Complex_lock locks[2] = {{0}, {0}};
    Simple_lock simple = {0};
    int body_runs = 0;
    int lock_index = 0;

    tap_plan(&state, 28);

    write_synchronized_with(lock)
    {
        TAP_EQ_U64(&state, lock.held, 1, "normal block holds lock");
        body_runs += 1;
    }
    TAP_EQ_U64(&state, body_runs, 1, "normal block runs once");
    TAP_EQ_U64(&state, lock.held, 0, "normal exit releases lock");
    TAP_EQ_U64(&state, lock.releases, 1, "normal exit releases exactly once");

    write_synchronized_with(lock)
    {
        body_runs += 1;
        break;
    }
    TAP_EQ_U64(&state, body_runs, 2, "break does not repeat block");
    TAP_EQ_U64(&state, lock.held, 0, "break releases lock");
    TAP_EQ_U64(&state, lock.releases, 2, "break releases exactly once");

    TAP_EQ_U64(&state, return_from_block(&lock), 1,
               "return runs while lock is held");
    TAP_EQ_U64(&state, lock.held, 0, "return releases lock");
    TAP_EQ_U64(&state, lock.releases, 3, "return releases exactly once");

    TAP_EQ_U64(&state, goto_from_block(&lock), 1,
               "goto runs while lock is held");
    TAP_EQ_U64(&state, lock.held, 0, "goto releases lock");
    TAP_EQ_U64(&state, lock.releases, 4, "goto releases exactly once");

    read_synchronized_with(lock)
    {
        TAP_EQ_U64(&state, lock.held, 1, "read block holds lock");
    }
    TAP_EQ_U64(&state, lock.reads, 1, "read block uses read acquisition");
    TAP_EQ_U64(&state, lock.held, 0, "read exit releases lock");
    TAP_EQ_U64(&state, lock.releases, 5, "read exit releases exactly once");

    synchronized_with(simple)
    {
        TAP_EQ_U64(&state, simple.held, 1, "simple block holds lock");
    }
    TAP_EQ_U64(&state, simple.acquisitions, 1, "simple block acquires once");
    TAP_EQ_U64(&state, simple.held, 0, "simple exit releases lock");
    TAP_EQ_U64(&state, simple.releases, 1, "simple exit releases exactly once");

    synchronized_with(simple)
    {
        break;
    }
    TAP_EQ_U64(&state, simple.held, 0, "simple break releases lock");
    TAP_EQ_U64(&state, simple.releases, 2, "simple break releases exactly once");

    write_synchronized_with(locks[lock_index++])
    {
        TAP_EQ_U64(&state, locks[0].held, 1,
                   "selected lock is held in block");
    }
    TAP_EQ_U64(&state, lock_index, 1, "lock expression is evaluated once");
    TAP_EQ_U64(&state, locks[0].held, 0, "selected lock is released");

    write_synchronized_with(locks[0])
    {
        write_synchronized_with(locks[1])
        {
            TAP_EQ_U64(&state, locks[0].held + locks[1].held, 2,
                       "nested blocks use independent guards");
        }
    }
    TAP_EQ_U64(&state, locks[0].held + locks[1].held, 0,
               "nested exit releases both locks");

    return tap_finish(&state);
}
