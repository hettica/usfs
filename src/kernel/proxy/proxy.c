// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT
#define _MSGQSUPPORT
#include <sys/poll.h>
#include <sys/time.h>
#include <unistd.h>
#undef _MSGQSUPPORT
#include <sys/device.h>
#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/lock_alloc.h>
#include <sys/lock_def.h>
#include <sys/pin.h>
#include <sys/sleep.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vmuser.h>
#include <limits.h>
#include <string.h>
#include "usfs_proxy.h"
#include "usfs_config.h"

#ifndef USFS_BUILD_REVISION
#define USFS_BUILD_REVISION "unknown"
#endif
#ifndef USFS_PROXY_BUILD_ID
#define USFS_PROXY_BUILD_ID USFS_BUILD_REVISION
#endif
#ifndef USFS_PROXY_VARIANT
#define USFS_PROXY_VARIANT "release"
#endif

static Simple_lock dispatch_lock;
static struct usfs_proxy_callbacks implementation;
static struct usfs_proxy_status status;
static int initialized;
static int control_busy;
static int pinned;
/* The dispatch lock does not exist before CFG_INIT. Serialize its creation
 * with attachment and explicit maintenance removal using an atomic gate. */
static volatile int setup_busy;
int usfs_proxy_entry(dev_t device, int command, struct uio * io);

#include "instrumentation/proxy.h"

int usfs_proxy_attach(const struct usfs_proxy_callbacks * callbacks)
{
    if (!__sync_bool_compare_and_swap(&setup_busy, 0, 1))
        return EBUSY;
    if (!initialized || !status.registered || !status.pager_registered)
    {
        __sync_lock_release(&setup_busy);
        return ENODEV;
    }
    if (callbacks == NULL || callbacks->abi != USFS_PROXY_ABI ||
        callbacks->size != sizeof(*callbacks) ||
        memchr(callbacks->build, 0, sizeof(callbacks->build)) == NULL ||
        !callbacks->open || !callbacks->close || !callbacks->read ||
        !callbacks->write || !callbacks->ioctl || !callbacks->mpx ||
        !callbacks->select || !callbacks->config || !callbacks->lifecycle ||
        !callbacks->pager)
    {
        __sync_lock_release(&setup_busy);
        return EINVAL;
    }
    simple_lock(&dispatch_lock);
    int rc = 0;
    if (implementation.config || control_busy || status.calls)
        rc = EBUSY;
    else
    {
        implementation = *callbacks;
        memcpy(status.implementation_build, callbacks->build, sizeof(callbacks->build));
        status.state = USFS_PROXY_CLOSED;
        status.lifecycle = USFS_KEXT_STATE_DOWN;
    }
    simple_unlock(&dispatch_lock);
    __sync_lock_release(&setup_busy);
    return rc;
}

int usfs_proxy_activate(void)
{
    simple_lock(&dispatch_lock);
    const int rc = implementation.config ? 0 : ENODEV;
    if (!rc)
    {
        status.state = USFS_PROXY_ACTIVE;
        status.lifecycle = USFS_KEXT_STATE_ACTIVE;
    }
    simple_unlock(&dispatch_lock);
    return rc;
}

/* Called by the implementation after its resource check, before teardown.
 * No cleanup callback may enter implementation code after this succeeds. */
int usfs_proxy_seal(void)
{
    simple_lock(&dispatch_lock);
    int rc = 0;
    if (!control_busy || !implementation.config ||
        (status.state != USFS_PROXY_CLOSING && status.state != USFS_PROXY_CLOSED))
        rc = EBUSY;
    else if (status.calls)
        rc = EBUSY;
    else
        status.state = USFS_PROXY_CLOSED;
    simple_unlock(&dispatch_lock);
    return rc;
}

static int enter(struct usfs_proxy_callbacks * callbacks, int cleanup)
{
    checkpoint(1);
    simple_lock(&dispatch_lock);
    int rc = 0;
    if (!implementation.config)
        rc = ENODEV;
    else if ((status.state != USFS_PROXY_ACTIVE &&
              !(cleanup && status.state == USFS_PROXY_CLOSING)) ||
             status.calls == UINT_MAX)
        rc = EBUSY;
    else
    {
        ++status.calls;
        *callbacks = implementation;
    }
    simple_unlock(&dispatch_lock);
    if (!rc)
        checkpoint(2);
    return rc;
}

static int leave(int rc)
{
    checkpoint(3);
    simple_lock(&dispatch_lock);
    --status.calls;
    simple_unlock(&dispatch_lock);
    return rc;
}

/* The local copy is protected until leave(), including the callback's return
 * sequence and function descriptor/TOC accesses on POWER. */
#define FORWARD(member, arguments) \
    struct usfs_proxy_callbacks callbacks; \
    int rc = enter(&callbacks, 0); \
    if (rc) return rc; \
    rc = callbacks.member arguments; \
    return leave(rc)

static int proxy_open(dev_t d, ulong f, chan_t c, int x)
{ FORWARD(open, (d, f, c, x)); }
static int proxy_close(dev_t d, chan_t c)
{
    struct usfs_proxy_callbacks callbacks;
    int rc = enter(&callbacks, 1);
    if (rc) return rc;
    rc = callbacks.close(d, c);
    return leave(rc);
}
static int proxy_read(dev_t d, struct uio * u, chan_t c, int x)
{ FORWARD(read, (d, u, c, x)); }
static int proxy_write(dev_t d, struct uio * u, chan_t c, int x)
{ FORWARD(write, (d, u, c, x)); }
static int proxy_ioctl(dev_t d, int n, void * p, ulong f, chan_t c, int x)
{ FORWARD(ioctl, (d, n, p, f, c, x)); }
static int proxy_mpx(dev_t d, chan_t * c, const char * n)
{
    struct usfs_proxy_callbacks callbacks;
    int rc = enter(&callbacks, n == NULL);
    if (rc) return rc;
    rc = callbacks.mpx(d, c, n);
    return leave(rc);
}
static int proxy_select(dev_t d, int e, ushort * r, int c)
{ FORWARD(select, (d, e, r, c)); }

/* The VMM can enter a strategy descriptor after implementation teardown.
 * Complete every rejected buffer from resident code without touching a vnode
 * or any memory owned by the replaceable extension. */
static void reject_pager_buffers(struct buf * head)
{
    for (struct buf * buffer = head; buffer != NULL; )
    {
        struct buf * next = buffer->av_forw;
        buffer->b_resid = buffer->b_bcount;
        buffer->b_flags |= B_ERROR;
        buffer->b_error = EIO;
        iodone(buffer);
        buffer = next;
    }
}

void usfs_proxy_pager_strategy(struct buf * head, int flags, int path)
{
    struct usfs_proxy_callbacks callbacks;
    if (enter(&callbacks, 0) != 0)
    {
        reject_pager_buffers(head);
        return;
    }
    callbacks.pager(head, flags, path);
    (void)leave(0);
}

static int register_pager(void)
{
    struct thrpginfo info;
    memset(&info, 0, sizeof(info));
    info.mysize = sizeof(info);
    info.bufstsz = sizeof(bufthrio_t);
    info.numbufs = USFS_PROXY_PAGER_BUFFER_COUNT;
    info.numwresvd = USFS_PROXY_PAGER_RESERVED_WRITES;
    const int rc = vm_mounte(D_REMOTE | D_THRPGIO | D_ENHANCEDIO,
                             (dev_t)&usfs_proxy_pager_strategy, &info);
    if (rc == 0)
    {
        simple_lock(&dispatch_lock);
        status.pager_registered = 1;
        simple_unlock(&dispatch_lock);
    }
    return rc;
}

static int initialize(dev_t device)
{
    int rc = 0;
    if (initialized)
    {
        if (status.device != (uint64_t)device) return EBUSY;
        if (status.registered && status.pager_registered) return 0;
    }
    else
    {
        rc = pincode((int (*)())usfs_proxy_entry);
        if (rc) return rc;
        pinned = 1;
        lock_alloc(&dispatch_lock, LOCK_ALLOC_PIN, 0, -1);
        simple_lock_init(&dispatch_lock);
        memset(&status, 0, sizeof(status));
        status.abi = USFS_PROXY_ABI;
        status.size = sizeof(status);
        status.device = (uint64_t)device;
        strcpy(status.proxy_build, USFS_PROXY_BUILD_ID);
        strcpy(status.variant, USFS_PROXY_VARIANT);
        control_busy = 0;
        initialized = 1;
    }
    if (!status.registered)
    {
        struct devsw descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.d_open = proxy_open;
        descriptor.d_close = proxy_close;
        descriptor.d_read = proxy_read;
        descriptor.d_write = proxy_write;
        descriptor.d_ioctl = proxy_ioctl;
        descriptor.d_mpx = proxy_mpx;
        descriptor.d_select = proxy_select;
        descriptor.d_config = usfs_proxy_entry;
        descriptor.d_strategy = descriptor.d_print = descriptor.d_dump = descriptor.d_revoke = nodev;
        descriptor.d_opts = DEV_MPSAFE | DEV_64BIT;
        rc = devswadd(device, &descriptor);
        if (rc) return rc;
        simple_lock(&dispatch_lock);
        status.registered = 1;
        simple_unlock(&dispatch_lock);
    }
    if (!status.pager_registered)
        rc = register_pager();
    /* Both registrations retain ownership after a partial initialization and
     * can be retried without duplicate devsw or pager entries. */
    return rc;
}

static int forward_configuration(dev_t device, int command, struct uio * io)
{
    struct usfs_proxy_callbacks callbacks;
    simple_lock(&dispatch_lock);
    if (control_busy || status.calls)
    {
        simple_unlock(&dispatch_lock);
        return EBUSY;
    }
    if (!implementation.config)
    {
        simple_unlock(&dispatch_lock);
        return ENODEV;
    }
    if (status.state == USFS_PROXY_CLOSED &&
        status.lifecycle == USFS_KEXT_STATE_DOWN)
    {
        /* Attachment has not yet been published by the initializer. */
        simple_unlock(&dispatch_lock);
        return EBUSY;
    }
    control_busy = 1;
    status.state = command == CFG_TERM && status.state == USFS_PROXY_ACTIVE ?
        USFS_PROXY_CLOSING : USFS_PROXY_CLOSED;
    callbacks = implementation;
    simple_unlock(&dispatch_lock);
    if (command == CFG_TERM) checkpoint(4);
    const int rc = callbacks.config(device, command, io);
    /* A successful deferred coverage drain may unpin the implementation.
     * Its lifecycle callback is then no longer safe to invoke. */
    const int lifecycle = command == USFS_CFG_INSTRUMENTATION_DRAIN && rc == 0 ?
        (int)USFS_KEXT_STATE_DOWN : callbacks.lifecycle();
    simple_lock(&dispatch_lock);
    status.lifecycle = (uint32_t)lifecycle;
    if (lifecycle == USFS_KEXT_STATE_DOWN)
    {
        memset(&implementation, 0, sizeof(implementation));
        memset(status.implementation_build, 0, sizeof(status.implementation_build));
        status.state = USFS_PROXY_EMPTY;
    }
    else if (lifecycle == USFS_KEXT_STATE_ACTIVE)
        status.state = USFS_PROXY_ACTIVE;
    else
        status.state = USFS_PROXY_CLOSED;
    control_busy = 0;
    simple_unlock(&dispatch_lock);
    return rc;
}

int usfs_proxy_entry(dev_t device, int command, struct uio * io)
{
    if (command == CFG_INIT)
    {
        if (!__sync_bool_compare_and_swap(&setup_busy, 0, 1)) return EBUSY;
        const int rc = initialize(device);
        __sync_lock_release(&setup_busy);
        return rc;
    }
    if (!initialized) return ENODEV;
    if (command == USFS_PROXY_QUERY)
    {
        struct usfs_proxy_status snapshot;
        if (!io || io->uio_resid != sizeof(snapshot)) return EINVAL;
        simple_lock(&dispatch_lock);
        snapshot = status;
        simple_unlock(&dispatch_lock);
        return uiomove((caddr_t)&snapshot, sizeof(snapshot), UIO_READ, io);
    }
    int instrumentation_handled = 0;
    const int instrumentation_rc = proxy_instrument_command(command, io,
                                                             &instrumentation_handled);
    if (instrumentation_handled) return instrumentation_rc;
    if (command == USFS_PROXY_UNSAFE_TERM)
    {
        if (!__sync_bool_compare_and_swap(&setup_busy, 0, 1)) return EBUSY;
        simple_lock(&dispatch_lock);
        if (implementation.config || status.calls || control_busy)
        { simple_unlock(&dispatch_lock); __sync_lock_release(&setup_busy); return EBUSY; }
        if (proxy_test_busy())
        { simple_unlock(&dispatch_lock); __sync_lock_release(&setup_busy); return EBUSY; }
        control_busy = 1;
        simple_unlock(&dispatch_lock);
        int rc = status.pager_registered ?
            vm_umount(D_REMOTE, (dev_t)&usfs_proxy_pager_strategy) : 0;
        if (rc)
        {
            simple_lock(&dispatch_lock);
            control_busy = 0;
            simple_unlock(&dispatch_lock);
            __sync_lock_release(&setup_busy);
            return rc;
        }
        simple_lock(&dispatch_lock);
        status.pager_registered = 0;
        simple_unlock(&dispatch_lock);
        rc = status.registered ? devswdel((dev_t)status.device) : 0;
        if (rc)
        {
            simple_lock(&dispatch_lock);
            control_busy = 0;
            simple_unlock(&dispatch_lock);
            __sync_lock_release(&setup_busy);
            return rc;
        }
        simple_lock(&dispatch_lock);
        status.registered = 0;
        simple_unlock(&dispatch_lock);
        /* Explicitly unsafe: pre-entry AIX dispatch is not counted here. */
        if (pinned)
        {
            rc = unpincode((int (*)())usfs_proxy_entry);
            if (rc)
            {
                simple_lock(&dispatch_lock);
                control_busy = 0;
                simple_unlock(&dispatch_lock);
                __sync_lock_release(&setup_busy);
                return rc;
            }
        }
        lock_free(&dispatch_lock);
        initialized = 0;
        pinned = 0;
        __sync_lock_release(&setup_busy);
        return 0;
    }
    return forward_configuration(device, command, io);
}
