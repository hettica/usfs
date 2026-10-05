// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_SYNCHRONIZED_H
#define USFS_SYNCHRONIZED_H

/**
 * GCC runs a cleanup function whenever its guard's scope is left, including
 * by break, return, or goto. 
 *
 * Usage:
 *
 *     read_synchronized_with(global_lock)
 *     {
 * 
 *     }
 *
 *     write_synchronized_with(global_lock)
 *     {
 * 
 *     }
 *
 *     synchronized_with(conn->lock)
 *     {
 * 
 *     }
 *
 * Passing a lock of the wrong type is a compile-time error.  
 * synchronized_with is only for the simple_lock / simple_unlock pair; 
 * interrupt-priority locking needs its explicit disable_lock / unlock_enable pair instead.
 */

struct usfs_complex_lock_guard
{
    Complex_lock * lock;
    int iterate;
};

struct usfs_simple_lock_guard
{
    Simple_lock * lock;
    int iterate;
};

/*
 * WARNING: keep 'inline' keyword, it's necessary to make this auto-cleanup magic work.
 */

static inline struct usfs_complex_lock_guard usfs_read_synchronized_acquire (Complex_lock * lock)
{
    struct usfs_complex_lock_guard guard;

    lock_read (lock);
    guard.lock = lock;
    guard.iterate = 1;
    return guard;
}

static inline struct usfs_complex_lock_guard usfs_write_synchronized_acquire (Complex_lock * lock)
{
    struct usfs_complex_lock_guard guard;

    lock_write (lock);
    guard.lock = lock;
    guard.iterate = 1;
    return guard;
}

static inline void usfs_complex_synchronized_release (const struct usfs_complex_lock_guard * guard)
{
    lock_done (guard->lock);
}

static inline struct usfs_simple_lock_guard usfs_simple_synchronized_acquire (Simple_lock * lock)
{
    struct usfs_simple_lock_guard guard;

    simple_lock (lock);
    guard.lock = lock;
    guard.iterate = 1;
    return guard;
}

static inline void usfs_simple_synchronized_release (const struct usfs_simple_lock_guard * guard)
{
    simple_unlock (guard->lock);
}

#define USFS_SYNCHRONIZED_CONCAT_INNER(a, b) a##b
#define USFS_SYNCHRONIZED_CONCAT(a, b)       USFS_SYNCHRONIZED_CONCAT_INNER (a, b)


#define USFS_READ_SYNCHRONIZED_ACQUIRE(lock) _Generic(&(lock), Complex_lock * \
                                                      : usfs_read_synchronized_acquire) (&(lock))
#define USFS_WRITE_SYNCHRONIZED_ACQUIRE(lock) _Generic(&(lock), Complex_lock * \
                                                       : usfs_write_synchronized_acquire) (&(lock))
#define USFS_SIMPLE_SYNCHRONIZED_ACQUIRE(lock) _Generic(&(lock), Simple_lock * \
                                                        : usfs_simple_synchronized_acquire) (&(lock))


#define USFS_READ_SYNCHRONIZED_WITH(lock, id)                                                                               \
    for (struct usfs_complex_lock_guard                                                                                     \
             USFS_SYNCHRONIZED_CONCAT (_usfs_read_synchronized_guard_, id)                                                  \
                 __attribute__ ((__cleanup__ (usfs_complex_synchronized_release))) = USFS_READ_SYNCHRONIZED_ACQUIRE (lock); \
         USFS_SYNCHRONIZED_CONCAT (_usfs_read_synchronized_guard_, id).iterate;                                             \
         USFS_SYNCHRONIZED_CONCAT (_usfs_read_synchronized_guard_, id).iterate = 0)


#define USFS_WRITE_SYNCHRONIZED_WITH(lock, id)                                                                               \
    for (struct usfs_complex_lock_guard                                                                                      \
             USFS_SYNCHRONIZED_CONCAT (_usfs_write_synchronized_guard_, id)                                                  \
                 __attribute__ ((__cleanup__ (usfs_complex_synchronized_release))) = USFS_WRITE_SYNCHRONIZED_ACQUIRE (lock); \
         USFS_SYNCHRONIZED_CONCAT (_usfs_write_synchronized_guard_, id).iterate;                                             \
         USFS_SYNCHRONIZED_CONCAT (_usfs_write_synchronized_guard_, id).iterate = 0)


#define USFS_SIMPLE_SYNCHRONIZED_WITH(lock, id)                                                                              \
    for (struct usfs_simple_lock_guard                                                                                       \
             USFS_SYNCHRONIZED_CONCAT (_usfs_simple_synchronized_guard_, id)                                                 \
                 __attribute__ ((__cleanup__ (usfs_simple_synchronized_release))) = USFS_SIMPLE_SYNCHRONIZED_ACQUIRE (lock); \
         USFS_SYNCHRONIZED_CONCAT (_usfs_simple_synchronized_guard_, id).iterate;                                            \
         USFS_SYNCHRONIZED_CONCAT (_usfs_simple_synchronized_guard_, id).iterate = 0)


#define read_synchronized_with(lock)  USFS_READ_SYNCHRONIZED_WITH (lock, __COUNTER__)
#define write_synchronized_with(lock) USFS_WRITE_SYNCHRONIZED_WITH (lock, __COUNTER__)
#define synchronized_with(lock)       USFS_SIMPLE_SYNCHRONIZED_WITH (lock, __COUNTER__)

#endif
