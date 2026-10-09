/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

static struct gnode test_gnode;
static struct usfs_node test_node;
static struct usfs_mount_data test_mount;
static struct usfs_open_state test_open;
static struct usfs_request test_request;
static struct usfs_write_in test_body;
static struct usfs_write_out test_reply;
static char transfer_buffer[USFS_MAX_DATA];
static int lock_depth, lock_entries, bad_lock, calls, balance, scenario;
static uint64_t eof;
static int namespace_depth;
enum
{
    COPY_FAULT_AFTER_BYTES = 7,
    COPY_TEST_INITIAL_OFFSET = 19,
    COPY_TEST_PROCESSED_VECTORS = 2,
    COPY_TEST_FIRST_VECTOR_BYTES = 5
};
static int fail_copy_after_bytes = -1;
static unsigned copied_bytes;
void lock_read(complex_lock_t lock) { (void)lock; ++namespace_depth; }
static int usfs_strip_write_privileges(struct vnode *vp, uint64_t fh, struct ucred *crp)
{ (void)vp; (void)fh; (void)crp; return namespace_depth == 1 ? 0 : EIO; }

void lock_write(complex_lock_t lock)
{
    (void)lock;
    if (lock_depth != 0) bad_lock = 1;
    ++lock_depth;
    ++lock_entries;
}

void lock_done(complex_lock_t lock) { if (lock == &test_mount.namespace_lock) --namespace_depth; else --lock_depth; }
static struct usfs_mount_data *_mount_of(struct vnode *vp) { (void)vp; return &test_mount; }
static struct usfs_node *_node_of(struct vnode *vp) { (void)vp; return &test_node; }
static int usfs_borrow_description(struct usfs_node *node, caddr_t vinfo,
                                    struct usfs_open_state **state)
{
    (void)node; (void)vinfo; *state = &test_open; return 0;
}
static void usfs_put_open_reference(struct vnode *virtual_fs_node, struct usfs_open_state *state,
                                    int operation, struct ucred *credentials)
{ (void)virtual_fs_node; (void)state; (void)operation; (void)credentials; }
int gn_getattr(struct vnode *vp, struct vattr *attr, struct ucred *crp)
{ (void)vp; (void)attr; (void)crp; return 0; }

int allocate_request(const struct usfs_request_allocation_spec *spec, struct usfs_request **out)
{
    memcpy(&test_body, spec->opcode_specific_body.bytes, sizeof(test_body));
    memset(&test_request, 0, sizeof(test_request));
    test_request.reply_buffer = (char *)&test_reply;
    test_request.reply_buffer_size = sizeof(test_reply);
    ++balance;
    *out = &test_request;
    return 0;
}
void free_request(struct usfs_request *request) { (void)request; --balance; }
caddr_t usfs_msg_data(const struct usfs_request *request, uint32_t body, uint32_t name)
{ (void)request; (void)body; (void)name; return transfer_buffer; }
int uiomove(caddr_t data, int32long64_t count, enum uio_rw direction, struct uio *uio)
{
    if (scenario == 4) return EFAULT;
    if (uio->uio_offset > INT64_MAX - count) return EOVERFLOW;

    while (count > 0 && uio->uio_resid > 0 && uio->uio_iovcnt > 0)
    {
        struct iovec * vector = uio->uio_iov;
        size_t amount = vector->iov_len;
        if (amount > (size_t)count)
            amount = (size_t)count;
        if (amount > (size_t)uio->uio_resid)
            amount = (size_t)uio->uio_resid;

        if (fail_copy_after_bytes >= 0)
        {
            if (copied_bytes >= (unsigned)fail_copy_after_bytes)
                return EFAULT;
            if (amount > (unsigned)fail_copy_after_bytes - copied_bytes)
                amount = (unsigned)fail_copy_after_bytes - copied_bytes;
        }

        if (amount != 0)
        {
            if (direction == UIO_WRITE)
                memcpy (data, vector->iov_base, amount);
            else
                memcpy (vector->iov_base, data, amount);
            vector->iov_base += amount;
            vector->iov_len -= amount;
            uio->uio_offset += amount;
            uio->uio_resid -= amount;
            copied_bytes += amount;
            data += amount;
            count -= amount;
        }

        if (vector->iov_len == 0)
        {
            uio->uio_iov++;
            uio->uio_iovcnt--;
            uio->uio_iovdcnt++;
            if (uio->uio_xmem != NULL)
                uio->uio_xmem++;
        }
    }
    return 0;
}
int usfs_kuiomove(enum usfs_uiomove_site site, caddr_t data, long count,
                   int direction, struct uio *uio)
{ (void)site; return uiomove(data, count, (enum uio_rw)direction, uio); }
int usfs_call(struct usfs_connection *connection, struct usfs_request *request)
{
    (void)connection;
    ++calls;
    if (lock_depth != 1) bad_lock = 1;
    memset(&test_reply, 0, sizeof(test_reply));
    test_reply.offset = test_body.flags & USFS_WRITE_APPEND ? eof : test_body.offset;
    test_reply.written = test_body.size;
    if (scenario == 1 && calls == 2) test_reply.written = 2;
    if (scenario == 2 && calls == 2) request->error = ENOSPC;
    if (scenario == 3) ++test_reply.offset;
    eof = test_reply.offset + test_reply.written;
    /* A later chunk must retain its position even if a page writeback changed
       backend EOF between requests. */
    if (calls == 1) eof += 17;
    return 0;
}

static int transfer_cached_data(struct vnode *vp, uint64_t fh, enum uio_rw rw,
                            int32long64_t flags, struct uio *uio, struct ucred *crp)
{ (void)vp; (void)fh; (void)rw; (void)flags; (void)uio; (void)crp; return EIO; }
static int _sync_node(struct vnode *vp, uint64_t fh, int32long64_t flags,
                      int range, offset_t offset, offset_t length, struct ucred *crp)
{ (void)vp; (void)fh; (void)flags; (void)range; (void)offset; (void)length; (void)crp; return 0; }
#include "fs/operations/vnode_rdwr.h"

static void test_write_transactions(struct tap_state *tap)
{
    test_node.gn = &test_gnode;
    for (scenario = 0; scenario < 6; ++scenario) {
        struct uio uio;
        static char source[USFS_MAX_DATA + 4];
        struct iovec vectors[] = { { source, USFS_MAX_DATA - 2 }, { source + USFS_MAX_DATA - 2, 6 } };
        struct xmem descriptors[sizeof (vectors) / sizeof (vectors[0])] = { 0 };
        struct ucred credential;
        uint64_t committed = scenario == 1 ? USFS_MAX_DATA + 2 :
                             scenario == 2 ? USFS_MAX_DATA :
                             scenario == 3 || scenario == 4 ? 0 : USFS_MAX_DATA + 4;
        int expected = scenario == 3 ? EIO : scenario == 4 ? EFAULT : 0;
        offset_t initial = scenario == 5 ? INT64_MAX : 123;
        offset_t start = scenario == 3 ? initial : 5;
        int rc;
        memset(&uio, 0, sizeof(uio));
        memset(&credential, 0, sizeof(credential));
        uio.uio_resid = USFS_MAX_DATA + 4;
        uio.uio_iov = vectors;
        uio.uio_iovcnt = sizeof (vectors) / sizeof (vectors[0]);
        uio.uio_xmem = descriptors;
        uio.uio_offset = initial;
        calls = balance = lock_depth = lock_entries = bad_lock = 0;
        copied_bytes = 0;
        eof = 5;

        const struct rdwr_context write_context = {
            .mount_data = &test_mount,
            .node = &test_node,
            .file_handle = 123,
            .open_flags = scenario == 3 ? FWRITE : FWRITE | FAPPEND,
            .user_io_request = &uio,
            .credentials = &credential
        };

        rc = write_file_data (&write_context);
        tap_ok(tap, rc == expected && lock_entries == 1 && lock_depth == 0 && !bad_lock &&
               balance == 0 && uio.uio_resid == (int32long64_t)(USFS_MAX_DATA + 4 - committed) &&
               uio.uio_offset == (committed ? start + (offset_t)committed : initial),
               "one write owns all chunks and preserves committed offsets through short writes and errors");

        const size_t first_committed = committed < USFS_MAX_DATA - 2 ? committed : USFS_MAX_DATA - 2;
        const size_t second_committed = committed - first_committed;
        const unsigned completed_vectors = (first_committed == USFS_MAX_DATA - 2) + (second_committed == 6);
        tap_ok (
            tap,
            vectors[0].iov_base == source + first_committed && vectors[0].iov_len == USFS_MAX_DATA - 2 - first_committed &&
                vectors[1].iov_base == source + USFS_MAX_DATA - 2 + second_committed && vectors[1].iov_len == 6 - second_committed &&
                uio.uio_iov == vectors + completed_vectors &&
                uio.uio_iovcnt == (int32long64_t)(sizeof (vectors) / sizeof (vectors[0]) - completed_vectors) &&
                uio.uio_iovdcnt == completed_vectors && uio.uio_xmem == descriptors + completed_vectors,
            "write vectors advance by committed bytes across chunk boundaries and failures"
        );
    }
}

static void test_partial_copy_failure (struct tap_state * tap)
{
    char source[] = "firstsecond";
    const size_t source_bytes = sizeof (source) - 1;
    struct iovec vectors[] = { { source, 0 },
                               { source, COPY_TEST_FIRST_VECTOR_BYTES },
                               { source + COPY_TEST_FIRST_VECTOR_BYTES, source_bytes - COPY_TEST_FIRST_VECTOR_BYTES } };
    enum
    {
        VECTOR_COUNT = sizeof (vectors) / sizeof (vectors[0])
    };
    struct iovec original_vectors[VECTOR_COUNT];
    struct xmem descriptors[VECTOR_COUNT] = { 0 };
    struct ucred credentials = { 0 };
    struct uio user_io = { 0 };

    memcpy (original_vectors, vectors, sizeof (vectors));
    user_io.uio_iov = vectors;
    user_io.uio_iovcnt = VECTOR_COUNT;
    user_io.uio_xmem = descriptors;
    user_io.uio_iovdcnt = COPY_TEST_PROCESSED_VECTORS;
    user_io.uio_resid = source_bytes;
    user_io.uio_offset = COPY_TEST_INITIAL_OFFSET;
    const struct uio original_io = user_io;
    const struct rdwr_context context = { .mount_data = &test_mount,
                                          .node = &test_node,
                                          .file_handle = 123,
                                          .open_flags = FWRITE,
                                          .user_io_request = &user_io,
                                          .credentials = &credentials };

    scenario = 0;
    calls = balance = lock_depth = lock_entries = bad_lock = 0;
    copied_bytes = 0;
    fail_copy_after_bytes = COPY_FAULT_AFTER_BYTES;
    const int rc = write_file_data (&context);
    fail_copy_after_bytes = -1;

    const int preserved = user_io.uio_iov == original_io.uio_iov && user_io.uio_iovcnt == original_io.uio_iovcnt &&
                          user_io.uio_iovdcnt == original_io.uio_iovdcnt && user_io.uio_xmem == original_io.uio_xmem &&
                          user_io.uio_offset == original_io.uio_offset && user_io.uio_resid == original_io.uio_resid &&
                          user_io.uio_segflg == original_io.uio_segflg && user_io.uio_fmode == original_io.uio_fmode &&
                          memcmp (vectors, original_vectors, sizeof (vectors)) == 0;
    tap_ok (tap, rc == EFAULT && calls == 0 && balance == 0 && preserved, "partial copy fault leaves every caller uio and vector field unchanged");

    copied_bytes = 0;
    const int retry_rc = preserved ? write_file_data (&context) : EIO;
    tap_ok (
        tap,
        retry_rc == 0 && user_io.uio_resid == 0 && user_io.uio_offset == COPY_TEST_INITIAL_OFFSET + (offset_t)source_bytes &&
            user_io.uio_iov == vectors + VECTOR_COUNT && user_io.uio_iovcnt == 0 &&
            user_io.uio_iovdcnt == COPY_TEST_PROCESSED_VECTORS + VECTOR_COUNT && user_io.uio_xmem == descriptors + VECTOR_COUNT &&
            memcmp (transfer_buffer, source, source_bytes) == 0 && balance == 0,
        "retry uses the original bytes and advances vector and cross-memory cursors together"
    );
}

int main(void)
{
    struct tap_state tap;
    struct vnode vnode;
    struct uio uio;
    struct iovec iovec;

    tap_plan (&tap, 24);
    memset(&vnode, 0, sizeof(vnode));
    memset(&uio, 0, sizeof(uio));
    memset(&iovec, 0, sizeof(iovec));
    tap_ok(&tap, usfs_validate_rdwr_arguments(NULL, UIO_READ, &uio) ==
                 EINVAL,
           "rdwr rejects null vnode");
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_READ, NULL) ==
                 EINVAL,
           "rdwr rejects null uio");
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, (enum uio_rw)99,
                                               &uio) == EINVAL,
           "rdwr rejects unknown operation");
    uio.uio_resid = -1;
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_READ, &uio) ==
                 EINVAL,
           "rdwr rejects negative residual");
    uio.uio_resid = 1;
    uio.uio_offset = -1;
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_READ, &uio) ==
                 EINVAL,
           "rdwr rejects negative offset");
    uio.uio_offset = 0;
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_READ, &uio) ==
                 EINVAL,
           "rdwr rejects positive transfer without iovec");
    uio.uio_iov = &iovec;
    uio.uio_iovcnt = 0;
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_READ, &uio) ==
                 EINVAL,
           "rdwr rejects empty iovec array");
    uio.uio_iovcnt = 1;
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_WRITE, &uio) == 0,
           "rdwr accepts a valid write description");
    uio.uio_resid = 0;
    uio.uio_iov = NULL;
    tap_ok(&tap, usfs_validate_rdwr_arguments(&vnode, UIO_READ, &uio) == 0,
           "zero-length rdwr needs no iovec");
    uio.uio_resid = USFS_MAX_DATA + 1;
    tap_ok(&tap, usfs_rdwr_chunk_size(&uio) == USFS_MAX_DATA,
           "rdwr chunk is capped at protocol maximum");
    test_write_transactions(&tap);
    test_partial_copy_failure (&tap);
    return tap_finish(&tap);
}
