// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT
#include "tap.h"
#include "../../../src/kernel/proxy/proxy.c"

static int pin_error, add_error, del_error, unpin_error;
static int locks, implementation_state, callback_error, forwarded;
static struct devsw installed;
int pincode(int (*entry)()) { (void)entry; return pin_error; }
int unpincode(int (*entry)()) { (void)entry; return unpin_error; }
void lock_alloc(void *p, int f, short c, uint n)
{ (void)p; (void)f; (void)c; (void)n; }
void lock_free(void *p) { (void)p; }
void simple_lock_init(Simple_lock *p) { (void)p; }
void simple_lock(Simple_lock *p) { (void)p; ++locks; }
void simple_unlock(Simple_lock *p) { (void)p; --locks; }
int devswadd(dev_t d, struct devsw *v) { (void)d; installed = *v; return add_error; }
int devswdel(dev_t d) { (void)d; return del_error; }
int nodev(void) { return ENODEV; }
int uiomove(caddr_t p, int32long64_t n, enum uio_rw rw, struct uio *u)
{ (void)p; (void)n; (void)rw; (void)u; return EFAULT; }

static int called(void) { ++forwarded; return locks ? EDEADLK : callback_error; }
static int test_open(dev_t d, ulong f, chan_t c, int x)
{ (void)d; (void)f; (void)c; (void)x; return called(); }
static int test_close(dev_t d, chan_t c)
{ (void)d; (void)c; return called(); }
static int test_io(dev_t d, struct uio *u, chan_t c, int x)
{ (void)d; (void)u; (void)c; (void)x; return called(); }
static int test_ioctl(dev_t d, int n, void *p, ulong f, chan_t c, int x)
{ (void)d; (void)n; (void)p; (void)f; (void)c; (void)x; return called(); }
static int test_mpx(dev_t d, chan_t *c, const char *n)
{ (void)d; (void)c; (void)n; return called(); }
static int test_select(dev_t d, int e, ushort *r, int c)
{ (void)d; (void)e; (void)r; (void)c; return called(); }
static int test_config(dev_t d, int n, struct uio *u)
{ (void)d; (void)n; (void)u; return called(); }
static int test_lifecycle(void) { return implementation_state; }

int main(void)
{
    struct tap_state tap;
    tap_plan(&tap, 31);
    struct usfs_proxy_callbacks table = {
        .abi=USFS_PROXY_ABI, .size=sizeof(table), .build="test",
        .open=test_open, .close=test_close, .read=test_io, .write=test_io,
        .ioctl=test_ioctl, .mpx=test_mpx, .select=test_select,
        .config=test_config, .lifecycle=test_lifecycle
    }, lease;
    tap_ok(&tap, usfs_proxy_attach(&table)==ENODEV, "uninitialized proxy rejects attachment");
    pin_error=ENOMEM;
    tap_ok(&tap, initialize(41)==ENOMEM && !initialized, "pin failure publishes nothing");
    pin_error=0; add_error=EEXIST;
    tap_ok(&tap, initialize(41)==EEXIST && !status.registered, "registration failure retains cleanup ownership");
    initialized=0; add_error=0;
    tap_ok(&tap, initialize(41)==0 && status.registered, "register resident trampolines");
    tap_ok(&tap, initialize(41)==0 && initialize(42)==EBUSY, "repeated initialization preserves identity");
    setup_busy=1;
    tap_ok(&tap, usfs_proxy_entry(41,CFG_INIT,NULL)==EBUSY &&
           usfs_proxy_attach(&table)==EBUSY &&
           usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==EBUSY &&
           status.registered, "initialization attachment and removal serialize in kernel");
    setup_busy=0;
    status.calls=1;
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==EBUSY &&
           status.registered, "maintenance refuses an active callback reference");
    status.calls=0; control_busy=1;
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==EBUSY &&
           status.registered, "maintenance refuses a competing control operation");
    control_busy=0;
    tap_ok(&tap, proxy_mpx(41,NULL,"")==ENODEV, "empty proxy rejects a late callback");
    table.abi++;
    tap_ok(&tap, usfs_proxy_attach(&table)==EINVAL, "incompatible ABI rejected");
    table.abi=USFS_PROXY_ABI; table.size--;
    tap_ok(&tap, usfs_proxy_attach(&table)==EINVAL, "incomplete interface rejected");
    table.size=sizeof(table); table.mpx=NULL;
    tap_ok(&tap, usfs_proxy_attach(&table)==EINVAL, "missing callback rejected");
    table.mpx=test_mpx;
    tap_ok(&tap, usfs_proxy_attach(&table)==0 && usfs_proxy_attach(&table)==EBUSY, "single attachment only");
    tap_ok(&tap, proxy_open(41,0,0,0)==EBUSY, "attachment does not publish incomplete initialization");
    tap_ok(&tap, forward_configuration(41,CFG_TERM,NULL)==EBUSY && implementation.config,
           "teardown cannot race incomplete attachment");
    tap_ok(&tap, usfs_proxy_activate()==0 && proxy_open(41,0,0,0)==0 && !status.calls, "forwarding runs outside lock and returns reference");
    callback_error=EIO;
    tap_ok(&tap, proxy_close(41,0)==EIO && !status.calls, "callback error still releases reference");
    callback_error=0;
    tap_ok(&tap, proxy_read(41,NULL,0,0)==0 && proxy_write(41,NULL,0,0)==0 && proxy_ioctl(41,0,NULL,0,0,0)==0 && proxy_select(41,0,NULL,0)==0, "all device trampolines forward");
    status.calls=UINT_MAX;
    tap_ok(&tap, enter(&lease)==EBUSY && status.calls==UINT_MAX, "reference counter cannot overflow");
    status.calls=0; enter(&lease);
    tap_ok(&tap, forward_configuration(41,CFG_TERM,NULL)==EBUSY && implementation.config, "admitted callback prevents teardown");
    leave(0); control_busy=1;
    tap_ok(&tap, forward_configuration(41,CFG_TERM,NULL)==EBUSY, "control operations exclude one another");
    control_busy=0; callback_error=EBUSY; implementation_state=USFS_KEXT_STATE_ACTIVE;
    tap_ok(&tap, forward_configuration(41,CFG_TERM,NULL)==EBUSY && status.state==USFS_PROXY_ACTIVE, "busy resources restore admission");
    callback_error=EIO; implementation_state=USFS_KEXT_STATE_CLEANUP_REQUIRED;
    tap_ok(&tap, forward_configuration(41,CFG_TERM,NULL)==EIO && status.state==USFS_PROXY_CLOSED && implementation.config, "partial teardown retains implementation");
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==EBUSY, "maintenance cannot remove attached implementation");
    callback_error=0; implementation_state=USFS_KEXT_STATE_DOWN;
    tap_ok(&tap, forward_configuration(41,CFG_TERM,NULL)==0 && !implementation.config && status.state==USFS_PROXY_EMPTY, "successful teardown detaches after return");
    tap_ok(&tap, proxy_mpx(41,NULL,"")==ENODEV && !status.calls, "delayed pre-admission callback is safe after detach");
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_QUERY,NULL)==EINVAL, "malformed query rejected");
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_TEST,NULL)==ENOTTY, "production excludes test control");
    del_error=EEXIST;
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==EEXIST && status.registered, "failed removal retains registration");
    del_error=0; unpin_error=EIO;
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==EIO && !status.registered, "failed unpin retains retry path");
    unpin_error=0;
    tap_ok(&tap, usfs_proxy_entry(41,USFS_PROXY_UNSAFE_TERM,NULL)==0 && locks==0 && forwarded>0, "maintenance retry completes with balanced locks");
    return tap_finish(&tap);
}
