/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "support/fake_kernel.h"

#include <string.h>

Complex_lock global_lock;

void lock_write(complex_lock_t lock)
{
    (void)lock;
    usfs_fake_kernel.lock_depth += 1;
}

void lock_done(complex_lock_t lock)
{
    (void)lock;
    usfs_fake_kernel.lock_depth -= 1;
}

void gn_mapcnt(struct gnode *gnode, long flags)
{
    if ((flags & SHM_RDONLY) != 0)
        gnode->gn_mrdcnt += 1;
    else
        gnode->gn_mwrcnt += 1;
}

void gn_unmapcnt(struct gnode *gnode, long flags)
{
    if ((flags & SHM_RDONLY) != 0)
        gnode->gn_mrdcnt -= 1;
    else
        gnode->gn_mwrcnt -= 1;
}

static struct usfs_node *_node_of(struct vnode *vnode)
{
    return (struct usfs_node *)vnode->v_gnode->gn_data;
}

static void usfs_vnode_hold_locked(struct vnode *vnode, int reuse)
{
    (void)reuse;
    vnode->v_count += 1;
}

static void usfs_vnode_release_locked(struct vnode *vnode)
{
    vnode->v_count -= 1;
}

#include "fs/open_state.h"

static struct usfs_mount_data test_mount;
static struct vnode *flush_vnode;
static unsigned release_calls;
static unsigned release_during_flush;
static int flush_running;
static int flush_unmap_rc;
static int cache_flush_error;
static unsigned connection_aborts;

void abort_connection (struct usfs_connection * connection)
{
    (void)connection;
    ++connection_aborts;
}

static struct usfs_mount_data *_mount_of(struct vnode *vnode)
{
    (void)vnode;
    return &test_mount;
}

#undef VNOP_RELE
#define VNOP_RELE(vnode) usfs_vnode_release_locked(vnode)

static void _send_release(struct usfs_mount_data *mount, uint64_t nodeid,
                          uint64_t fh, int32long64_t flags, uint32_t isdir,
                          struct ucred *credential)
{
    (void)mount; (void)nodeid; (void)fh; (void)flags;
    (void)isdir; (void)credential;
    ++release_calls;
    if (flush_running || usfs_fake_kernel.lock_depth != 0)
        ++release_during_flush;
}

static int flush_cached_pages(struct vnode *vp, int range, uint64_t offset,
                            uint64_t length, int discard)
{
    (void)vp; (void)range; (void)offset; (void)length; (void)discard;
    return cache_flush_error;
}

#include "fs/operations/vnode_unmap.h"
#include "fs/open_reference.h"

static int _flush_handle(struct usfs_mount_data *mount, uint64_t nodeid,
                         uint64_t fh, int32long64_t flags,
                         struct ucred *credential)
{
    (void)mount; (void)nodeid; (void)fh; (void)flags;
    flush_running = 1;
    /* Deterministic interleaving: the last unmap completes inside FLUSH. */
    flush_unmap_rc = gn_unmap(flush_vnode, SHM_RDONLY, credential);
    flush_running = 0;
    return EIO;
}

#include "fs/operations/vnode_close.h"

static int test_description_handle(struct usfs_node *node, caddr_t vinfo,
                                    uint64_t *fh)
{
    struct usfs_open_state *state = NULL;
    int rc = usfs_borrow_description(node, vinfo, &state);
    if (rc == 0) {
        *fh = state->fh;
        usfs_put_open_reference(node->vn, state, 1, NULL);
    }
    return rc;
}

static void test_close_flush(struct tap_state *tap)
{
    struct usfs_node node;
    struct gnode gnode;
    struct vnode vnode;
    struct ucred credential;
    struct usfs_open_state *state = NULL;

    usfs_fake_kernel_reset();
    memset(&node, 0, sizeof(node));
    memset(&gnode, 0, sizeof(gnode));
    memset(&vnode, 0, sizeof(vnode));
    memset(&credential, 0, sizeof(credential));
    node.vn = &vnode;
    node.gn = &gnode;
    gnode.gn_data = (caddr_t)&node;
    gnode.gn_type = VREG;
    vnode.v_gnode = &gnode;
    vnode.v_count = 1;
    (void)allocate_open_state(FREAD, &state);
    state->fh = 303;
    attach_open_state(&node, state);
    (void)usfs_retain_mapping_state(&vnode, SHM_RDONLY, NULL);
    flush_vnode = &vnode;
    tap_ok(tap, gn_close(&vnode, FREAD, (caddr_t)state, &credential) == EIO &&
                   flush_unmap_rc == 0 && release_calls == 1 &&
                   release_during_flush == 0,
           "last unmap during failing FLUSH defers exactly one RELEASE until close finishes");
    tap_ok(tap, node.opens == NULL && vnode.v_count == 1 &&
                   usfs_fake_kernel_clean(),
           "close and final unmap balance state and vnode ownership");
}

static void test_borrowed_lifetime(struct tap_state *tap)
{
    struct usfs_node node;
    struct gnode gnode;
    struct vnode vnode;
    struct ucred credential;
    struct usfs_open_state *old = NULL, *replacement = NULL;
    struct usfs_open_state *borrowed = NULL, *selected = NULL;
    unsigned variant;

    for (variant = 0; variant < 3; ++variant) {
        usfs_fake_kernel_reset();
        release_calls = 0;
        release_during_flush = 0;
        memset(&node, 0, sizeof(node));
        memset(&gnode, 0, sizeof(gnode));
        memset(&vnode, 0, sizeof(vnode));
        memset(&credential, 0, sizeof(credential));
        node.vn = &vnode;
        node.gn = &gnode;
        gnode.gn_data = (caddr_t)&node;
        gnode.gn_type = variant == 1 ? VDIR : VREG;
        vnode.v_gnode = &gnode;
        vnode.v_count = 1;
        (void)allocate_open_state(FREAD | FWRITE, &old);
        old->fh = 404;
        attach_open_state(&node, old);
        if (variant == 2)
            (void)usfs_retain_mapping_state(&vnode, SHM_RDONLY, NULL);
        tap_ok(tap, usfs_borrow_description(&node, (caddr_t)old, &borrowed) == 0 &&
                       borrowed == old && old->operation_refs == 1,
               "the exact file description retains an operation reference");
        (void)usfs_close_open_state(&node, (caddr_t)old);
        usfs_put_open_reference(&vnode, old, 0, &credential);
        if (variant == 2)
            (void)gn_unmap(&vnode, SHM_RDONLY, &credential);
        tap_ok(tap, release_calls == 0 && old->refs == 1 &&
                       usfs_borrow_description(&node, (caddr_t)old, &selected) == EBADF,
               "close and final unmap cannot release an operation's borrowed handle");
        (void)allocate_open_state(FREAD, &replacement);
        replacement->fh = 505;
        attach_open_state(&node, replacement);
        tap_ok(tap, replacement != old &&
                       usfs_borrow_description(&node, (caddr_t)replacement, &selected) == 0 &&
                       selected->fh == 505 && borrowed->fh == 404,
               "reused descriptor has independent state while the old operation remains live");
        usfs_put_open_reference(&vnode, selected, 1, &credential);
        (void)usfs_close_open_state(&node, (caddr_t)replacement);
        usfs_put_open_reference(&vnode, replacement, 0, &credential);
        usfs_put_open_reference(&vnode, borrowed, 1, &credential);
        tap_ok(tap, release_calls == 2 && release_during_flush == 0 &&
                       node.opens == NULL && vnode.v_count == 1 &&
                       usfs_fake_kernel_clean(),
               "both handles release exactly once outside locks after their final owners finish");
    }
}

static void test_unmap_flush_failure (struct tap_state * tap)
{
    struct usfs_node node = { 0 };
    struct gnode gnode = { 0 };
    struct vnode vnode = { 0 };
    struct ucred credentials = { 0 };
    struct usfs_open_state * state = NULL;

    usfs_fake_kernel_reset ();
    release_calls = release_during_flush = connection_aborts = 0;
    node.vn = &vnode;
    node.gn = &gnode;
    gnode.gn_data = (caddr_t)&node;
    gnode.gn_type = VREG;
    vnode.v_gnode = &gnode;
    vnode.v_count = 1;
    (void)allocate_open_state (FREAD | FWRITE, &state);
    state->fh = 606;
    attach_open_state (&node, state);
    (void)usfs_retain_mapping_state (&vnode, 0, NULL);
    (void)usfs_close_open_state (&node, (caddr_t)state);
    usfs_put_open_reference (&vnode, state, 0, &credentials);

    cache_flush_error = ENOSPC;
    const int rc = gn_unmap (&vnode, 0, &credentials);
    cache_flush_error = 0;

    tap_ok (tap, rc == ENOSPC && connection_aborts == 1, "unmap returns its writeback error and quarantines the connection");
    tap_ok (
        tap,
        release_calls == 1 && release_during_flush == 0 && node.opens == NULL && node.mapping_count == 0 && gnode.gn_mwrcnt == 0 &&
            vnode.v_count == 1 && usfs_fake_kernel_clean (),
        "failed unmap writeback still releases mapping, handle, and vnode ownership exactly once"
    );
}

int main(void)
{
    struct tap_state tap;
    struct usfs_node node;
    struct gnode gnode;
    struct vnode vnode;
    struct usfs_open_state *reader = NULL;
    struct usfs_open_state *writer = NULL;
    struct usfs_open_state *release = NULL;
    uint64_t fh = 0;

    tap_plan (&tap, 31);
    usfs_fake_kernel_reset();
    memset(&node, 0, sizeof(node));
    memset(&gnode, 0, sizeof(gnode));
    memset(&vnode, 0, sizeof(vnode));
    node.gn = &gnode;
    node.vn = &vnode;
    gnode.gn_vnode = &vnode;
    gnode.gn_data = (caddr_t)&node;
    vnode.v_gnode = &gnode;
    vnode.v_count = 1;

    tap_ok(&tap, allocate_open_state(FREAD, &reader) == 0 &&
                 allocate_open_state(FWRITE, &writer) == 0 &&
                 reader != writer,
           "each open allocates distinct state");
    reader->fh = 101;
    writer->fh = 202;
    attach_open_state(&node, reader);
    attach_open_state(&node, writer);
    tap_ok(&tap, vnode.v_count == 3 && node.opens == writer &&
                 writer->next == reader,
           "attached descriptions independently hold the vnode");
    tap_ok(&tap, test_description_handle(&node, (caddr_t)reader, &fh) == 0 &&
                 fh == 101 &&
                 test_description_handle(&node, (caddr_t)writer, &fh) == 0 &&
                 fh == 202,
           "f_vinfo resolves each description's handle");
    tap_ok(&tap, test_description_handle(&node, (caddr_t)reader, &fh) == 0 &&
                 fh == 101 &&
                 test_description_handle(&node, (caddr_t)writer, &fh) == 0 &&
                 fh == 202,
           "exact file descriptions identify their own open handles");

    tap_ok(&tap, usfs_retain_mapping_state(&vnode, SHM_RDONLY, NULL) == 0 &&
                 usfs_retain_mapping_state(&vnode, 0, NULL) == 0 &&
                 node.mapping_count == 2 && vnode.v_count == 5,
           "read and write mappings retain compatible descriptions");
    tap_ok(&tap, writer->write_mappings != 0 && writer->fh == 202,
           "page-out keeps its writable mapping description");

    tap_ok(&tap, usfs_close_open_state(&node, (caddr_t)reader) == 0 &&
                 usfs_release_open_reference(&node, reader, 0, &release) == 0 &&
                 release == NULL &&
                 !reader->active,
           "closing a mapped reader defers its release");
    usfs_vnode_release_locked(&vnode);
    tap_ok(&tap, usfs_close_open_state(&node, (caddr_t)writer) == 0 &&
                 usfs_release_open_reference(&node, writer, 0, &release) == 0 &&
                 release == NULL &&
                 !writer->active,
           "closing a mapped writer defers its release");
    usfs_vnode_release_locked(&vnode);
    tap_ok(&tap, test_description_handle(&node, (caddr_t)reader, &fh) == EBADF,
           "closed f_vinfo cannot be reused for file I/O");

    tap_ok(&tap, usfs_retain_mapping_state(&vnode, SHM_RDONLY, NULL) == 0 &&
                 usfs_retain_mapping_state(&vnode, 0, NULL) == 0 &&
                 node.mapping_count == 4 && vnode.v_count == 5,
           "forked mappings retain their closed backing descriptions");
    tap_ok(&tap, usfs_release_mapping_state(&vnode, SHM_RDONLY,
                                             &release) == 0 &&
                 release == NULL &&
                 usfs_release_mapping_state(&vnode, 0, &release) == 0 &&
                 release == NULL && node.mapping_count == 2,
           "first inherited unmaps keep the original mappings alive");
    usfs_vnode_release_locked(&vnode);
    usfs_vnode_release_locked(&vnode);

    tap_ok(&tap, usfs_release_mapping_state(&vnode, SHM_RDONLY,
                                             &release) == 0 &&
                 release == reader && release->fh == 101,
           "last read unmap returns the reader for release");
    usfs_open_state_discard(release);
    usfs_vnode_release_locked(&vnode);
    release = NULL;
    tap_ok(&tap, usfs_release_mapping_state(&vnode, 0, &release) == 0 &&
                 release == writer && release->fh == 202,
           "last write unmap returns the writer for release");
    usfs_open_state_discard(release);
    usfs_vnode_release_locked(&vnode);
    tap_ok(&tap, node.opens == NULL && node.mapping_count == 0 &&
                 vnode.v_count == 1,
           "all description and mapping references balance");
    tap_ok(&tap, usfs_fake_kernel_clean(),
           "open-state helpers leave fake kernel ownership balanced");
    test_close_flush(&tap);
    test_borrowed_lifetime(&tap);
    test_unmap_flush_failure (&tap);
    return tap_finish(&tap);
}
