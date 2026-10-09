/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include <string.h>

Complex_lock global_lock;
static struct usfs_node node;
static struct gnode gn;
static struct vnode vn;
static struct usfs_mount_data mount_data;
static struct usfs_connection connection;
static struct usfs_open_state handle;
static int locks, pageout_locks, bad_lock, submitted, waited, invalidated;
static int submit_error, wait_error, move_error, move_limit, barrier, aborted;
static int released, disposed, deleted;
static int connection_locks, moves, quarantine_move, truncate_timeout;
static uint64_t backend_size, invalid_first, invalid_pages;
static char cache_data[USFS_MAX_DATA * 2];
static char backend_data[USFS_MAX_DATA * 2];
static int flushed, flush_error, flush_wait_error, intervening_write;
static uint64_t flush_first, flush_pages;
static void write_during_eviction(void);
int usfs_checkpoint(enum usfs_checkpoint checkpoint) {
    if (checkpoint == USFS_INSTRUMENT_CACHE_BEFORE_EVICT && intervening_write)
        write_during_eviction();
    return 0;
}

void simple_lock(simple_lock_t lock) { (void)lock; ++connection_locks; }
void simple_unlock(simple_lock_t lock) { (void)lock; --connection_locks; }

void lock_read(complex_lock_t lock) { if (lock == &global_lock) ++locks; }
void lock_write(complex_lock_t lock) {
    if (lock == &global_lock) ++locks;
    if (lock == &node.pageout_lock) ++pageout_locks;
}
void lock_done(complex_lock_t lock) {
    if (lock == &global_lock) --locks;
    if (lock == &node.pageout_lock) --pageout_locks;
}
static void check_vm_lock(void) { if (locks || pageout_locks || connection_locks) ++bad_lock; }
static struct usfs_node *_node_of(struct vnode *vp) { (void)vp; return &node; }
static struct usfs_mount_data *_mount_of(struct vnode *vp) { (void)vp; return &mount_data; }
void abort_connection(struct usfs_connection *conn) { (void)conn; ++aborted; }
static int usfs_retain_operation_locked(struct usfs_node *n, struct usfs_open_state *state,
                                       struct usfs_open_state **out) {
    (void)n; ++state->refs; ++state->operation_refs; ++vn.v_count; *out = state; return 0;
}
static struct usfs_open_state **usfs_open_state_link_locked(struct usfs_node *n,
                                                          struct usfs_open_state *state) {
    (void)state; return &n->opens;
}
static void usfs_open_state_discard(struct usfs_open_state *state) { (void)state; ++disposed; }
static void usfs_vnode_hold_locked(struct vnode *vp, int reuse) { (void)reuse; ++vp->v_count; }
#undef VNOP_RELE
#define VNOP_RELE(vp) (--(vp)->v_count)
static void _send_release(struct usfs_mount_data *m, uint64_t id, uint64_t fh,
                          int32long64_t flags, uint32_t dir, struct ucred *crp) {
    (void)m; (void)id; (void)fh; (void)flags; (void)dir; (void)crp;
    if (locks) ++bad_lock;
    ++released;
}
static int _setattr_unlocked(struct vnode *vp, const struct usfs_setattr_in *body,
                            uint64_t fh, struct ucred *crp) {
    (void)vp; (void)fh; (void)crp;
    if (pageout_locks != 1 || locks) ++bad_lock;
    backend_size = body->size;
    if (truncate_timeout) { connection.state = USFS_CONN_UNHEALTHY; return ETIMEDOUT; }
    return 0;
}
static int _sync_node(struct vnode *vp, uint64_t fh, int32long64_t flags,
                      int range, offset_t offset, offset_t length, struct ucred *crp) {
    (void)vp; (void)fh; (void)flags; (void)range; (void)offset; (void)length; (void)crp;
    if (!submitted || !waited || locks || pageout_locks) ++bad_lock;
    ++barrier; return 0;
}
int vm_writep(vmid_t sid, vpn_t first, vpn_t count) {
    (void)sid; check_vm_lock(); ++submitted;
    if (!submit_error) memcpy(backend_data + first * PAGESIZE,
                             cache_data + first * PAGESIZE, count * PAGESIZE);
    return submit_error;
}
int vm_flushp(vmid_t sid, vpn_t first, vpn_t count) {
    (void)sid; check_vm_lock(); ++flushed;
    flush_first = first; flush_pages = count;
    if (!flush_error) memcpy(backend_data + first * PAGESIZE,
                            cache_data + first * PAGESIZE, count * PAGESIZE);
    return flush_error;
}
int vms_iowaitf(vmid_t sid, int flags) {
    (void)sid; if (flags != V_WAITALL) ++bad_lock;
    check_vm_lock(); ++waited; return flushed ? flush_wait_error : wait_error;
}
int vm_invalidatep(vmid_t sid, vpn_t first, ulong count) {
    (void)sid; check_vm_lock(); ++invalidated;
    invalid_first = first; invalid_pages = count;
    if ((uint64_t)first * PAGESIZE < sizeof(cache_data))
        memset(cache_data + first * PAGESIZE, 0,
               count * PAGESIZE <= sizeof(cache_data) - first * PAGESIZE ?
               count * PAGESIZE : sizeof(cache_data) - first * PAGESIZE);
    return 0;
}
int vms_delete(vmid_t sid) { (void)sid; check_vm_lock(); ++deleted; return 0; }
int vm_uiomove(vmid_t sid, vmsize_t count, enum uio_rw direction, struct uio *uio) {
    (void)sid; check_vm_lock();
    ++moves;
    if (quarantine_move) connection.state = USFS_CONN_UNHEALTHY;
    if (move_limit >= 0 && count > (vmsize_t)move_limit) count = (vmsize_t)move_limit;
    if ((uint64_t)uio->uio_offset + count > sizeof(cache_data)) return EFAULT;
    if (direction == UIO_WRITE)
        memcpy(cache_data + uio->uio_offset, uio->uio_iov->iov_base, count);
    else memcpy(uio->uio_iov->iov_base, cache_data + uio->uio_offset, count);
    uio->uio_offset += count; uio->uio_resid -= count;
    return move_error;
}
#include "fs/cache.h"

static void reset(void) {
    memset(&node, 0, sizeof(node)); memset(&gn, 0, sizeof(gn));
    memset(&vn, 0, sizeof(vn)); memset(&mount_data, 0, sizeof(mount_data));
    memset(&connection, 0, sizeof(connection)); memset(&handle, 0, sizeof(handle));
    memset(cache_data, 'x', sizeof(cache_data));
    memset(backend_data, 0, sizeof(backend_data));
    flushed = flush_error = flush_wait_error = intervening_write = 0;
    flush_first = flush_pages = 0;
    node.gn = &gn; node.vn = &vn; node.nodeid = 1; node.opens = &handle;
    node.cache_size = backend_size = PAGESIZE * 3;
    gn.gn_seg = 7; vn.v_gnode = &gn; vn.v_count = 1;
    mount_data.nodes = &node; mount_data.conn = &connection; connection.mounted_data = &mount_data;
    connection.state = USFS_CONN_ACTIVE;
    handle.active = 1; handle.refs = 1; handle.flags = FREAD | FWRITE;
    locks = pageout_locks = bad_lock = submitted = waited = invalidated = 0;
    submit_error = wait_error = move_error = barrier = aborted = 0;
    released = disposed = deleted = 0; move_limit = -1;
    connection_locks = moves = quarantine_move = truncate_timeout = 0;
}
static struct uio make_uio(struct iovec *vector, void *data, size_t count, offset_t offset) {
    struct uio uio; memset(&uio, 0, sizeof(uio));
    vector->iov_base = data; vector->iov_len = count;
    uio.uio_iov = vector; uio.uio_iovcnt = 1; uio.uio_resid = count;
    uio.uio_offset = offset; uio.uio_segflg = UIO_SYSSPACE;
    return uio;
}
static void write_during_eviction(void) {
    if (intervening_write == 1) {
        cache_data[PAGESIZE + 7] = 'm';
    } else {
        struct iovec vector;
        char value = 'w';
        struct uio uio = make_uio(&vector, &value, 1, PAGESIZE + 7);
        if (transfer_cached_data(&vn, 1, UIO_WRITE, 0, &uio, NULL) != 0) ++bad_lock;
    }
}
int main(void) {
    struct tap_state tap; struct usfs_setattr_in change;
    struct usfs_open_state *borrowed = NULL;
    struct iovec vector; struct uio uio; char data[16];
    tap_plan(&tap, 35);
    reset();
    tap_ok(&tap, ensure_cache_handle(&node, 0, &handle) == 0 &&
           ensure_cache_handle(&node, 1, &handle) == 0 && handle.refs == 3 &&
           handle.cache_refs == 2 && vn.v_count == 1,
           "cache retains compatible backend handles without a vnode reference cycle");
    handle.active = 0; --handle.refs;
    tap_ok(&tap, borrow_cache_handle(&node, 1, &borrowed) == 0 && borrowed == &handle &&
           handle.operation_refs == 1 && vn.v_count == 2,
           "pageout can borrow a retained handle after the final descriptor closes");
    --handle.refs; --handle.operation_refs; --vn.v_count;
    dispose_cache_resources(&node, &mount_data);
    tap_ok(&tap, released == 1 && disposed == 1 && deleted == 1 && node.opens == NULL &&
           handle.refs == 0 && bad_lock == 0, "reclamation deletes the segment and releases a shared cache handle exactly once");
    reset(); wait_error = EIO;
    tap_ok(&tap, flush_cached_pages(&vn, 0, 0, 0, 0) == EIO && submitted == 1 && waited == 1,
           "cache sync submits dirty pages and collects writeback completion errors");
    wait_error = 0;
    tap_ok(&tap, flush_cached_pages(&vn, 0, 0, 0, 1) == EIO && flushed == 0 && invalidated == 0,
           "a later successful pageout cannot hide a retained error or discard failed data");
    reset();
    tap_ok(&tap, flush_cached_pages(&vn, 1, 1, PAGESIZE * 2, 1) == 0 &&
           flush_first == 1 && flush_pages == 1 && waited == 2 && invalidated == 0 && bad_lock == 0,
           "range discard covers only complete pages after completion outside pager locks");
    const uint64_t zero_length_offsets[] = { 0, PAGESIZE + 1, PAGESIZE * 2 + 17, PAGESIZE * 4 };
    for (unsigned i = 0; i < sizeof(zero_length_offsets) / sizeof(zero_length_offsets[0]); ++i) {
        for (int discard = 0; discard <= 1; ++discard) {
            reset(); node.cache_size = backend_size = PAGESIZE * 2 + 17;
            int rc = flush_cached_pages(&vn, 1, zero_length_offsets[i], 0, discard);
            tap_ok(&tap, rc == 0 && submitted == 1 && waited == 1 + discard &&
                   memcmp(backend_data, cache_data, (size_t)node.cache_size) == 0 &&
                   flushed == discard && (!discard || (flush_first == 0 && flush_pages == 2)) &&
                   invalidated == 0 && bad_lock == 0,
                   "zero-length range writes the whole file at any offset and evicts only complete pages");
        }
    }
    reset(); submit_error = ENOSPC;
    int zero_length_rc = flush_cached_pages(&vn, 1, PAGESIZE * 4, 0, 1);
    submit_error = 0;
    tap_ok(&tap, zero_length_rc == ENOSPC && waited == 1 && flushed == 0 &&
           flush_cached_pages(&vn, 1, PAGESIZE, 0, 1) == ENOSPC &&
           submitted == 2 && waited == 2 && flushed == 0 && bad_lock == 0,
           "zero-length range drains failed writeback and retains the error without evicting pages");
    for (int kind = 1; kind <= 2; ++kind) {
        reset(); intervening_write = kind;
        int rc = flush_cached_pages(&vn, 1, 1, PAGESIZE * 2, 1);
        char expected = kind == 1 ? 'm' : 'w';
        tap_ok(&tap, rc == 0 && cache_data[PAGESIZE + 7] == expected &&
               backend_data[PAGESIZE + 7] == expected && invalidated == 0 &&
               flush_cached_pages(&vn, 0, 0, 0, 0) == 0 &&
               backend_data[PAGESIZE + 7] == expected && bad_lock == 0,
               "eviction preserves an intervening mapped or ordinary cached write through later sync");
    }
    reset(); submit_error = ENOSPC;
    tap_ok(&tap, flush_cached_pages(&vn, 0, 0, 0, 1) == ENOSPC && waited == 1 && flushed == 0,
           "initial submission failure still drains I/O and prevents eviction");
    reset(); flush_error = ENOSPC; flush_wait_error = EIO;
    tap_ok(&tap, flush_cached_pages(&vn, 0, 0, 0, 1) == ENOSPC && waited == 2 &&
           node.writeback_error == ENOSPC, "eviction submission failure drains I/O and retains the first error");
    reset(); flush_wait_error = EIO;
    tap_ok(&tap, flush_cached_pages(&vn, 0, 0, 0, 1) == EIO && waited == 2 &&
           node.writeback_error == EIO, "eviction completion failure becomes sticky");
    reset(); handle.flags = FREAD;
    tap_ok(&tap, flush_cached_pages(&vn, 0, 0, 0, 1) == 0 && flushed == 1 && waited == 2 &&
           node.cache_writer == NULL, "clean read-only cache eviction requires no writable handle");
    reset();
    tap_ok(&tap, flush_cached_pages(&vn, 1, 1, PAGESIZE - 2, 1) == 0 && flushed == 0 && waited == 1,
           "a range without complete interior pages retains both boundaries");
    reset(); (void)ensure_cache_handle(&node, 1, &handle);
    memset(&change, 0, sizeof(change)); change.valid = USFS_SET_SIZE; change.size = PAGESIZE + 3;
    tap_ok(&tap, resize_cached_file(&vn, &change, 1, NULL) == 0 &&
           backend_size == change.size && node.cache_size == change.size &&
           invalid_first == 2 && invalid_pages == 1 && cache_data[PAGESIZE + 2] == 'x' &&
           cache_data[PAGESIZE + 3] == 0 && bad_lock == 0,
           "shrink serializes backend EOF with pageout, discards removed pages, and zeroes the tail");
    change.size = PAGESIZE * 3;
    tap_ok(&tap, resize_cached_file(&vn, &change, 1, NULL) == 0 &&
           cache_data[PAGESIZE * 2] == 0 && cache_data[PAGESIZE + 3] == 0,
           "regrowth does not expose old bytes from removed resident pages");
    reset(); handle.flags = FREAD; change.size = PAGESIZE + 3;
    tap_ok(&tap, resize_cached_file(&vn, &change, 0, NULL) == 0 &&
           invalid_first == 1 && node.cache_writer == NULL && backend_size == change.size,
           "path truncation of a read-only mapped segment needs no writable backing descriptor");
    reset(); memset(data, 0, sizeof(data)); cache_data[4] = 'd';
    uio = make_uio(&vector, data, sizeof(data), 0);
    tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_READ, 0, &uio, NULL) == 0 && data[4] == 'd',
           "ordinary reads consume the same cached bytes as mapped writes");
    reset(); memset(data, 'w', sizeof(data)); uio = make_uio(&vector, data, sizeof(data), 0);
    tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_WRITE, FSYNC, &uio, NULL) == 0 &&
           cache_data[0] == 'w' && barrier == 1 && bad_lock == 0,
           "synchronous cached writes wait for pageout before the durability barrier");
    reset(); node.cache_size = backend_size = PAGESIZE; move_limit = 4; move_error = EFAULT;
    uio = make_uio(&vector, data, sizeof(data), PAGESIZE);
    tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_WRITE, 0, &uio, NULL) == EFAULT &&
           uio.uio_resid == 12 && backend_size == PAGESIZE + 4,
           "partial user copies retain only the successfully transferred extension");
    reset(); node.cache_size = backend_size = PAGESIZE; move_limit = 0; move_error = EFAULT;
    uio = make_uio(&vector, data, sizeof(data), 93);
    tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_WRITE, FAPPEND, &uio, NULL) == EFAULT &&
           uio.uio_resid == sizeof(data) && uio.uio_offset == 93 && backend_size == PAGESIZE,
           "failed append preserves incoming position and EOF when no bytes transfer");
    reset();
    tap_ok(&tap, flush_connection_cache(&connection) == 0 && submitted == 1 && waited == 1 &&
           vn.v_count == 1 && locks == 0 && bad_lock == 0,
           "filesystem sync holds each vnode while flushing outside the global lock");
    for (unsigned eof = 0; eof < 2; ++eof) {
        reset(); connection.state = USFS_CONN_UNHEALTHY;
        memset(data, 'z', sizeof(data));
        uio = make_uio(&vector, data, sizeof(data), eof ? node.cache_size : 0);
        tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_READ, 0, &uio, NULL) == EIO &&
               uio.uio_resid == sizeof(data) && data[0] == 'z' && moves == 0,
               "quarantined cached reads reject both resident data and cached EOF");
    }
    reset(); truncate_timeout = 1;
    memset(&change, 0, sizeof(change)); change.valid = USFS_SET_SIZE;
    int rc = resize_cached_file(&vn, &change, 1, NULL);
    uio = make_uio(&vector, data, sizeof(data), 0);
    tap_ok(&tap, rc == ETIMEDOUT && backend_size == 0 && node.cache_size != 0 &&
           transfer_cached_data(&vn, 1, UIO_READ, 0, &uio, NULL) == EIO && moves == 0,
           "a committed truncate with a lost reply cannot expose the old resident cache");
    reset(); quarantine_move = 1; node.cache_size = sizeof(cache_data);
    static char chunks[USFS_MAX_DATA + 1];
    uio = make_uio(&vector, chunks, sizeof(chunks), 0);
    tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_READ, 0, &uio, NULL) == EIO &&
           uio.uio_resid == 1 && moves == 1 && bad_lock == 0 && connection_locks == 0,
           "quarantine between cached chunks preserves the first transfer and rejects the next");
    reset(); connection.state = USFS_CONN_DEAD;
    uio = make_uio(&vector, data, sizeof(data), 0);
    tap_ok(&tap, transfer_cached_data(&vn, 1, UIO_WRITE, 0, &uio, NULL) == EIO &&
           moves == 0 && cache_data[0] == 'x', "dead connections reject cached writes before changing pages");
    return tap_finish(&tap);
}
