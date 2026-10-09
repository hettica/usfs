/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include "device/protocol/connection_refs.h"

#include "device/operations/open.h"
#include "device/operations/close.h"
#include "device/operations/ioctl.h"
#include "device/operations/mpx.h"
#include "device/operations/read.h"
#include "device/operations/select.h"
#include "device/operations/write.h"

#include <string.h>

struct usfs_connection *g_connections[USFS_MAX_CONNECTIONS];
Simple_lock g_connections_table_lock;
Simple_lock g_lifecycle_lock;
struct usfs_runtime_limits g_usfs_runtime_limits;
struct gfs gfs;
heapaddr_t kernel_heap;
int g_gate_is_open;
int g_kext_is_running_control_operation;
int g_status_channel_present;

static struct usfs_connection allocated_connection;
static struct usfs_reply_header reply_input;
static int allocation_fails;
static int copyin_rc;
static int copyout_rc;
static int uiomove_rc;
static int kuiomove_rc;
static int sleep_rc;
static int checkpoint_rc;
static int lifecycle_state;
static int privilege_rc;
static int kext_snapshot_rc;
static int runtime_snapshot_rc;
static int runtime_set_rc;
static unsigned free_lock_calls;
static struct usfs_connection *copy_connection;
static struct usfs_request *copy_request;
static int abandon_during_copy;
static int disconnect_during_copy;
static int references_during_copy;
static unsigned abort_calls;
static unsigned connection_lost_calls;
static unsigned protocol_error_calls;
static unsigned free_connection_calls;
static unsigned wake_calls;
static unsigned release_slot_calls;
static unsigned free_request_calls;
static unsigned mpx_callbacks_entered;
static unsigned mpx_callbacks_left;
static unsigned mpx_callbacks_in_flight;

int usfs_lifecycle_mpx_enter(const int is_deallocation)
{
    (void)is_deallocation;
    mpx_callbacks_entered += 1;
    mpx_callbacks_in_flight += 1;
    return 0;
}

void usfs_lifecycle_mpx_leave(void)
{
    mpx_callbacks_left += 1;
    mpx_callbacks_in_flight -= 1;
}

struct usfs_connection *get_connection_by_channel(chan_t channel)
{
    if (channel < 0 || channel >= USFS_MAX_CONNECTIONS)
        return NULL;
    return g_connections[channel];
}

int is_channel_valid(chan_t channel)
{
    return channel >= 0 && channel < USFS_MAX_CONNECTIONS;
}

void abort_connection(struct usfs_connection *connection)
{
    connection->state = USFS_CONN_DEAD;
    abort_calls += 1;
}

void report_connection_lost(const struct usfs_connection *connection)
{
    (void)connection;
    connection_lost_calls += 1;
}

void report_protocol_error(const struct usfs_connection *connection, int error,
                           uint16_t opcode, uint64_t request_id)
{
    (void)connection;
    (void)error;
    (void)opcode;
    (void)request_id;
    protocol_error_calls += 1;
}

void simple_lock(simple_lock_t lock)
{
    (void)lock;
}

void simple_unlock(simple_lock_t lock)
{
    (void)lock;
}

void simple_lock_init(simple_lock_t lock)
{
    (void)lock;
}

void lock_alloc(void *lock, int flags, short class_id, uint occurrence)
{
    (void)lock;
    (void)flags;
    (void)class_id;
    (void)occurrence;
}

void lock_free(void *lock)
{
    (void)lock;
    free_lock_calls += 1;
}

void *usfs_kmalloc(enum usfs_allocation_site site, uint size, int alignment,
                   heapaddr_t heap)
{
    (void)site;
    (void)size;
    (void)alignment;
    (void)heap;
    return allocation_fails ? NULL : &allocated_connection;
}

int xmfree(void *allocation, heapaddr_t heap)
{
    (void)allocation;
    (void)heap;
    free_connection_calls += 1;
    return 0;
}

int usfs_lifecycle_state_read(void)
{
    return lifecycle_state;
}

int usfs_checkpoint(enum usfs_checkpoint checkpoint)
{
    (void)checkpoint;
    return checkpoint_rc;
}

int copyin(void *source, void *destination, size_t size)
{
    if (copyin_rc != 0 || source == NULL || destination == NULL)
        return EFAULT;
    memcpy(destination, source, size);
    return 0;
}

int copyout(void *source, void *destination, size_t size)
{
    if (copyout_rc != 0 || source == NULL || destination == NULL)
        return EFAULT;
    memcpy(destination, source, size);
    return 0;
}

int privcheck(int privilege)
{
    (void)privilege;
    return privilege_rc;
}

int take_kext_state_snapshot(struct kext_state *state, chan_t channel)
{
    (void)channel;
    if (kext_snapshot_rc != 0)
        return kext_snapshot_rc;
    memset(state, 0, sizeof(*state));
    return 0;
}

int take_kext_runtime_config_snapshot(struct kext_runtime_config *config)
{
    if (runtime_snapshot_rc != 0)
        return runtime_snapshot_rc;
    memset(config, 0, sizeof(*config));
    return 0;
}

int set_kext_runtime_config(const struct kext_runtime_config *config)
{
    (void)config;
    return runtime_set_rc;
}

int is_instrumentation_dev_ctl_command(int command)
{
    (void)command;
    return 0;
}

int process_instrumentation_dev_ctl_command(int command,
                                            void *user_space_buffer,
                                            chan_t channel)
{
    (void)command;
    (void)user_space_buffer;
    (void)channel;
    return EINVAL;
}

int e_sleep_thread(tid_t *event, void *lock, int flags)
{
    (void)event;
    (void)lock;
    (void)flags;
    return sleep_rc;
}

int uiomove(caddr_t buffer, int32long64_t size, enum uio_rw direction,
            struct uio *user_io)
{
    if (direction == UIO_READ) {
        if (copy_connection != NULL)
            references_during_copy = copy_connection->refs_counter;
        if (abandon_during_copy) {
            copy_request->abandoned = 1;
            copy_connection->state = USFS_CONN_UNHEALTHY;
        }
        if (disconnect_during_copy)
            copy_connection->state = USFS_CONN_DEAD;
    }
    if (uiomove_rc != 0)
        return uiomove_rc;
    if (direction == UIO_WRITE)
        memcpy(buffer, &reply_input, (size_t)size);
    user_io->uio_resid -= size;
    return 0;
}

int usfs_kuiomove(enum usfs_uiomove_site site, caddr_t address, long count,
                  int direction, struct uio *user_io)
{
    (void)site;
    (void)address;
    (void)count;
    (void)direction;
    (void)user_io;
    if (site == USFS_UIOMOVE_REPLY_BODY && abandon_during_copy) {
        copy_request->abandoned = 1;
        copy_connection->state = USFS_CONN_UNHEALTHY;
    }
    return kuiomove_rc;
}

void wake_request_up(const struct usfs_request *request)
{
    (void)request;
    wake_calls += 1;
}

void release_request_slot(struct usfs_connection *connection,
                          struct usfs_request *request)
{
    (void)connection;
    (void)request;
    release_slot_calls += 1;
}

void free_request(struct usfs_request *request)
{
    (void)request;
    free_request_calls += 1;
}

static void reset_state(void)
{
    memset(g_connections, 0, sizeof(g_connections));
    memset(&allocated_connection, 0, sizeof(allocated_connection));
    memset(&reply_input, 0, sizeof(reply_input));
    g_gate_is_open = 1;
    g_kext_is_running_control_operation = 0;
    g_status_channel_present = 0;
    lifecycle_state = USFS_KEXT_ACTIVE;
    g_usfs_runtime_limits.request_timeout_ms = USFS_DEFAULT_REQUEST_TIMEOUT_MS;
    g_usfs_runtime_limits.pager_timeout_ms = USFS_DEFAULT_PAGER_TIMEOUT_MS;
    g_usfs_runtime_limits.max_outstanding_requests = USFS_DEFAULT_MAX_OUTSTANDING;
    allocation_fails = 0;
    copyin_rc = 0;
    copyout_rc = 0;
    uiomove_rc = 0;
    kuiomove_rc = 0;
    sleep_rc = 0;
    checkpoint_rc = 0;
    privilege_rc = 0;
    kext_snapshot_rc = 0;
    runtime_snapshot_rc = 0;
    runtime_set_rc = 0;
    free_lock_calls = 0;
    copy_connection = NULL;
    copy_request = NULL;
    abandon_during_copy = 0;
    disconnect_during_copy = 0;
    references_during_copy = 0;
    abort_calls = 0;
    connection_lost_calls = 0;
    protocol_error_calls = 0;
    free_connection_calls = 0;
    wake_calls = 0;
    release_slot_calls = 0;
    free_request_calls = 0;
    mpx_callbacks_entered = 0;
    mpx_callbacks_left = 0;
    mpx_callbacks_in_flight = 0;
}

static void test_read_ownership(struct tap_state *tap)
{
    struct usfs_request request;
    struct usfs_in_hdr header;
    struct uio user_io;
    struct usfs_connection *connection;
    chan_t channel;
    int variant;
    int cycle;
    int balanced = 1;

    reset_state();
    (void)usfs_dev_mpx(0, &channel, "");
    connection = g_connections[channel];
    memset(&user_io, 0, sizeof(user_io));
    connection->state = USFS_CONN_DEAD;
    tap_ok(tap, usfs_dev_read(0, &user_io, channel, 0) == EIO &&
                   connection->refs_counter == 1,
           "complete read releases its connection after inactive rejection");
    connection->state = USFS_CONN_ACTIVE;
    user_io.uio_fmode = FNONBLOCK;
    tap_ok(tap, usfs_dev_read(0, &user_io, channel, 0) == EAGAIN &&
                   connection->refs_counter == 1,
           "complete nonblocking read releases its connection on an empty queue");
    memset(&request, 0, sizeof(request));
    request.request_buffer_size = sizeof(header);
    connection->pending_requests_head = &request;
    connection->pending_requests_tail = &request;
    tap_ok(tap, usfs_dev_read(0, &user_io, channel, 0) == EMSGSIZE &&
                   connection->refs_counter == 1,
           "complete read releases its connection after rejecting a short buffer");
    connection->pending_requests_head = NULL;
    connection->pending_requests_tail = NULL;
    user_io.uio_fmode = 0;
    sleep_rc = THREAD_INTERRUPTED;
    tap_ok(tap, usfs_dev_read(0, &user_io, channel, 0) == EINTR &&
                   connection->refs_counter == 1,
           "complete read releases its connection after an interrupted wait");
    (void)usfs_dev_mpx(0, &channel, NULL);

    for (variant = 0; variant < 6; ++variant) {
        reset_state();
        (void)usfs_dev_mpx(0, &channel, "");
        connection = g_connections[channel];
        memset(&request, 0, sizeof(request));
        memset(&header, 0, sizeof(header));
        memset(&user_io, 0, sizeof(user_io));
        request.request_buffer = (char *)&header;
        request.request_buffer_size = sizeof(header);
        request.state = USFS_REQ_PENDING;
        connection->pending_requests_head = &request;
        connection->pending_requests_tail = &request;
        user_io.uio_resid = sizeof(header);
        copy_connection = connection;
        copy_request = &request;
        abandon_during_copy = variant == 2 || variant == 3 || variant == 5;
        disconnect_during_copy = variant >= 4;
        uiomove_rc = variant == 1 || variant == 3 ? EFAULT : 0;
        tap_ok(tap, usfs_dev_read(0, &user_io, channel, 0) == uiomove_rc &&
                       references_during_copy == 2 &&
                       connection->refs_counter == 1 &&
                       free_connection_calls == 0 && free_lock_calls == 0 &&
                       (variant == 0 ?
                        connection->delivered_requests_head == &request :
                        abandon_during_copy ? free_request_calls == 1 :
                        request.state == USFS_REQ_ABORTED),
               "complete delivery balances its temporary reference in every copy state");
        (void)usfs_dev_mpx(0, &channel, NULL);
    }

    reset_state();
    for (cycle = 0; cycle < 32; ++cycle) {
        if (usfs_dev_mpx(0, &channel, "") != 0) {
            balanced = 0;
            break;
        }
        connection = g_connections[channel];
        acquire_connection(connection); /* Mount ownership. */
        acquire_connection(connection); /* Request ownership. */
        memset(&request, 0, sizeof(request));
        memset(&header, 0, sizeof(header));
        memset(&user_io, 0, sizeof(user_io));
        request.request_buffer = (char *)&header;
        request.request_buffer_size = sizeof(header);
        connection->pending_requests_head = &request;
        connection->pending_requests_tail = &request;
        user_io.uio_resid = sizeof(header);
        copy_connection = connection;
        if (usfs_dev_read(0, &user_io, channel, 0) != 0 ||
            references_during_copy != 4 ||
            connection->refs_counter != 3)
            balanced = 0;
        connection->delivered_requests_head = NULL;
        release_connection(connection); /* Completed request. */
        (void)usfs_dev_close(0, channel);
        (void)usfs_dev_mpx(0, &channel, NULL);
        if (connection->refs_counter != 1 ||
            free_connection_calls != (unsigned)cycle)
            balanced = 0;
        release_connection(connection); /* Final unmount. */
        if (free_connection_calls != (unsigned)cycle + 1 ||
            free_lock_calls != (unsigned)cycle + 1)
            balanced = 0;
    }
    tap_ok(tap, balanced,
           "32 request-close-unmount cycles reclaim each connection and lock exactly once");
}

static void test_ownership_replies(struct tap_state *tap)
{
    const uint16_t opcodes[] = {USFS_OP_LOOKUP, USFS_OP_OPEN, USFS_OP_CREATE, USFS_OP_CREATE_ATTR};
    for (unsigned op = 0; op < 4; ++op) for (unsigned variant = 0; variant < 7; ++variant) {
        struct { struct usfs_in_hdr header; struct usfs_create_attr_in body; } input;
        union { struct usfs_entry_out entry; struct usfs_open_out open; struct usfs_create_out create; } output;
        struct usfs_connection connection;
        struct usfs_request request;
        struct uio uio;
        uint32_t length;
        reset_state();
        memset(&input, 0, sizeof(input)); memset(&output, 0, sizeof(output));
        memset(&connection, 0, sizeof(connection)); memset(&request, 0, sizeof(request));
        memset(&uio, 0, sizeof(uio));
        input.header.opcode = opcodes[op]; input.body.activation = USFS_CREATE_OPEN;
        if (op == 1) {
            length = sizeof(output.open); output.open.fh = 17;
            if (variant == 3) output.open.pad = 1;
        } else {
            struct usfs_attr *attr = op == 0 ? &output.entry.attr : &output.create.attr;
            length = op == 0 ? sizeof(output.entry) : sizeof(output.create);
            output.create.nodeid = 2;
            attr->ino = 2; attr->blksize = 4096; attr->mode = 0100644;
            if (variant == 3) attr->pad = 1;
        }
        connection.state = USFS_CONN_ACTIVE; connection.refs_counter = 1;
        connection.delivered_requests_head = &request; g_connections[6] = &connection;
        request.id = 17; request.state = USFS_REQ_SENT;
        request.request_buffer = (caddr_t)&input; request.reply_buffer = (caddr_t)&output;
        request.max_allowed_reply_buffer_size = length;
        reply_input.version = USFS_PROTOCOL_VERSION; reply_input.opcode = opcodes[op]; reply_input.id = 17;
        if (variant == 1) { reply_input.error = ENOSPC; length = 0; }
        if (variant == 2) --length;
        if (variant == 4 || variant == 5) kuiomove_rc = EFAULT;
        if (variant >= 5) {
            abandon_during_copy = 1; copy_connection = &connection; copy_request = &request;
        }
        reply_input.len = sizeof(reply_input) + length; uio.uio_resid = reply_input.len;
        int rc = usfs_dev_write(0, &uio, 6, 0);
        int malformed = variant >= 2 && variant <= 5;
        int expected = variant == 2 || variant == 3 ? EINVAL :
                       variant == 4 || variant == 5 ? EFAULT : 0;
        tap_ok(tap, rc == expected && abort_calls == (unsigned)malformed &&
               protocol_error_calls == (unsigned)malformed && connection.refs_counter == 1 &&
               connection.delivered_requests_head == NULL &&
               (variant >= 5 ? free_request_calls == 1 && release_slot_calls == 1 && wake_calls == 0 :
                free_request_calls == 0 && wake_calls == 1 &&
                request.state == (malformed ? USFS_REQ_ABORTED : USFS_REQ_ANSWERED)) &&
               (variant != 1 || request.error == ENOSPC),
               "ownership replies validate before publication and preserve copy, timeout, errno and cleanup ownership");
    }
    for (uint32_t activation = USFS_CREATE_DEFAULT; activation <= USFS_CREATE_OPEN; ++activation) {
        struct { struct usfs_in_hdr header; struct usfs_create_attr_in body; } input = {0};
        struct usfs_create_out output = {0};
        struct usfs_request request = {0};
        input.header.opcode = USFS_OP_CREATE_ATTR; input.body.activation = activation;
        output.nodeid = activation == USFS_CREATE_DEFAULT ? 0 : 2;
        output.attr.ino = 2; output.attr.blksize = 4096; output.attr.mode = 0100644;
        request.request_buffer = (char *)&input; request.reply_buffer = (char *)&output;
        int valid = ownership_reply_valid(&request, sizeof(output));
        if (activation == USFS_CREATE_DEFAULT) output.nodeid = 2;
        else if (activation == USFS_CREATE_LOOKUP) output.fh = 1;
        else output.nodeid = USFS_ROOT_ID;
        tap_ok(tap, valid && !ownership_reply_valid(&request, sizeof(output)),
               "CREATE_ATTR ownership validation derives activation from the original request");
    }
}

static void test_reply_cleanup (struct tap_state * tap, const int abandoned_before_reply)
{
    reset_state ();

    const chan_t channel = 6;
    const uint64_t request_id = 17;
    struct usfs_connection connection = { 0 };
    struct usfs_request request = { 0 };
    struct usfs_in_hdr request_header = { 0 };
    struct uio user_io = { 0 };
    uint64_t reply_payload = 0;

    connection.state = USFS_CONN_ACTIVE;
    connection.refs_counter = 1;
    connection.delivered_requests_head = &request;
    g_connections[channel] = &connection;

    request_header.opcode = USFS_OP_READ;
    request.id = request_id;
    request.state = USFS_REQ_SENT;
    request.abandoned = abandoned_before_reply;
    request.request_buffer = (char *)&request_header;
    request.reply_buffer = (char *)&reply_payload;
    request.max_allowed_reply_buffer_size = sizeof (reply_payload);

    reply_input.version = USFS_PROTOCOL_VERSION;
    reply_input.opcode = USFS_OP_READ;
    reply_input.id = request_id;
    reply_input.len = sizeof (reply_input) + sizeof (reply_payload);
    user_io.uio_resid = reply_input.len;
    kuiomove_rc = EFAULT;

    const int rc = usfs_dev_write (0, &user_io, channel, 0);

    tap_ok (
        tap,
        connection.refs_counter == 1 && connection.delivered_requests_head == NULL && abort_calls == 0,
        "ordinary reply cleanup releases its temporary reference without aborting the connection"
    );

    if (abandoned_before_reply)
    {
        tap_ok (
            tap,
            rc == 0 && user_io.uio_resid == 0 && free_request_calls == 1 && release_slot_calls == 1 && wake_calls == 0,
            "already abandoned reply skips the failing body copy and consumes the remaining input"
        );
        return;
    }

    tap_ok (
        tap,
        rc == EFAULT && user_io.uio_resid == sizeof (reply_payload) && request.state == USFS_REQ_ABORTED && free_request_calls == 0 &&
            release_slot_calls == 0 && wake_calls == 1,
        "ordinary reply copy failure preserves residual input and returns request ownership to the waiter"
    );
}

int main(void)
{
    struct tap_state tap;
    struct usfs_connection connection;
    struct usfs_request request;
    struct usfs_request previous;
    struct usfs_in_hdr request_header;
    struct kext_runtime_config runtime_config;
    struct kext_state status;
    struct uio user_io;
    struct usfs_request *taken;
    ushort selected = 0;
    chan_t channel;
    int index;

    tap_plan (&tap, 113);
    reset_state();
    memset(&connection, 0, sizeof(connection));
    connection.refs_counter = 1;
    memset(&request, 0, sizeof(request));
    memset(&previous, 0, sizeof(previous));
    memset(&request_header, 0, sizeof(request_header));
    memset(&runtime_config, 0, sizeof(runtime_config));
    memset(&user_io, 0, sizeof(user_io));

    tap_ok(&tap, usfs_dev_open(0, 0, -1, 0) == ENXIO,
           "open rejects an invalid channel");
    connection.state = USFS_CONN_DEAD;
    g_connections[2] = &connection;
    tap_ok(&tap, usfs_dev_open(0, 0, 2, 0) == ENXIO,
           "open rejects an inactive connection");
    connection.state = USFS_CONN_ACTIVE;
    tap_ok(&tap, usfs_dev_open(0, 0, 2, 0) == 0,
           "open accepts an active connection");
    tap_ok(&tap, usfs_dev_close(0, -1) == 0 && abort_calls == 0,
           "close tolerates an already absent channel");
    tap_ok(&tap, usfs_dev_close(0, 2) == 0 && abort_calls == 1 &&
                     connection_lost_calls == 1,
           "close aborts an existing connection");

    tap_ok(&tap, usfs_dev_mpx(0, NULL, "") == EINVAL,
           "mpx rejects a null channel output pointer");
    channel = 17;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "named") == EINVAL && channel == 17,
           "mpx rejects named channels without changing output");
    channel = -1;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, NULL) == EINVAL,
           "mpx rejects deallocation of an invalid channel");
    channel = 4;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, NULL) == EINVAL,
           "mpx rejects deallocation of an unused channel");
    allocation_fails = 1;
    channel = 4;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "") == ENOMEM && channel == -1,
           "mpx reports connection allocation failure");
    allocation_fails = 0;
    g_gate_is_open = 0;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "") == EBUSY &&
                     free_connection_calls == 1,
           "mpx rejects allocation while lifecycle admission is closed");
    checkpoint_rc = EIO;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "") == EIO &&
                     free_connection_calls == 2,
           "mpx preserves a rejected-admission checkpoint failure");
    checkpoint_rc = 0;
    g_gate_is_open = 1;
    g_kext_is_running_control_operation = 1;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "") == EBUSY &&
                     free_connection_calls == 3,
           "mpx rejects allocation during a lifecycle control operation");
    g_kext_is_running_control_operation = 0;
    lifecycle_state = USFS_KEXT_STOPPING;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "") == EBUSY &&
                     free_connection_calls == 4,
           "mpx rejects allocation outside the active lifecycle");
    lifecycle_state = USFS_KEXT_ACTIVE;
    free_connection_calls = 0;
    for (index = 0; index < USFS_MAX_CONNECTIONS; index++)
        g_connections[index] = &connection;
    tap_ok(&tap, usfs_dev_mpx(0, &channel, "") == EBUSY &&
                     free_connection_calls == 1,
           "mpx rejects allocation when every channel is occupied");

    tap_ok (
        &tap,
        usfs_dev_mpx (0, &channel, "status") == 0 && channel == USFS_STATUS_CHANNEL && free_connection_calls == 1 &&
            g_status_channel_present,
        "status channel remains available with every backend slot occupied"
    );
    tap_ok (
        &tap,
        usfs_dev_mpx (0, &channel, "status") == 0 && channel == USFS_STATUS_CHANNEL && g_status_channel_present,
        "repeated status allocation shares one channel ownership flag"
    );
    tap_ok (
        &tap,
        usfs_dev_open (0, 0, channel, 0) == 0 && usfs_dev_close (0, channel) == 0 && abort_calls == 1,
        "status open and close do not affect backend connections"
    );
    tap_ok (&tap, g_status_channel_present, "status channel remains owned until its final mpx deallocation");
    tap_ok (&tap, usfs_dev_ioctl (0, USFS_IOC_GET_KEXT_STATE, &status, 0, channel, 0) == 0, "status channel permits the health snapshot ioctl");
    tap_ok (
        &tap,
        usfs_dev_ioctl (0, USFS_IOC_CONNECTION_REQUEST, &request, 0, channel, 0) == EINVAL,
        "status channel cannot become a backend connection"
    );
    tap_ok (&tap, usfs_dev_ioctl (0, USFS_IOC_SET_RUNTIME_CONFIG, &request, 0, channel, 0) == EINVAL, "status channel cannot change runtime policy");
    tap_ok (
        &tap,
        usfs_dev_read (0, &user_io, channel, 0) == ENXIO && usfs_dev_write (0, &user_io, channel, 0) == ENXIO &&
            usfs_dev_select (0, POLLIN, &selected, channel) == ENXIO,
        "status channel rejects daemon data operations"
    );
    tap_ok (
        &tap,
        usfs_dev_mpx (0, &channel, NULL) == 0 && !g_status_channel_present && g_connections[USFS_MAX_CONNECTIONS - 1] == &connection,
        "closing status does not deallocate a backend slot"
    );

    g_gate_is_open = 0;
    tap_ok (&tap, usfs_dev_mpx (0, &channel, "status") == EBUSY, "status channel obeys the lifecycle admission gate");
    g_gate_is_open = 1;
    tap_ok (&tap, usfs_dev_mpx (0, &channel, "status/extra") == EINVAL, "unknown status extensions remain unavailable");
    tap_ok (&tap, mpx_callbacks_entered == mpx_callbacks_left && mpx_callbacks_in_flight == 0,
            "mpx callbacks release their lifetime lease on every path");

    reset_state();
    memset(&connection, 0, sizeof(connection));
    connection.refs_counter = 1;
    connection.state = USFS_CONN_ACTIVE;
    g_connections[3] = &connection;
    tap_ok(&tap, usfs_dev_ioctl(0, -1, NULL, 0, 3, 0) == EINVAL,
           "ioctl rejects an unknown command");
    tap_ok(&tap, process_public_dev_ctl_command(0, -1, NULL, 0, 3, 0) == EINVAL,
           "public ioctl dispatcher rejects an unknown command");
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_CONNECTION_REQUEST, NULL, 0, -1, 0) ==
               ENXIO,
           "connection request rejects an invalid channel");
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_CONNECTION_REQUEST, NULL, 0, 3, 0) ==
                   EFAULT &&
               connection.ready == 0,
           "connection request rejects a null output buffer");
    kext_snapshot_rc = EIO;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_GET_KEXT_STATE, NULL, 0, 3, 0) == EIO,
           "state query preserves snapshot failure");
    kext_snapshot_rc = 0;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_GET_KEXT_STATE, NULL, 0, 3, 0) == EFAULT,
           "state query rejects a null output buffer");
    runtime_snapshot_rc = EBUSY;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_GET_RUNTIME_CONFIG, NULL, 0, 3, 0) ==
               EBUSY,
           "runtime query preserves snapshot failure");
    runtime_snapshot_rc = 0;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_GET_RUNTIME_CONFIG, NULL, 0, 3, 0) ==
               EFAULT,
           "runtime query rejects a null output buffer");
    privilege_rc = EPERM;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_SET_RUNTIME_CONFIG, &runtime_config, 0,
                          3, 0) == EPERM,
           "runtime update requires device configuration privilege");
    privilege_rc = 0;
    copyin_rc = EFAULT;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_SET_RUNTIME_CONFIG, &runtime_config, 0,
                          3, 0) == EFAULT,
           "runtime update preserves copyin failure");
    copyin_rc = 0;
    runtime_set_rc = EINVAL;
    tap_ok(&tap,
           usfs_dev_ioctl(0, USFS_IOC_SET_RUNTIME_CONFIG, &runtime_config, 0,
                          3, 0) == EINVAL,
           "runtime update preserves configuration validation failure");

    reset_state();
    memset(&connection, 0, sizeof(connection));
    connection.refs_counter = 1;
    memset(&request, 0, sizeof(request));
    memset(&user_io, 0, sizeof(user_io));
    connection.state = USFS_CONN_ACTIVE;
    g_connections[5] = &connection;
    tap_ok(&tap, usfs_dev_read(0, NULL, 5, 0) == EINVAL,
           "read rejects a null uio");
    tap_ok(&tap, usfs_dev_read(0, &user_io, -1, 0) == ENXIO,
           "read rejects an invalid channel");
    connection.state = USFS_CONN_DEAD;
    tap_ok(&tap, take_pending_request(&connection, &user_io, &taken) == EIO,
           "read rejects an inactive connection");
    connection.state = USFS_CONN_ACTIVE;
    user_io.uio_fmode = FNONBLOCK;
    tap_ok(&tap, take_pending_request(&connection, &user_io, &taken) == EAGAIN,
           "nonblocking read rejects an empty request queue");
    request.request_buffer_size = 64;
    connection.pending_requests_head = &request;
    connection.pending_requests_tail = &request;
    user_io.uio_resid = 16;
    tap_ok(&tap, take_pending_request(&connection, &user_io, &taken) == EMSGSIZE,
           "read rejects a user buffer smaller than the pending request");
    connection.pending_requests_head = NULL;
    connection.pending_requests_tail = NULL;
    user_io.uio_fmode = 0;
    sleep_rc = THREAD_INTERRUPTED;
    tap_ok(&tap, take_pending_request(&connection, &user_io, &taken) == EINTR,
           "blocking read preserves an interrupted wait");
    request.abandoned = 0;
    request.state = USFS_REQ_SENDING;
    tap_ok(&tap,
           finish_request_delivery(&connection, &request, EFAULT) == EFAULT &&
               request.state == USFS_REQ_ABORTED && wake_calls == 1,
           "delivery copy failure aborts and wakes the request");
    request.abandoned = 0;
    request.state = USFS_REQ_SENDING;
    connection.state = USFS_CONN_DEAD;
    tap_ok(&tap,
           finish_request_delivery(&connection, &request, 0) == 0 &&
               request.state == USFS_REQ_ABORTED && wake_calls == 2,
           "delivery aborts when the connection dies during copyout");
    request.abandoned = 1;
    tap_ok(&tap,
           finish_request_delivery(&connection, &request, EFAULT) == EFAULT &&
               release_slot_calls == 1 && free_request_calls == 1,
           "abandoned failed delivery releases request ownership");
    connection.state = USFS_CONN_UNHEALTHY;
    request.abandoned = 1;
    request.next = NULL;
    tap_ok(&tap,
           finish_request_delivery(&connection, &request, 0) == 0 &&
               connection.delivered_requests_head == NULL &&
               release_slot_calls == 2 && free_request_calls == 2,
           "timed-out successful copy releases ownership without requeueing");

    reset_state();
    memset(&connection, 0, sizeof(connection));
    connection.refs_counter = 1;
    memset(&request, 0, sizeof(request));
    memset(&previous, 0, sizeof(previous));
    memset(&request_header, 0, sizeof(request_header));
    memset(&user_io, 0, sizeof(user_io));
    connection.state = USFS_CONN_ACTIVE;
    g_connections[6] = &connection;
    tap_ok(&tap, usfs_dev_write(0, NULL, 6, 0) == EINVAL,
           "write rejects a null uio");
    tap_ok(&tap, usfs_dev_write(0, &user_io, -1, 0) == ENXIO,
           "write rejects an invalid channel");

    struct reply_reception reply = { 0 };

    reply.connection = &connection;

    user_io.uio_resid = sizeof (reply.header) - 1;
    tap_ok (&tap, read_reply_header (&reply, &user_io) == EINVAL,
           "reply receive rejects a truncated header");
    user_io.uio_resid = sizeof (reply.header);
    uiomove_rc = EFAULT;
    tap_ok (&tap, read_reply_header (&reply, &user_io) == EFAULT,
           "reply receive preserves header copy failure");
    uiomove_rc = 0;
    reply_input.version = USFS_PROTOCOL_VERSION + 1;
    reply_input.len = sizeof(reply_input);
    user_io.uio_resid = sizeof (reply.header);
    tap_ok (&tap, read_reply_header (&reply, &user_io) == EINVAL,
           "reply receive rejects a malformed header");
    reply_input.version = USFS_PROTOCOL_VERSION;
    reply_input.len = sizeof(reply_input) + 4;
    user_io.uio_resid = sizeof (reply.header) + 3;
    tap_ok (&tap, read_reply_header (&reply, &user_io) == EINVAL,
           "reply receive rejects a body length that exceeds residual data");

    reply.header.version = USFS_PROTOCOL_VERSION;
    reply.header.len = sizeof (reply.header);
    reply.header.id = 99;
    tap_ok(&tap,
           claim_reply_request (&reply) == EINVAL,
           "reply claim rejects an unknown request id");
    request.id = 99;
    request.request_buffer = (char *)&request_header;
    request.max_allowed_reply_buffer_size = 8;
    connection.delivered_requests_head = &request;
    request_header.opcode = USFS_OP_READ;
    reply.header.opcode = USFS_OP_WRITE;
    tap_ok(&tap,
           claim_reply_request (&reply) == EINVAL,
           "reply claim rejects an opcode mismatch");
    reply.header.opcode = USFS_OP_READ;
    reply.body_length = 9;
    tap_ok(&tap,
           claim_reply_request (&reply) == EINVAL,
           "reply claim rejects a body larger than request capacity");

    reply.body_length = 0;
    reply.request = &request;
    request.abandoned = 1;
    tap_ok(&tap,
           finish_receiving_reply (&reply, 0) == true,
           "reply completion returns abandoned ownership to the writer");
    request.abandoned = 0;
    request.state = USFS_REQ_REPLYING;
    tap_ok(&tap,
           finish_receiving_reply (&reply, EFAULT) == false &&
               request.state == USFS_REQ_ABORTED && wake_calls == 1,
           "reply body copy failure aborts and wakes the waiter");
    request.state = USFS_REQ_REPLYING;
    connection.state = USFS_CONN_DEAD;
    tap_ok(&tap,
           finish_receiving_reply (&reply, 0) == false &&
               request.state == USFS_REQ_ABORTED && wake_calls == 2,
           "reply completion aborts after connection loss");
    connection.state = USFS_CONN_ACTIVE;
    connection.delivered_requests_head = NULL;
    user_io.uio_resid = sizeof(reply_input);
    reply_input.version = USFS_PROTOCOL_VERSION;
    reply_input.len = sizeof(reply_input);
    reply_input.opcode = USFS_OP_READ;
    reply_input.id = 123;
    tap_ok(&tap,
           usfs_dev_write(0, &user_io, 6, 0) == EINVAL && abort_calls == 1 &&
               protocol_error_calls == 1 && connection.refs_counter == 1,
           "write aborts the connection for an uncorrelated reply");

    test_read_ownership(&tap);
    test_ownership_replies(&tap);
    {
        ushort ready = 0;
        memset(&connection, 0, sizeof(connection));
        connection.device_number = 42;
        connection.state = USFS_CONN_ACTIVE;
        g_connections[6] = &connection;
        tap_ok(&tap, usfs_dev_select(42, POLLIN | POLLHUP, &ready, 6) == 0 &&
            ready == 0 && connection.select_events == (POLLIN | POLLHUP),
            "idle device registers asynchronous read readiness");
        connection.pending_requests_head = &request;
        tap_ok(&tap, usfs_dev_select(42, POLLIN | POLLSYNC, &ready, 6) == 0 &&
            ready == POLLIN, "queued device request is immediately readable");
        connection.pending_requests_head = NULL;
        connection.state = USFS_CONN_DEAD;
        ready = 0;
        tap_ok(&tap, usfs_dev_select(42, POLLIN | POLLHUP, &ready, 6) == 0 && ready == (POLLIN | POLLHUP) &&
            usfs_dev_select(43, POLLIN, &ready, 6) == ENXIO,
            "terminal channel wakes readers and foreign device identity is rejected");
        connection.state = USFS_CONN_CLOSED; connection.refs_counter = 1;
        ready = 0;
        tap_ok(&tap, usfs_dev_select(42, POLLIN | POLLHUP | POLLERR, &ready, 6) == 0 &&
               ready == (POLLIN | POLLHUP), "orderly closure signals EOF readiness without an error event");
        memset(&user_io, 0, sizeof(user_io)); user_io.uio_resid = 512;
        tap_ok(&tap, usfs_dev_read(42, &user_io, 6, 0) == 0 && user_io.uio_resid == 512 &&
               connection.refs_counter == 1, "orderly device read returns EOF without claiming a request");
        unsigned lost_before = connection_lost_calls, abort_before = abort_calls;
        tap_ok(&tap, claim_reply_request (&reply) == EIO &&
               usfs_dev_write(42, &user_io, 6, 0) == EIO && user_io.uio_resid == 512 &&
               usfs_dev_close(42, 6) == 0 && connection.state == USFS_CONN_CLOSED &&
               connection_lost_calls == lost_before && abort_calls == abort_before,
               "orderly closure rejects replies and descriptor close does not report daemon loss");
    }
    test_reply_cleanup (&tap, false);
    test_reply_cleanup (&tap, true);

    return tap_finish(&tap);
}
