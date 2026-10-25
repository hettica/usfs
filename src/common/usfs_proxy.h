// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT
#ifndef USFS_PROXY_H
#define USFS_PROXY_H

#include <sys/types.h>
#include <sys/inttypes.h>

#define USFS_PROXY_PATH "/usr/lib/drivers/usfs/usfs_proxy.kext"
#define USFS_PROXY_ABI 1u
#define USFS_PROXY_QUERY 0x55535001
#define USFS_PROXY_UNSAFE_TERM 0x55535002
#define USFS_PROXY_TEST 0x55535003
#define USFS_PROXY_EXPORT 0x55535004
#define USFS_PROXY_EMPTY 0u
#define USFS_PROXY_ACTIVE 1u
#define USFS_PROXY_CLOSED 2u
#define USFS_PROXY_ID_SIZE 64

struct usfs_proxy_status
{
    uint32_t abi;
    uint32_t size;
    uint32_t state;
    uint32_t calls;
    uint32_t lifecycle;
    uint32_t registered;
    uint64_t device;
    char proxy_build[USFS_PROXY_ID_SIZE];
    char implementation_build[USFS_PROXY_ID_SIZE];
    char variant[16];
};

/* Test control is not accepted by production proxies. Barriers are released
 * explicitly, never by an elapsed grace period. */
struct usfs_proxy_test_control
{
    uint32_t operation; /* 0 query, 1 arm, 2 release */
    uint32_t point;     /* 1 before admission, 2 admitted, 3 returned */
    uint32_t reached;
    uint32_t reserved;
};

#ifdef _KERNEL
struct uio;
struct usfs_proxy_callbacks
{
    uint32_t abi;
    uint32_t size;
    char build[USFS_PROXY_ID_SIZE];
    int (*open)(dev_t, ulong, chan_t, int);
    int (*close)(dev_t, chan_t);
    int (*read)(dev_t, struct uio *, chan_t, int);
    int (*write)(dev_t, struct uio *, chan_t, int);
    int (*ioctl)(dev_t, int, void *, ulong, chan_t, int);
    int (*mpx)(dev_t, chan_t *, const char *);
    int (*select)(dev_t, int, ushort *, int);
    int (*config)(dev_t, int, struct uio *);
    int (*lifecycle)(void);
};
int usfs_proxy_attach(const struct usfs_proxy_callbacks * callbacks);
int usfs_proxy_activate(void);
#endif
#endif
