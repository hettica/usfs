/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Native AIX USFS client: connection lifecycle, request dispatch, namespace
 * identity, open handles, directory snapshots and ordered worker execution.
 */
#include "usfs.h"

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mntctl.h>
#include <sys/stat.h>
#include <sys/vmount.h>
#include <unistd.h>

#include "usfs_file.h"
#include "usfs_proto.h"
#include "usfs_validate.h"

#define USFS_DEVICE_PATH "/dev/usfs0"

/* AIX kernel-side open mode bits as delivered in usfs_open_in.flags
 * (FREAD/FWRITE from AIX /usr/include/fcntl.h). */
#define USFS_KERNEL_FREAD  0x1u
#define USFS_KERNEL_FWRITE 0x2u

#define CLIENT_DEFAULT_BLOCK_SIZE     4096
#define CLIENT_NANOSECONDS_PER_SECOND 1000000000L
#define CLIENT_DECIMAL_RADIX          10
#define CLIENT_HEXADECIMAL_RADIX      16
#define CLIENT_HIDDEN_NAME_ATTEMPTS   10
#define CLIENT_HIDDEN_NAME_CAPACITY   64

static int translate_open_flags (const uint32_t flags)
{
    const uint32_t access = flags & (USFS_KERNEL_FREAD | USFS_KERNEL_FWRITE);
    const uint32_t consumed = O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | _FLARGEFILE | _FCLOEXEC | _FNOFOLLOW | _FEXEC | _FKERNEL;
    const uint32_t retained = O_APPEND | O_NONBLOCK | _FNDELAY;

    if (access == 0 && (flags & _FEXEC) == 0)
        return -EINVAL;

    if ((flags & _FEXEC) != 0 && (access & USFS_KERNEL_FWRITE) != 0)
        return -EINVAL;

    if ((flags & O_TRUNC) != 0 && (access & USFS_KERNEL_FWRITE) == 0)
        return -EINVAL;

    if ((flags & ~(USFS_KERNEL_FREAD | USFS_KERNEL_FWRITE | consumed | retained)) != 0)
        return -EOPNOTSUPP;

    const int retained_flags = (int)(flags & retained);

    if (access == (USFS_KERNEL_FREAD | USFS_KERNEL_FWRITE))
        return O_RDWR | retained_flags;

    if ((access & USFS_KERNEL_FWRITE) != 0)
        return O_WRONLY | retained_flags;

    return O_RDONLY | retained_flags;
}

static int normalize_callback_error (const int result)
{
    if (result == 0)
        return 0;

    /* Native callbacks return negative errno. Do not let a buggy
       positive return escape as an invalid negative wire errno. */
    if (result > 0 || result == INT_MIN)
        return EIO;

    return -result;
}

struct client_session
{
    struct usfs_client * client;  // Owner used for ordinary diagnostics.
    volatile sig_atomic_t exited; // Atomic exit request shared with signal handlers.
    int fatal_error;              // First fatal session error.
    int wake_read;                // Owned notification read descriptor.
    int wake_write;               // Owned notification write descriptor.
};

_Static_assert(__atomic_always_lock_free (sizeof (sig_atomic_t), 0), "signal exit state requires lock-free atomics");

static int is_session_exited (const struct client_session * session)
{
    return __atomic_load_n (&session->exited, __ATOMIC_ACQUIRE);
}

static void notify_session (const struct client_session * session)
{
    const int saved = errno;
    const char byte = 1;
    ssize_t result;

    do
    {
        result = write (session->wake_write, &byte, 1);
    }
    while (result < 0 && errno == EINTR);
    /* EAGAIN means a notification is already pending in the pipe. */
    errno = saved;
}

static void open_session_wake_fifo (struct client_session * session, const char * path)
{
    if (mkfifo (path, S_IRUSR | S_IWUSR) != 0)
        return;

    session->wake_read = open (path, O_RDONLY | O_NONBLOCK | _FCLOEXEC);

    if (session->wake_read < 0)
        return;

    session->wake_write = open (path, O_WRONLY | O_NONBLOCK | _FCLOEXEC);
}

static int detach_session_wake_fifo (struct client_session * session, const char * path, const char * directory)
{
    int saved_error = errno;

    if (unlink (path) != 0 && session->wake_write >= 0)
    {
        saved_error = errno;
        close (session->wake_write);
        session->wake_write = INVALID_FILE_DESCRIPTOR;
    }

    if (rmdir (directory) != 0 && session->wake_write >= 0)
    {
        saved_error = errno;
        close (session->wake_write);
        session->wake_write = INVALID_FILE_DESCRIPTOR;
    }

    if (session->wake_write < 0)
    {
        if (session->wake_read >= 0)
            close (session->wake_read);

        session->wake_read = INVALID_FILE_DESCRIPTOR;
        errno = saved_error;

        return -1;
    }

    return 0;
}

static int create_session_wake_pipe (struct client_session * session)
{
    char directory[] = "/tmp/usfs-wake-XXXXXX"; // todo: review path and permissions

    session->wake_write = INVALID_FILE_DESCRIPTOR;
    session->wake_read = INVALID_FILE_DESCRIPTOR;

    if (mkdtemp (directory) == NULL)
        return -1;

    char path[sizeof (directory) + sizeof ("/wake")];

    snprintf (path, sizeof (path), "%s/wake", directory);
    open_session_wake_fifo (session, path);

    return detach_session_wake_fifo (session, path, directory);
}

enum session_poll_slot
{
    SESSION_POLL_WAKE = 0,
    SESSION_POLL_DEVICE = 1,
    SESSION_POLL_SLOT_COUNT = 2
};

#define CLIENT_WAKE_DRAIN_BUFFER_SIZE 128
#define CLIENT_POLL_WAIT_FOREVER      -1

/* The sole reader uses this for both device availability and queue capacity. */
static int wait_for_session_event (const struct client_session * session, const int device_descriptor)
{
    if (is_session_exited (session))
        return 0;

    struct pollfd poll_descriptors[SESSION_POLL_SLOT_COUNT];

    poll_descriptors[SESSION_POLL_WAKE].fd = session->wake_read;
    poll_descriptors[SESSION_POLL_WAKE].events = POLLIN;
    poll_descriptors[SESSION_POLL_WAKE].revents = 0;
    poll_descriptors[SESSION_POLL_DEVICE].fd = device_descriptor;
    poll_descriptors[SESSION_POLL_DEVICE].events = POLLIN;
    poll_descriptors[SESSION_POLL_DEVICE].revents = 0;

    const int rc = poll (poll_descriptors, device_descriptor < 0 ? 1 : SESSION_POLL_SLOT_COUNT, CLIENT_POLL_WAIT_FOREVER);

    if (rc < 0)
        return errno == EINTR ? 0 : -errno;

    if (poll_descriptors[SESSION_POLL_WAKE].revents & POLLIN)
    {
        char pending_notifications[CLIENT_WAKE_DRAIN_BUFFER_SIZE];

        while (read (session->wake_read, pending_notifications, sizeof (pending_notifications)) > 0)
        {
        }
    }

    if (poll_descriptors[SESSION_POLL_WAKE].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -EIO;

    return 0;
}

/* Longest path inside the mounted file system. The path a syscall presents is
   <mountpoint> + this, and that has to fit AIX's PATH_MAX of 1024, so 2048 is
   ample headroom. */
#define USFS_PATH_MAX 2048

/* Components build_node_path can walk. The shortest possible component is "/x", so
   1024 bytes of path cannot hold more than 512 of them. */
#define USFS_MAX_DEPTH 512

/* Indexes cover retained identities; unused records are reclaimed. */
#define CLIENT_NODE_BUCKETS 4096

#define CLIENT_ID_HASH_MULTIPLIER   1000003u
#define CLIENT_NAME_HASH_MULTIPLIER 31u

/* Retained directory data uses at most 16 MiB of RAM. Additional snapshots
   spill to unlinked files, with a separate per-daemon temporary-storage cap. */
#define CLIENT_DIRCACHE_SLOTS         64
#define CLIENT_DIRCACHE_MAX           (4u * 1024 * 1024)
#define CLIENT_DIRCACHE_RAM_MAX       (16u * 1024 * 1024)
#define CLIENT_DIRSPOOL_MAX           (256u * 1024 * 1024)
#define CLIENT_DIRSPOOL_RESERVATION   (1024u * 1024u)
#define CLIENT_DIRECTORY_INDEX_STRIDE 1024u

#define CLIENT_DIRBUF_INITIAL_CAPACITY 4096
#define CLIENT_DIRBUF_GROWTH_FACTOR    2
#define CLIENT_DIRENT_MODE_MASK        0170000u
#define CLIENT_DIRENT_TYPE_SHIFT       12
#define CLIENT_SYNTHETIC_INODE_BASE    0xF0000000u
#define CLIENT_CREATE_PERMISSION_MASK  07777

struct client_alias
{
    struct client_alias * next;  // Next name belonging to the object.
    struct client_alias * hnext; // Next entry in the parent/name hash bucket.
    struct client_node * node;   // Object identified by this name.
    uint64_t parent;             // Parent node identifier.
    char * name;                 // Owned name within the parent directory.
    int attached;                // Whether this alias is present in the indexes.
    int managed_hidden;          // Whether this session owns deferred deletion of this name.
    int cleanup_attempted;       // Whether final-close deletion already ran for this name.
};

struct client_node
{
    pthread_mutex_t mutation_lock;      // Serializes mutations of this object.
    struct client_node * next;          // Next retained object.
    struct client_node * hnext_id;      // Next entry in the node-ID hash bucket.
    struct client_node * hnext_backend; // Next entry in the backend-identity hash bucket.
    uint64_t id;                        // Stable wire node identifier.
    uint64_t lookup_refs;               // Kernel lookup references.
    uint64_t open_refs;                 // Open handle references.
    uint64_t operation_refs;            // In-flight request references.
    uint64_t ancestor_refs;             // References from named descendants.
    uint64_t cache_refs;                // Directory snapshot references.
    struct client_node * gc_next;       // Next pending garbage-collection candidate.
    int gc_queued;                      // Whether collection is already queued.
    struct client_alias * aliases;      // Names belonging to this object.
    uint64_t backend_dev;               // Backend device identity.
    uint64_t backend_ino;               // Backend inode identity.
    uint64_t backend_links;             // Last observed backend hard-link count.
    mode_t backend_type;                // Backend file type bits.
    unsigned names;                     // Number of attached names.
    unsigned hidden_names;              // Attached aliases owned for deferred deletion.
    int backend_indexed;                // Whether backend identity is indexed.
    int backend_retired;                // Whether the backend identity was retired.
    int detached;                       // Whether retained identity has no resolving name.
};

struct client_handle
{
    struct client_handle * next; // Next open handle.
    uint64_t nodeid;             // Wire node identifier.
    uint64_t fh;                 // Backend callback handle.
    uint64_t wire_fh;            // Nonzero opaque handle sent to the kernel, including for backend fh zero.
    uint64_t generation;         // Unique handle lifetime identifier.
    int flags;                   // Open flags supplied to callbacks.
    int isdir;                   // Whether this is a directory handle.
};

struct client_dircache_key
{
    uint64_t nodeid;     // Directory node identifier.
    uint64_t fh;         // Backend directory handle.
    uint64_t generation; // Handle lifetime used for identity checks.
    uint32_t uid;        // Request user identity.
    uint32_t gid;        // Request group identity.
    uint32_t pid;        // Request process identity.
};

struct client_dircache
{
    struct client_dircache_key key; // Snapshot identity and credentials.
    uint64_t seq;                   // Least-recently-used sequence number.
    uint64_t snapshot_id;           // Opaque cursor identity for this listing.
    char * data;                    // Owned serialized directory entries.
    size_t data_capacity;           // Retained RAM charged for this snapshot.
    size_t len;                     // Number of serialized bytes.
    uint32_t count;                 // Number of serialized entries.
    int spool_fd;                   // Unlinked temporary file when spooled.
    size_t * checkpoints;           // Byte offset of every 1024th entry.
    size_t checkpoint_count;        // Number of populated checkpoint offsets.
    size_t spool_reserved;          // Bytes charged to the daemon's spool quota.
    bool spooled;                   // Whether spool_fd owns a temporary file.
    bool owns_node_ref;             // Whether this snapshot retains its node.
    bool completed;                 // Entry storage was released after EOF.
};

/* Admission counts are protected by lock; the lock itself is never held
 * across a callback. A queued writer prevents newly arriving readers from
 * overtaking it. Dispatch is nonrecursive: callbacks must not synchronously
 * access this mount through its kernel interface. */
struct client_namespace
{
    pthread_mutex_t lock;     // Protects admission counters.
    pthread_cond_t changed;   // Notifies waiters when admission changes.
    unsigned readers;         // Number of admitted readers.
    unsigned waiting_writers; // Writers waiting for exclusive admission.
    int writer;               // Whether a writer is admitted.
};

struct usfs_client
{
    int fd;                                                    // Owned connection device descriptor.
    struct usfs_dev_info info;                                 // Kernel connection identity and filesystem type.
    struct usfs_operations ops;                                // Filesystem callback table.
    void * user_data;                                          // Filesystem-owned callback state.
    char * mountpoint;                                         // Owned canonical mount pathname.
    int mounted;                                               // Whether this connection still owns a mount.
    int mount_mode;                                            // Requested usfs_mount_access; automatic derives access from callbacks.
    int init_done;                                             // Whether initialization callback has run.
    struct client_session session;                             // Exit and notification state.
    struct usfs_limits limits;                                 // Maximum backend I/O sizes.
    usfs_diagnostic_fn diagnostic;                             // Optional message callback.
    void * diagnostic_data;                                    // Borrowed message callback data.
    struct usfs_behavior behavior;                             // Native handle/path and removal policies.
    struct client_node * nodes;                                // Retained object list.
    struct client_node * by_id[CLIENT_NODE_BUCKETS];           // Objects indexed by wire node ID.
    struct client_alias * by_parent_name[CLIENT_NODE_BUCKETS]; // Aliases indexed by parent and name.
    struct client_node * by_backend[CLIENT_NODE_BUCKETS];      // Objects indexed by backend identity.
    uint64_t next_id;                                          // Next available wire node identifier.
    uint64_t root_lookup_refs;                                 // Lookup references held on the implicit root.
    struct client_node * gc_pending;                           // Objects pending collection.
    struct client_handle * handles;                            // Open callback handles.
    uint64_t handle_generation;                                // Next handle lifetime sequence number.
    uint64_t hidden_generation;                                // Next candidate hidden-name sequence number.
    struct client_dircache dircache[CLIENT_DIRCACHE_SLOTS];    // Owned directory snapshot slots.
    uint64_t dircache_seq;                                     // Next snapshot recency sequence number.
    size_t dircache_ram_bytes;                                 // Retained directory data allocation.
    uint64_t next_snapshot_id;                                 // Next positive READDIR cursor identity.
    size_t dircache_spool_bytes;                               // Reserved temporary storage across snapshots.
    pthread_mutex_t node_lock;                                 // Protects object identities and references.
    pthread_mutex_t dircache_lock;                             // Protects directory snapshots.
    pthread_mutex_t reply_lock;                                // Serializes complete wire replies.
    struct client_namespace namespace;                         // Request namespace admission state.
    char * readbuf;                                            // Owned request buffer of USFS_MSG_MAX bytes.
    char * writebuf;                                           // Owned fallback reply buffer of USFS_MSG_MAX bytes.
};

struct usfs_client_request
{
    struct usfs_client * client; // Client invoking this callback.
    void * user_data;            // Unchanged application pointer.
    uid_t uid;                   // Caller identity, or zero during lifecycle calls.
    gid_t gid;
    pid_t pid;
};

struct request_context
{
    struct usfs_client * client;       // Session serving this request.
    const struct usfs_in_hdr * header; // Borrowed validated request header.
};

struct client_thread_context
{
    struct usfs_client_request context; // Current callback credentials and private state.
    char * writebuf;                    // Borrowed worker reply buffer, or NULL.
    int active;                         // Whether callback context is active.
};

static pthread_key_t client_context_key;
static pthread_mutex_t client_context_key_lock = PTHREAD_MUTEX_INITIALIZER;
static int client_context_key_ready;
static pthread_mutex_t signal_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static struct client_session * signal_session;
static unsigned signal_handlers_active;
static struct sigaction previous_signal_actions[4];
static struct sigaction installed_signal_actions[4];
_Static_assert(__atomic_always_lock_free (sizeof (signal_session), 0), "signal session publication must be lock free");
_Static_assert(__atomic_always_lock_free (sizeof (signal_handlers_active), 0), "signal handler lifetime counter must be lock free");

static void emit_diagnostic_arguments (const struct usfs_client * client, const char * format, va_list arguments)
{
    if (client == NULL || client->diagnostic == NULL)
    {
        vfprintf (stderr, format, arguments);
        return;
    }

    va_list measurement;
    va_copy (measurement, arguments);
    const int length = vsnprintf (NULL, 0, format, measurement);
    va_end (measurement);
    if (length < 0)
    {
        client->diagnostic (client->diagnostic_data, "Failed to format USFS diagnostic\n");
        return;
    }

    const size_t capacity = (size_t)length + 1u;
    char * message = malloc (capacity);
    if (message == NULL)
    {
        client->diagnostic (client->diagnostic_data, "Failed to allocate USFS diagnostic\n");
        return;
    }

    vsnprintf (message, capacity, format, arguments);
    client->diagnostic (client->diagnostic_data, message);
    free (message);
}

static void emit_diagnostic (const struct usfs_client * client, const char * format, ...)
{
    const int saved_error = errno;
    va_list arguments;

    va_start (arguments, format);
    emit_diagnostic_arguments (client, format, arguments);
    va_end (arguments);
    errno = saved_error;
}

static int current_system_error (void)
{
    return errno != 0 ? errno : EIO;
}

/* Deterministic unit barrier, compiled away in ordinary clients. */
#ifndef USFS_NAMESPACE_WAITING
    #define USFS_NAMESPACE_WAITING(exclusive) ((void)0)
#endif

static int namespace_access_must_wait (const struct client_namespace * admission_state, const int exclusive)
{
    if (admission_state->writer)
        return true;

    if (exclusive)
        return admission_state->readers != 0;

    return admission_state->waiting_writers != 0;
}

static int wait_for_namespace_access_locked (struct client_namespace * admission_state, const int exclusive)
{
    int rc = 0;

    if (exclusive)
        admission_state->waiting_writers++;

    while (namespace_access_must_wait (admission_state, exclusive))
    {
        USFS_NAMESPACE_WAITING (exclusive);
        rc = pthread_cond_wait (&admission_state->changed, &admission_state->lock);

        if (rc != 0)
            break;
    }

    if (exclusive)
        admission_state->waiting_writers--;

    return rc;
}

static int acquire_namespace_access (struct usfs_client * client, const int exclusive)
{
    struct client_namespace * admission_state = &client->namespace;
    int rc = pthread_mutex_lock (&admission_state->lock);

    if (rc != 0)
        return rc;

    rc = wait_for_namespace_access_locked (admission_state, exclusive);

    if (rc != 0)
    {
        pthread_cond_broadcast (&admission_state->changed);
    }
    else if (exclusive)
    {
        admission_state->writer = 1;
    }
    else
    {
        admission_state->readers++;
    }

    pthread_mutex_unlock (&admission_state->lock);

    return rc;
}

static void release_namespace_access (struct usfs_client * client, const int exclusive)
{
    struct client_namespace * admission_state = &client->namespace;

    pthread_mutex_lock (&admission_state->lock);

    if (exclusive)
        admission_state->writer = 0;
    else
        admission_state->readers--;

    pthread_cond_broadcast (&admission_state->changed);
    pthread_mutex_unlock (&admission_state->lock);
}

static int request_mutates_namespace (const uint16_t opcode)
{
    switch (opcode)
    {
        case USFS_OP_CREATE:
        case USFS_OP_CREATE_ATTR:
        case USFS_OP_FORGET:
        case USFS_OP_MKDIR:
        case USFS_OP_UNLINK:
        case USFS_OP_RMDIR:
        case USFS_OP_RENAME:
        case USFS_OP_SYMLINK:
        case USFS_OP_LINK:
            return true;
        default:
            return false;
    }
}

static int ensure_thread_context_key (void)
{
    if (__atomic_load_n (&client_context_key_ready, __ATOMIC_ACQUIRE))
        return 0;

    int error = pthread_mutex_lock (&client_context_key_lock);

    if (error != 0)
        return error;

    if (!__atomic_load_n (&client_context_key_ready, __ATOMIC_RELAXED))
    {
        error = pthread_key_create (&client_context_key, free);

        if (error == 0)
            __atomic_store_n (&client_context_key_ready, 1, __ATOMIC_RELEASE);
    }

    pthread_mutex_unlock (&client_context_key_lock);

    return error;
}

static struct client_thread_context * get_thread_context (void)
{
    if (ensure_thread_context_key () != 0)
        return NULL;

    return pthread_getspecific (client_context_key);
}

static int acquire_thread_context (struct client_thread_context ** out)
{
    int error = ensure_thread_context_key ();
    if (error != 0)
        return error;

    *out = pthread_getspecific (client_context_key);
    if (*out != NULL)
        return 0;

    struct client_thread_context * thread = calloc (1, sizeof (*thread));
    if (thread == NULL)
        return ENOMEM;

    error = pthread_setspecific (client_context_key, thread);
    if (error != 0)
    {
        free (thread);

        return error;
    }

    *out = thread;

    return 0;
}

struct client_context_scope
{
    struct client_thread_context * thread; // Thread context restored when the scope ends.
    struct usfs_client_request saved;      // Saved outer callback context.
    int active;                            // Saved outer active state.
};

static void restore_callback_context (const struct client_context_scope * scope)
{
    if (scope->thread == NULL)
        return;

    scope->thread->context = scope->saved;
    scope->thread->active = scope->active;
}

static int enter_callback_context (struct usfs_client * client, struct client_context_scope * scope)
{
    struct client_thread_context * thread;
    const int error = acquire_thread_context (&thread);

    if (error != 0)
        return error;

    scope->thread = thread;
    scope->saved = thread->context;
    scope->active = thread->active;
    memset (&thread->context, 0, sizeof (thread->context));
    thread->context.client = client;
    thread->context.user_data = client->user_data;
    thread->active = 1;

    return 0;
}

/* ----------------------------------------------------------- *
 * Small public helpers                                        *
 * ----------------------------------------------------------- */

static struct usfs_client_request * get_callback_request (void)
{
    struct client_thread_context * thread = get_thread_context ();

    if (thread == NULL)
        return NULL;

    if (!thread->active)
        return NULL;

    return &thread->context;
}

void usfs_client_request_stop (struct usfs_client * client)
{
    if (client == NULL)
        return;

    __atomic_store_n (&client->session.exited, 1, __ATOMIC_RELEASE);
    notify_session (&client->session);
}

/* ----------------------------------------------------------- *
 * Signal handling                                             *
 * ----------------------------------------------------------- */

static void handle_exit_signal (const int signal_number)
{
    const int saved = errno;

    (void)signal_number;

    __atomic_add_fetch (&signal_handlers_active, 1, __ATOMIC_SEQ_CST);
    struct client_session * session = __atomic_load_n (&signal_session, __ATOMIC_SEQ_CST);

    if (session != NULL)
    {
        __atomic_store_n (&session->exited, 1, __ATOMIC_RELEASE);
        notify_session (session);
    }

    __atomic_sub_fetch (&signal_handlers_active, 1, __ATOMIC_SEQ_CST);
    errno = saved;
}

static void wait_for_signal_handlers (void)
{
    while (__atomic_load_n (&signal_handlers_active, __ATOMIC_SEQ_CST) != 0)
        sched_yield ();
}

static void restore_signal_actions (const struct usfs_client * client, const size_t installed_count)
{
    static const int signal_numbers[] = { SIGHUP, SIGINT, SIGTERM, SIGPIPE };

    for (size_t signal_index = installed_count; signal_index != 0; signal_index--)
    {
        const size_t action_index = signal_index - 1u;
        struct sigaction current_action = { 0 };
        if (sigaction (signal_numbers[action_index], NULL, &current_action) != 0)
        {
            emit_diagnostic (client, "Failed to inspect signal disposition %d: %s\n", signal_numbers[action_index], strerror (errno));
            continue;
        }

        const struct sigaction * installed_action = &installed_signal_actions[action_index];
        if (current_action.sa_handler != installed_action->sa_handler || current_action.sa_flags != installed_action->sa_flags ||
            memcmp (&current_action.sa_mask, &installed_action->sa_mask, sizeof (current_action.sa_mask)) != 0)
            continue;

        if (sigaction (signal_numbers[action_index], &previous_signal_actions[action_index], NULL) != 0)
            emit_diagnostic (client, "Failed to restore signal disposition %d: %s\n", signal_numbers[action_index], strerror (errno));
    }
}

static int install_signal_actions (const struct usfs_client * client)
{
    static const int signal_numbers[] = { SIGHUP, SIGINT, SIGTERM, SIGPIPE };
    struct sigaction signal_action = { 0 };

    signal_action.sa_handler = handle_exit_signal;
    sigemptyset (&signal_action.sa_mask);

    for (size_t signal_index = 0; signal_index < sizeof (signal_numbers) / sizeof (signal_numbers[0]); signal_index++)
    {
        if (signal_numbers[signal_index] == SIGPIPE)
            signal_action.sa_handler = SIG_IGN;

        if (sigaction (signal_numbers[signal_index], &signal_action, &previous_signal_actions[signal_index]) == 0)
        {
            if (sigaction (signal_numbers[signal_index], NULL, &installed_signal_actions[signal_index]) == 0)
                continue;

            const int inspect_error = errno;
            installed_signal_actions[signal_index] = signal_action;
            restore_signal_actions (client, signal_index + 1u);
            errno = inspect_error;
            return -1;
        }

        const int install_error = errno;
        restore_signal_actions (client, signal_index);
        errno = install_error;
        return -1;
    }

    return 0;
}

static int install_signal_handlers (struct client_session * session)
{
    if (session == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    const int lock_error = pthread_mutex_lock (&signal_owner_lock);
    if (lock_error != 0)
    {
        errno = lock_error;
        return -1;
    }

    struct client_session * owner = __atomic_load_n (&signal_session, __ATOMIC_SEQ_CST);
    if (owner == session)
    {
        pthread_mutex_unlock (&signal_owner_lock);
        return 0;
    }

    if (owner != NULL)
    {
        pthread_mutex_unlock (&signal_owner_lock);
        errno = EBUSY;
        return -1;
    }

    __atomic_store_n (&signal_session, session, __ATOMIC_SEQ_CST);

    if (install_signal_actions (session->client) != 0)
    {
        const int install_error = errno;

        __atomic_store_n (&signal_session, NULL, __ATOMIC_SEQ_CST);
        wait_for_signal_handlers ();
        pthread_mutex_unlock (&signal_owner_lock);
        errno = install_error;

        return -1;
    }

    pthread_mutex_unlock (&signal_owner_lock);

    return 0;
}

static void remove_signal_handlers (const struct client_session * session)
{
    if (session == NULL)
        return;

    const int lock_error = pthread_mutex_lock (&signal_owner_lock);
    if (lock_error != 0)
    {
        emit_diagnostic (session->client, "Failed to lock signal handler ownership: %s\n", strerror (lock_error));
        return;
    }

    if (__atomic_load_n (&signal_session, __ATOMIC_SEQ_CST) == session)
    {
        restore_signal_actions (session->client, sizeof (previous_signal_actions) / sizeof (previous_signal_actions[0]));
        __atomic_store_n (&signal_session, NULL, __ATOMIC_SEQ_CST);
        wait_for_signal_handlers ();
    }

    pthread_mutex_unlock (&signal_owner_lock);
}

/* ----------------------------------------------------------- *
 * Node table: nodeid -> (parent, name) -> path                *
 * ----------------------------------------------------------- */

static size_t hash_node_id (const uint64_t id)
{
    return id % CLIENT_NODE_BUCKETS;
}

static size_t hash_parent_name (const uint64_t parent, const char * name)
{
    uint64_t hash = parent * CLIENT_ID_HASH_MULTIPLIER;

    while (*name != '\0')
        hash = (hash * CLIENT_NAME_HASH_MULTIPLIER) + (unsigned char)*name++;

    return hash % CLIENT_NODE_BUCKETS;
}

static struct client_node * find_node_by_id (const struct usfs_client * client, const uint64_t id)
{
    for (struct client_node * node = client->by_id[hash_node_id (id)]; node != NULL; node = node->hnext_id)
        if (node->id == id)
            return node;

    return NULL;
}

static int is_backend_stat_valid (const struct stat * metadata);

static void queue_node_for_collection (struct usfs_client * client, struct client_node * node)
{
    if (node == NULL)
        return;

    if (node->gc_queued)
        return;

    node->gc_queued = true;
    node->gc_next = client->gc_pending;
    client->gc_pending = node;
}

static struct client_alias * find_alias (const struct usfs_client * client, const uint64_t parent, const char * name)
{
    for (struct client_alias * alias = client->by_parent_name[hash_parent_name (parent, name)]; alias != NULL; alias = alias->hnext)
        if (alias->parent == parent && strcmp (alias->name, name) == 0)
            return alias;

    return NULL;
}

static struct client_node * find_child_node (const struct usfs_client * client, const uint64_t parent, const char * name)
{
    const struct client_alias * alias = find_alias (client, parent, name);

    return alias != NULL ? alias->node : NULL;
}

static struct client_alias * prepare_alias (const uint64_t parent, const char * name)
{
    struct client_alias * alias = calloc (1, sizeof (*alias));

    if (alias == NULL)
        return NULL;

    alias->name = strdup (name);

    if (alias->name == NULL)
    {
        free (alias);

        return NULL;
    }

    alias->parent = parent;

    return alias;
}

static struct client_node * prepare_node (const uint64_t parent, const char * name)
{
    struct client_node * node = calloc (1, sizeof (*node));

    if (node == NULL)
        return NULL;

    const int rc = pthread_mutex_init (&node->mutation_lock, NULL);

    if (rc != 0)
    {
        free (node);
        errno = rc;

        return NULL;
    }

    node->aliases = prepare_alias (parent, name);

    if (node->aliases == NULL)
    {
        pthread_mutex_destroy (&node->mutation_lock);
        free (node);

        return NULL;
    }

    return node;
}

static void destroy_node (struct client_node * node)
{
    if (node == NULL)
        return;

    pthread_mutex_destroy (&node->mutation_lock);

    while (node->aliases != NULL)
    {
        struct client_alias * alias = node->aliases;

        node->aliases = alias->next;
        free (alias->name);
        free (alias);
    }

    free (node);
}

static void index_alias (struct usfs_client * client, struct client_alias * alias)
{
    struct client_node * parent = find_node_by_id (client, alias->parent);
    const size_t bucket = hash_parent_name (alias->parent, alias->name);

    alias->hnext = client->by_parent_name[bucket];
    client->by_parent_name[bucket] = alias;
    alias->attached = 1;
    alias->node->names++;
    if (alias->managed_hidden)
        alias->node->hidden_names++;

    alias->node->detached = alias->node->names == alias->node->hidden_names;

    if (parent != NULL)
        parent->ancestor_refs++;
}

static void attach_alias (struct usfs_client * client, struct client_node * node, struct client_alias * alias)
{
    alias->node = node;
    alias->next = node->aliases;
    node->aliases = alias;
    index_alias (client, alias);
}

static void unindex_alias (struct usfs_client * client, struct client_alias * alias)
{
    if (!alias->attached)
        return;

    struct client_alias ** cursor = &client->by_parent_name[hash_parent_name (alias->parent, alias->name)];

    while (*cursor != NULL && *cursor != alias)
        cursor = &(*cursor)->hnext;

    if (*cursor == NULL)
        return;

    *cursor = alias->hnext;
    alias->hnext = NULL;
    alias->attached = 0;
    alias->node->names--;
    if (alias->managed_hidden)
        alias->node->hidden_names--;

    alias->node->detached = alias->node->names == alias->node->hidden_names;

    struct client_node * parent = find_node_by_id (client, alias->parent);

    if (parent != NULL)
    {
        parent->ancestor_refs--;
        queue_node_for_collection (client, parent);
    }
}

static size_t hash_backend_identity (const uint64_t device, const uint64_t inode)
{
    return (size_t)((device * CLIENT_ID_HASH_MULTIPLIER + inode) % CLIENT_NODE_BUCKETS);
}

static void unindex_node_backend_identity (struct usfs_client * client, struct client_node * node)
{
    if (!node->backend_indexed)
        return;

    struct client_node ** cursor = &client->by_backend[hash_backend_identity (node->backend_dev, node->backend_ino)];

    while (*cursor != NULL && *cursor != node)
        cursor = &(*cursor)->hnext_backend;

    if (*cursor == node)
        *cursor = node->hnext_backend;

    node->hnext_backend = NULL;
    node->backend_indexed = false;
}

static void retire_node_backend_identity (struct usfs_client * client, struct client_node * node)
{
    unindex_node_backend_identity (client, node);
    node->backend_retired = true;
}

static struct client_node * find_node_by_backend_identity (const struct usfs_client * client, const struct stat * metadata)
{
    if (metadata == NULL)
        return NULL;

    if (metadata->st_ino == 0)
        return NULL;

    if (S_ISDIR (metadata->st_mode))
        return NULL;

    const size_t bucket = hash_backend_identity ((uint64_t)metadata->st_dev, (uint64_t)metadata->st_ino);

    for (struct client_node * node = client->by_backend[bucket]; node != NULL; node = node->hnext_backend)
    {
        if (node->backend_dev != (uint64_t)metadata->st_dev)
            continue;

        if (node->backend_ino == (uint64_t)metadata->st_ino)
            return node;
    }

    return NULL;
}

static int node_backend_identity_matches (const struct client_node * node, const struct stat * metadata)
{
    const mode_t type = metadata->st_mode & S_IFMT;

    if (node->backend_type != 0 && node->backend_type != type)
        return false;

    if (node->backend_ino != 0)
    {
        if (node->backend_dev != (uint64_t)metadata->st_dev)
            return false;

        if (node->backend_ino != (uint64_t)metadata->st_ino)
            return false;
    }

    if (node->backend_retired && metadata->st_nlink != 0)
        return false;

    return true;
}

static void index_node_backend_identity (struct usfs_client * client, struct client_node * node)
{
    if (S_ISDIR (node->backend_type))
        return;

    if (node->backend_ino == 0)
        return;

    if (node->backend_indexed)
        return;

    const size_t bucket = hash_backend_identity (node->backend_dev, node->backend_ino);

    node->hnext_backend = client->by_backend[bucket];
    client->by_backend[bucket] = node;
    node->backend_indexed = true;
}

static int update_node_backend_identity (struct usfs_client * client, struct client_node * node, const struct stat * metadata)
{
    if (metadata == NULL)
        return 0;

    if (!is_backend_stat_valid (metadata))
        return EIO;

    if (!node_backend_identity_matches (node, metadata))
        return EIO;

    const struct client_node * other = find_node_by_backend_identity (client, metadata);

    if (other != NULL && other != node)
        return EIO;

    node->backend_dev = (uint64_t)metadata->st_dev;
    node->backend_ino = (uint64_t)metadata->st_ino;
    node->backend_type = metadata->st_mode & S_IFMT;
    node->backend_links = (uint64_t)metadata->st_nlink;

    if (metadata->st_nlink == 0)
    {
        retire_node_backend_identity (client, node);

        return 0;
    }

    index_node_backend_identity (client, node);

    return 0;
}

static void remove_alias (struct usfs_client * client, struct client_alias * alias)
{
    struct client_node * node = alias->node;
    struct client_alias ** cursor = &node->aliases;

    unindex_alias (client, alias);

    if (node->backend_links != 0)
        node->backend_links--;

    if (node->backend_links == 0)
        retire_node_backend_identity (client, node);

    while (*cursor != NULL && *cursor != alias)
        cursor = &(*cursor)->next;

    if (*cursor == alias)
        *cursor = alias->next;

    free (alias->name);
    free (alias);
    queue_node_for_collection (client, node);
}

static int node_has_references (const struct client_node * node)
{
    if (node->hidden_names != 0)
        return true;

    if (node->lookup_refs != 0)
        return true;

    if (node->open_refs != 0)
        return true;

    if (node->operation_refs != 0)
        return true;

    if (node->ancestor_refs != 0)
        return true;

    return node->cache_refs != 0;
}

static void detach_node_aliases (struct usfs_client * client, struct client_node * node)
{
    while (node->aliases != NULL)
    {
        struct client_alias * alias = node->aliases;

        unindex_alias (client, alias);
        node->aliases = alias->next;
        free (alias->name);
        free (alias);
    }
}

static void unindex_node (struct usfs_client * client, const struct client_node * node)
{
    struct client_node ** cursor = &client->by_id[hash_node_id (node->id)];

    while (*cursor != node)
        cursor = &(*cursor)->hnext_id;

    *cursor = node->hnext_id;
    cursor = &client->nodes;

    while (*cursor != node)
        cursor = &(*cursor)->next;

    *cursor = node->next;
}

/* All reference changes and reclamation are serialized by node_lock. Each
 * attached alias retains its parent. Removing the last dependent can enqueue
 * another ancestor, so the work list releases the chain without recursion. */
static void collect_unreferenced_nodes (struct usfs_client * client)
{
    while (client->gc_pending != NULL)
    {
        struct client_node * node = client->gc_pending;

        client->gc_pending = node->gc_next;
        node->gc_queued = false;

        if (node_has_references (node))
            continue;

        detach_node_aliases (client, node);
        retire_node_backend_identity (client, node);
        unindex_node (client, node);
        destroy_node (node);
    }
}

static void invalidate_handle_directory_cache_locked (struct usfs_client *, uint64_t, uint64_t);

struct open_handle_state
{
    struct usfs_open_file file_info; // Flags and backend handle shared across open and publication.
    uint64_t nodeid;                 // Wire node identifier to retain at publication.
    uint64_t wire_fh;                // Published kernel handle distinct from the backend callback value.
    uint32_t is_directory;           // Whether directory callbacks and cache invalidation apply.
};

/* node_lock protects the handle table. Duplicate nonzero backend handles retain
 * their existing representation; zero and conflicting synthetic values get a
 * nonzero token so metadata requests can distinguish handles from pathnames. */
static uint64_t select_wire_handle (const struct usfs_client * client, const uint64_t backend_handle)
{
    uint64_t candidate = backend_handle != 0 ? backend_handle : UINT64_MAX - client->handle_generation;

    for (;;)
    {
        int occupied = candidate == 0;

        for (const struct client_handle * handle = client->handles; handle != NULL; handle = handle->next)
            if (handle->wire_fh == candidate && (backend_handle == 0 || handle->fh != backend_handle))
                occupied = true;

        if (!occupied)
            return candidate;

        candidate--;
    }
}

static uint64_t backend_handle_value (struct usfs_client * client, const uint64_t nodeid, const uint64_t wire_handle)
{
    uint64_t backend_handle = wire_handle;

    pthread_mutex_lock (&client->node_lock);
    for (const struct client_handle * handle = client->handles; handle != NULL; handle = handle->next)
    {
        if (handle->nodeid != nodeid || handle->wire_fh != wire_handle)
            continue;

        backend_handle = handle->fh;
        break;
    }
    pthread_mutex_unlock (&client->node_lock);

    return backend_handle;
}

static void publish_client_handle (struct usfs_client * client, struct client_handle * handle, struct open_handle_state * opened)
{
    pthread_mutex_lock (&client->dircache_lock);

    pthread_mutex_lock (&client->node_lock);
    struct client_node * node = find_node_by_id (client, opened->nodeid);

    handle->nodeid = opened->nodeid;
    handle->fh = opened->file_info.value;
    handle->flags = opened->file_info.open_flags;
    handle->isdir = opened->is_directory != 0;

    handle->generation = client->handle_generation == UINT64_MAX ? 0 : ++client->handle_generation;
    handle->wire_fh = select_wire_handle (client, handle->fh);
    opened->wire_fh = handle->wire_fh;
    handle->next = client->handles;
    client->handles = handle;

    if (node != NULL)
        node->open_refs++;

    pthread_mutex_unlock (&client->node_lock);

    if (opened->is_directory)
        invalidate_handle_directory_cache_locked (client, opened->nodeid, opened->wire_fh);

    pthread_mutex_unlock (&client->dircache_lock);
}

static void relink_alias_with_prepared_name (struct usfs_client * client, struct client_alias * alias, const uint64_t parent, char * name)
{
    unindex_alias (client, alias);
    free (alias->name);
    alias->name = name;
    alias->parent = parent;
    index_alias (client, alias);
}

static struct client_node * select_node_for_adoption (
    const struct usfs_client * client,
    struct client_node * prepared,
    const struct stat * metadata,
    int * existing_name
)
{
    const struct client_alias * alias = prepared->aliases;
    struct client_node * node = find_child_node (client, alias->parent, alias->name);

    *existing_name = node != NULL;

    if (metadata != NULL && metadata->st_nlink == 0)
        return NULL;

    if (node == NULL)
        node = find_node_by_backend_identity (client, metadata);

    return node != NULL ? node : prepared;
}

static int prepare_node_adoption (
    struct usfs_client * client,
    struct client_node * node,
    const struct client_node * prepared,
    const struct stat * metadata
)
{
    if (node->lookup_refs == UINT64_MAX)
        return EOVERFLOW;

    if (node == prepared && client->next_id == UINT64_MAX)
        return EOVERFLOW;

    return update_node_backend_identity (client, node, metadata);
}

static void publish_node_adoption (struct usfs_client * client, struct client_node * node, struct client_node ** prepared, const int existing_name)
{
    struct client_alias * alias = (*prepared)->aliases;

    if (node == *prepared)
    {
        node->id = client->next_id++;
        node->next = client->nodes;
        client->nodes = node;

        const size_t bucket = hash_node_id (node->id);

        node->hnext_id = client->by_id[bucket];
        client->by_id[bucket] = node;
        alias->node = node;
        index_alias (client, alias);
        *prepared = NULL;
    }
    else if (!existing_name)
    {
        (*prepared)->aliases = NULL;
        attach_alias (client, node, alias);
    }

    node->lookup_refs++;
}

static struct client_node * adopt_prepared_node (
    struct usfs_client * client,
    struct client_node ** prepared,
    const struct stat * metadata,
    int * error
)
{
    int existing_name;
    struct client_node * node = select_node_for_adoption (client, *prepared, metadata, &existing_name);

    if (node == NULL)
    {
        *error = EIO;

        return NULL;
    }

    *error = prepare_node_adoption (client, node, *prepared, metadata);

    if (*error != 0)
        return NULL;

    publish_node_adoption (client, node, prepared, existing_name);

    return node;
}

static struct client_node * find_or_create_node (struct usfs_client * client, const uint64_t parent, const char * name, const struct stat * metadata)
{
    struct client_node * prepared = prepare_node (parent, name);
    int error;

    if (prepared == NULL)
    {
        errno = ENOMEM;

        return NULL;
    }

    pthread_mutex_lock (&client->node_lock);
    struct client_node * node = adopt_prepared_node (client, &prepared, metadata, &error);

    pthread_mutex_unlock (&client->node_lock);

    destroy_node (prepared);

    if (node == NULL)
        errno = error;

    return node;
}

static struct client_alias * select_node_path_alias (const struct client_node * node)
{
    for (struct client_alias * alias = node->aliases; alias != NULL; alias = alias->next)
        if (alias->attached && !alias->managed_hidden)
            return alias;

    for (struct client_alias * alias = node->aliases; alias != NULL; alias = alias->next)
        if (alias->attached)
            return alias;

    return node->aliases; /* informational former path for retained handles */
}

static int collect_node_path_components (const struct usfs_client * client, uint64_t nodeid, const char * names[USFS_MAX_DEPTH], int * depth)
{
    *depth = 0;

    while (nodeid != USFS_ROOT_ID)
    {
        const struct client_node * node = find_node_by_id (client, nodeid);

        if (node == NULL)
            return -ENOENT;

        if (*depth >= USFS_MAX_DEPTH)
            return -ENOENT;

        const struct client_alias * alias = select_node_path_alias (node);

        if (alias == NULL)
            return -ENOENT;

        names[(*depth)++] = alias->name;
        nodeid = alias->parent;
    }

    return 0;
}

static int join_node_path_components (const char * const names[USFS_MAX_DEPTH], const int depth, char * path_buffer, const size_t buffer_size)
{
    if (depth == 0)
    {
        if (buffer_size < sizeof ("/"))
            return -ENAMETOOLONG;

        strcpy (path_buffer, "/");

        return 0;
    }

    size_t used = 0;

    path_buffer[0] = '\0';

    for (int component_index = depth - 1; component_index >= 0; component_index--)
    {
        const size_t name_length = strlen (names[component_index]);

        if (used + 1 + name_length + 1 > buffer_size)
            return -ENAMETOOLONG;

        path_buffer[used] = '/';
        memcpy (path_buffer + used + 1, names[component_index], name_length + 1);
        used += 1 + name_length;
    }

    return 0;
}

static int build_node_path_locked (const struct usfs_client * client, const uint64_t nodeid, char * path_buffer, const size_t buffer_size)
{
    const char * names[USFS_MAX_DEPTH];
    int depth;
    const int rc = collect_node_path_components (client, nodeid, names, &depth);

    if (rc != 0)
        return rc;

    return join_node_path_components (names, depth, path_buffer, buffer_size);
}

static int build_node_path (struct usfs_client * client, const uint64_t id, char * path_buffer, const size_t buffer_size)
{
    pthread_mutex_lock (&client->node_lock);
    const int result = build_node_path_locked (client, id, path_buffer, buffer_size);

    pthread_mutex_unlock (&client->node_lock);

    return result;
}

struct resolved_handle_path
{
    char buffer[USFS_PATH_MAX]; // Storage for the resolved node path.
    const char * path;          // Points into buffer, or NULL for a detached node.
};

static int resolve_node_handle_path (struct usfs_client * client, const uint64_t id, const uint64_t fh, struct resolved_handle_path * resolved)
{
    int result;

    pthread_mutex_lock (&client->node_lock);
    const struct client_node * node = find_node_by_id (client, id);

    resolved->path = resolved->buffer;

    if (node != NULL && node->detached && node->hidden_names == 0)
    {
        resolved->path = NULL;
        result = fh != 0 ? 0 : -ESTALE;
        for (const struct client_handle * handle = client->handles; handle != NULL; handle = handle->next)
            if (handle->nodeid == id && handle->wire_fh == fh)
                result = 0;
    }
    else
    {
        result = build_node_path_locked (client, id, resolved->buffer, sizeof (resolved->buffer));
    }

    pthread_mutex_unlock (&client->node_lock);

    return result;
}

static void omit_handle_callback_path (const struct usfs_client * client, const int has_file_info, struct resolved_handle_path * resolved)
{
    if (has_file_info && (client->behavior.handle_paths == USFS_PATH_OMIT_FOR_HANDLE))
        resolved->path = NULL;
}

static uint64_t get_node_parent_id_locked (const struct usfs_client * client, const uint64_t nodeid)
{
    const struct client_node * node = find_node_by_id (client, nodeid);

    if (node == NULL)
        return USFS_ROOT_ID;

    const struct client_alias * alias = select_node_path_alias (node);

    if (alias == NULL)
        return USFS_ROOT_ID;

    return alias->parent;
}

static uint64_t get_node_parent_id (struct usfs_client * client, const uint64_t nodeid)
{
    pthread_mutex_lock (&client->node_lock);
    const uint64_t parent = get_node_parent_id_locked (client, nodeid);

    pthread_mutex_unlock (&client->node_lock);

    return parent;
}

/* ----------------------------------------------------------- *
 * Attribute and reply marshalling                             *
 * ----------------------------------------------------------- */

static void convert_stat_to_attributes (const struct stat * metadata, const uint64_t fallback_ino, struct usfs_attr * attr)
{
    memset (attr, 0, sizeof (*attr));

    attr->ino = (metadata->st_ino != 0) ? (uint64_t)metadata->st_ino : fallback_ino;
    attr->size = (uint64_t)metadata->st_size;
    attr->blocks = (uint64_t)metadata->st_blocks;
    attr->atime = (int64_t)metadata->st_atime;
    attr->mtime = (int64_t)metadata->st_mtime;
    attr->ctime = (int64_t)metadata->st_ctime;
    attr->atimensec = (uint32_t)metadata->st_atim.tv_nsec;
    attr->mtimensec = (uint32_t)metadata->st_mtim.tv_nsec;
    attr->ctimensec = (uint32_t)metadata->st_ctim.tv_nsec;
    attr->mode = (uint32_t)metadata->st_mode;
    attr->nlink = (uint32_t)metadata->st_nlink;
    attr->uid = (uint32_t)metadata->st_uid;
    attr->gid = (uint32_t)metadata->st_gid;
    attr->rdev = (uint32_t)metadata->st_rdev;
    attr->blksize = (uint32_t)(metadata->st_blksize != 0 ? metadata->st_blksize : CLIENT_DEFAULT_BLOCK_SIZE);
}

static void subtract_managed_hidden_links (struct usfs_client * client, const uint64_t nodeid, struct usfs_attr * attributes)
{
    pthread_mutex_lock (&client->node_lock);
    const struct client_node * node = find_node_by_id (client, nodeid);
    const unsigned hidden_names = node != NULL ? node->hidden_names : 0;

    pthread_mutex_unlock (&client->node_lock);
    attributes->nlink = attributes->nlink > hidden_names ? attributes->nlink - hidden_names : 0;
}

static void convert_node_attributes (struct usfs_client * client, const struct stat * metadata, const uint64_t nodeid, struct usfs_attr * attributes)
{
    convert_stat_to_attributes (metadata, nodeid, attributes);
    subtract_managed_hidden_links (client, nodeid, attributes);
}

static int is_backend_stat_valid (const struct stat * metadata)
{
    struct usfs_attr attr;
    const mode_t type = metadata->st_mode & S_IFMT;

    convert_stat_to_attributes (metadata, USFS_ROOT_ID, &attr);

    if (type == 0)
        return false;

    if (!usfs_attr_valid (&attr))
        return false;

    if (metadata->st_atim.tv_nsec < 0 || metadata->st_atim.tv_nsec >= CLIENT_NANOSECONDS_PER_SECOND)
        return false;

    if (metadata->st_mtim.tv_nsec < 0 || metadata->st_mtim.tv_nsec >= CLIENT_NANOSECONDS_PER_SECOND)
        return false;

    if (metadata->st_ctim.tv_nsec < 0 || metadata->st_ctim.tv_nsec >= CLIENT_NANOSECONDS_PER_SECOND)
        return false;

    if (S_ISDIR (type))
        return true;

    if (metadata->st_nlink <= 1)
        return true;

    return metadata->st_ino != 0;
}

static int write_all_bytes (const int fd, const void * buffer, size_t length)
{
    const char * cursor = buffer;

    while (length > 0)
    {
        const ssize_t written = write (fd, cursor, length);

        if (written < 0)
        {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (written == 0)
        {
            errno = EIO;

            return -1;
        }

        cursor += written;
        length -= (size_t)written;
    }

    return 0;
}

static int resolve_session_result (const struct usfs_client * client, const int result)
{
    const int fatal = __atomic_load_n (&client->session.fatal_error, __ATOMIC_ACQUIRE);

    return fatal != 0 ? -fatal : result;
}

static void fail_session (struct usfs_client * client, int error)
{
    int expected = 0;

    if (error <= 0)
        error = EIO;

    (void)__atomic_compare_exchange_n (&client->session.fatal_error, &expected, error, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    usfs_client_request_stop (client);
}

static char * get_reply_buffer (const struct usfs_client * client)
{
    const struct client_thread_context * thread = get_thread_context ();

    if (thread == NULL)
        return client->writebuf;

    if (thread->writebuf == NULL)
        return client->writebuf;

    return thread->writebuf;
}

static int send_reply (const struct request_context * context, const int error, const void * body, uint32_t body_len)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;
    int reply_error = error;

    char * write_buf = get_reply_buffer (client);
    struct usfs_reply_header * out = (struct usfs_reply_header *)write_buf;

    if (error != 0)
        body_len = 0;

    if ((size_t)body_len > USFS_MSG_MAX - sizeof (*out) || (body_len > 0 && body == NULL))
    {
        reply_error = EIO;
        body_len = 0;
    }

    memset (out, 0, sizeof (*out));
    out->len = (uint32_t)sizeof (*out) + body_len;
    out->version = (uint16_t)USFS_PROTOCOL_VERSION;
    out->opcode = request_header->opcode;
    out->id = request_header->unique;
    out->error = (int32_t)reply_error;

    if (body_len > 0 && body != NULL && body != write_buf + sizeof (*out))
        memcpy (write_buf + sizeof (*out), body, body_len);

    pthread_mutex_lock (&client->reply_lock);
    const int result = write_all_bytes (client->fd, write_buf, out->len);
    if (result != 0)
        fail_session (client, errno);

    pthread_mutex_unlock (&client->reply_lock);

    return result;
}

/* ----------------------------------------------------------- *
 * Request dispatch                                            *
 * ----------------------------------------------------------- */

static void set_request_context (struct usfs_client * client, const struct usfs_in_hdr * request_header)
{
    struct usfs_client_request * context = get_callback_request ();

    context->client = client;
    context->uid = (uid_t)request_header->uid;
    context->gid = (gid_t)request_header->gid;
    context->pid = (pid_t)request_header->pid;
    context->user_data = client->user_data;
}

static int read_backend_attributes (const struct usfs_client * client, const char * path, struct stat * metadata, struct usfs_open_file * file_info)
{
    if (client->ops.getattr == NULL)
        return -ENOSYS;

    memset (metadata, 0, sizeof (*metadata));

    return client->ops.getattr (get_callback_request (), path, metadata, file_info);
}

static int read_path_attributes (const struct usfs_client * client, const char * path, struct stat * metadata)
{
    return read_backend_attributes (client, path, metadata, NULL);
}

static void invalidate_node_directory_cache (struct usfs_client * client, uint64_t nodeid);

struct path_buffer
{
    char data[USFS_PATH_MAX]; // Storage for a full backend path, including its terminator.
};

static int build_child_path (struct usfs_client * client, const uint64_t parent, const char * name, struct path_buffer * path)
{
    const size_t name_length = strlen (name);

    if (name_length + sizeof ("/") >= sizeof (path->data))
        return -ENAMETOOLONG;

    const int rc = build_node_path (client, parent, path->data, sizeof (path->data) - name_length - sizeof ("/"));
    if (rc != 0)
        return rc;

    if (strcmp (path->data, "/") != 0)
        strcat (path->data, "/");

    strcat (path->data, name);

    return 0;
}

/* Validates that a request payload ends in a NUL, i.e. carries a usable name. */
static int is_payload_null_terminated (const char * payload, const uint32_t payload_length)
{
    if (payload_length == 0)
        return false;

    return payload[payload_length - 1] == '\0';
}

static int send_lookup_entry_reply (const struct request_context * context, const uint64_t nodeid, const struct stat * metadata)
{
    struct usfs_entry_out entry = { 0 };

    entry.nodeid = nodeid;
    convert_node_attributes (context->client, metadata, nodeid, &entry.attr);

    return send_reply (context, 0, &entry, (uint32_t)sizeof (entry));
}

static int resolve_relative_lookup_path_locked (const struct usfs_client * client, const char * name, uint64_t * nodeid, struct path_buffer * path)
{
    const struct client_node * node = find_node_by_id (client, *nodeid);

    if (*nodeid != USFS_ROOT_ID)
    {
        if (node == NULL)
            return -ESTALE;

        if (node->detached)
            return -ESTALE;

        if (strcmp (name, "..") == 0)
            *nodeid = select_node_path_alias (node)->parent;
    }

    return build_node_path_locked (client, *nodeid, path->data, sizeof (path->data));
}

static int retain_lookup_reference_locked (struct usfs_client * client, const uint64_t nodeid, const struct stat * metadata)
{
    struct client_node * node = find_node_by_id (client, nodeid);

    if (nodeid != USFS_ROOT_ID)
    {
        if (node == NULL)
            return EIO;

        const int rc = update_node_backend_identity (client, node, metadata);

        if (rc != 0)
            return rc;
    }

    uint64_t * references = nodeid == USFS_ROOT_ID ? &client->root_lookup_refs : &node->lookup_refs;

    if (*references == UINT64_MAX)
        return EOVERFLOW;

    ++*references;

    return 0;
}

static int handle_lookup_self_or_parent_request (const struct request_context * context, const char * name)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct path_buffer path = { 0 };
    uint64_t nodeid = request_header->nodeid;

    pthread_mutex_lock (&client->node_lock);
    int rc = resolve_relative_lookup_path_locked (client, name, &nodeid, &path);

    pthread_mutex_unlock (&client->node_lock);

    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct stat metadata;

    rc = read_path_attributes (client, path.data, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (!is_backend_stat_valid (&metadata))
        return send_reply (context, EIO, NULL, 0);

    if (!S_ISDIR (metadata.st_mode))
        return send_reply (context, EIO, NULL, 0);

    pthread_mutex_lock (&client->node_lock);
    rc = retain_lookup_reference_locked (client, nodeid, &metadata);
    pthread_mutex_unlock (&client->node_lock);

    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return send_lookup_entry_reply (context, nodeid, &metadata);
}

static int handle_lookup_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    const char * name = payload;

    if (!is_payload_null_terminated (name, payload_length))
        return send_reply (context, EINVAL, NULL, 0);

    if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
        return handle_lookup_self_or_parent_request (context, name);

    struct path_buffer path = { 0 };

    int rc = build_child_path (client, request_header->nodeid, name, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct stat metadata;

    rc = read_path_attributes (client, path.data, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    const struct client_node * node = find_or_create_node (client, request_header->nodeid, name, &metadata);
    if (node == NULL)
        return send_reply (context, errno, NULL, 0);

    return send_lookup_entry_reply (context, node->id, &metadata);
}

static int refresh_node_identity (struct usfs_client * client, const uint64_t nodeid, const struct stat * metadata);

struct fid_identity
{
    char path[USFS_PATH_MAX];   // Current alias copied out of the node table.
    const char * callback_path; // Current path, or NULL when all known aliases disappeared.
    uint64_t device;            // Backend device identity supplied to export_id.
    uint64_t inode;             // Backend inode identity supplied to export_id.
    mode_t type;                // Backend object type supplied to export_id.
};

static int prepare_root_fid_identity (struct usfs_client * client, struct fid_identity * identity)
{
    struct stat metadata;
    const int rc = read_path_attributes (client, "/", &metadata);
    if (rc != 0)
        return -rc;

    if (!is_backend_stat_valid (&metadata) || !S_ISDIR (metadata.st_mode))
        return EIO;

    identity->callback_path = "/";
    identity->device = (uint64_t)metadata.st_dev;
    identity->inode = (uint64_t)metadata.st_ino;
    identity->type = metadata.st_mode & S_IFMT;

    return metadata.st_nlink == 0 ? ESTALE : 0;
}

static int copy_cached_fid_identity (struct usfs_client * client, const uint64_t nodeid, struct fid_identity * identity)
{
    pthread_mutex_lock (&client->node_lock);

    struct client_node * node = find_node_by_id (client, nodeid);
    if (node == NULL || node->backend_retired || node->backend_links <= node->hidden_names)
    {
        pthread_mutex_unlock (&client->node_lock);
        return ESTALE;
    }

    identity->device = node->backend_dev;
    identity->inode = node->backend_ino;
    identity->type = node->backend_type;
    identity->callback_path = NULL;

    int rc = 0;
    if (!node->detached)
    {
        rc = build_node_path_locked (client, nodeid, identity->path, sizeof (identity->path));
        if (rc == 0)
            identity->callback_path = identity->path;
    }

    pthread_mutex_unlock (&client->node_lock);

    if (rc != 0)
        return -rc;

    if (identity->callback_path == NULL)
        return identity->inode != 0 && identity->type != 0 ? 0 : ESTALE;

    return 0;
}

static int refresh_named_fid_identity (struct usfs_client * client, const uint64_t nodeid, struct fid_identity * identity)
{
    struct stat metadata;
    int rc = read_path_attributes (client, identity->callback_path, &metadata);
    if (rc != 0)
        return rc == -ENOENT ? ESTALE : -rc;

    if (!is_backend_stat_valid (&metadata))
        return EIO;

    if (metadata.st_nlink == 0)
        return ESTALE;

    rc = refresh_node_identity (client, nodeid, &metadata);
    if (rc != 0)
        return ESTALE;

    identity->device = (uint64_t)metadata.st_dev;
    identity->inode = (uint64_t)metadata.st_ino;
    identity->type = metadata.st_mode & S_IFMT;

    return 0;
}

static int prepare_fid_identity (struct usfs_client * client, const uint64_t nodeid, struct fid_identity * identity)
{
    if (nodeid == USFS_ROOT_ID)
        return prepare_root_fid_identity (client, identity);

    const int rc = copy_cached_fid_identity (client, nodeid, identity);
    if (rc != 0)
        return rc;

    if (identity->callback_path == NULL)
        return 0;

    return refresh_named_fid_identity (client, nodeid, identity);
}

static int handle_fid_request (const struct request_context * context, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;

    if (payload_length != 0)
        return send_reply (context, EINVAL, NULL, 0);

    if (client->ops.export_id == NULL || client->ops.resolve_id == NULL)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    struct fid_identity identity = { 0 };
    const int rc = prepare_fid_identity (client, context->header->nodeid, &identity);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    struct usfs_fid_out reply = { 0 };
    const int callback_result =
        client->ops.export_id (get_callback_request (), identity.callback_path, identity.device, identity.inode, identity.type, &reply.token);
    const int callback_error = normalize_callback_error (callback_result);
    if (callback_error != 0)
        return send_reply (context, callback_error, NULL, 0);

    if (reply.token == 0)
        return send_reply (context, EIO, NULL, 0);

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int validate_resolved_identity_path (const char * path)
{
    if (path[0] != '/')
        return EIO;

    if (path[1] == '\0')
        return 0;

    const char * component = path + 1;
    size_t depth = 0;

    while (*component != '\0')
    {
        const char * separator = strchr (component, '/');
        const size_t name_length = separator != NULL ? (size_t)(separator - component) : strlen (component);

        if (name_length == 0 || name_length >= USFS_MAX_NAME)
            return EIO;

        if (name_length == 1 && component[0] == '.')
            return EIO;

        if (name_length == 2 && component[0] == '.' && component[1] == '.')
            return EIO;

        if (++depth > USFS_MAX_DEPTH)
            return EIO;

        if (separator == NULL)
            return 0;

        component = separator + 1;
        if (*component == '\0')
            return EIO;
    }

    return EIO;
}

static void release_resolved_path_references (struct usfs_client * client, const uint64_t nodeids[USFS_MAX_DEPTH], const size_t count)
{
    pthread_mutex_lock (&client->node_lock);

    for (size_t node_index = 0; node_index < count; node_index++)
    {
        struct client_node * node = find_node_by_id (client, nodeids[node_index]);
        if (node == NULL)
            continue;

        node->lookup_refs--;
        queue_node_for_collection (client, node);
    }

    collect_unreferenced_nodes (client);
    pthread_mutex_unlock (&client->node_lock);
}

static int adopt_resolved_identity_path (
    struct usfs_client * client,
    char path[USFS_PATH_MAX],
    const struct stat * target_metadata,
    uint64_t * nodeid
)
{
    uint64_t temporary_nodeids[USFS_MAX_DEPTH] = { 0 };
    size_t adopted_count = 0;
    uint64_t parent_id = USFS_ROOT_ID;
    char * component = path + 1;
    int error = 0;

    while (*component != '\0')
    {
        char * separator = strchr (component, '/');
        if (separator != NULL)
            *separator = '\0';

        struct stat metadata;
        if (separator == NULL)
        {
            metadata = *target_metadata;
        }
        else
        {
            const int rc = read_path_attributes (client, path, &metadata);
            if (rc != 0)
                error = rc == -ENOENT ? ESTALE : -rc;
            else if (!is_backend_stat_valid (&metadata) || !S_ISDIR (metadata.st_mode))
                error = ESTALE;
        }

        if (error == 0)
        {
            const struct client_node * node = find_or_create_node (client, parent_id, component, &metadata);
            if (node == NULL)
                error = errno == EIO ? ESTALE : errno;
            else
            {
                parent_id = node->id;
                temporary_nodeids[adopted_count++] = node->id;
            }
        }

        if (separator != NULL)
            *separator = '/';

        if (error != 0 || separator == NULL)
            break;

        component = separator + 1;
    }

    if (error != 0)
    {
        release_resolved_path_references (client, temporary_nodeids, adopted_count);
        return error;
    }

    *nodeid = parent_id;
    release_resolved_path_references (client, temporary_nodeids, adopted_count - 1u);

    return 0;
}

static int parse_vget_request (
    const struct request_context * context,
    const char * payload,
    const uint32_t payload_length,
    struct usfs_vget_in * request
)
{
    if (context->header->nodeid != USFS_ROOT_ID)
        return EINVAL;

    if (payload_length != sizeof (*request))
        return EINVAL;

    memcpy (request, payload, sizeof (*request));
    if (request->token == 0)
        return EINVAL;

    return 0;
}

/* Resolve and verify a current backend identity without acquiring lookup refs. */
static int resolve_vget_identity (struct usfs_client * client, const uint64_t token, char path[USFS_PATH_MAX], struct stat * metadata)
{
    memset (path, UCHAR_MAX, USFS_PATH_MAX);

    const int resolve_result = client->ops.resolve_id (get_callback_request (), token, path, USFS_PATH_MAX);
    const int resolve_error = normalize_callback_error (resolve_result);
    if (resolve_error != 0)
        return resolve_error;

    if (memchr (path, '\0', USFS_PATH_MAX) == NULL)
        return EIO;

    const int path_error = validate_resolved_identity_path (path);
    if (path_error != 0)
        return path_error;

    const int attribute_result = read_path_attributes (client, path, metadata);
    if (attribute_result != 0)
        return attribute_result == -ENOENT ? ESTALE : -attribute_result;

    if (!is_backend_stat_valid (metadata))
        return EIO;

    if (metadata->st_nlink == 0)
        return ESTALE;

    uint64_t current_token = 0;
    const int export_result = client->ops.export_id (
        get_callback_request (),
        path,
        (uint64_t)metadata->st_dev,
        (uint64_t)metadata->st_ino,
        metadata->st_mode & S_IFMT,
        &current_token
    );
    const int export_error = normalize_callback_error (export_result);
    if (export_error != 0)
        return export_error;

    if (current_token != token)
        return ESTALE;

    return 0;
}

/* A successful VGET transfers exactly one target lookup reference. */
static int materialize_vget_node (struct usfs_client * client, char path[USFS_PATH_MAX], const struct stat * metadata, uint64_t * nodeid)
{
    *nodeid = USFS_ROOT_ID;

    if (strcmp (path, "/") != 0)
        return adopt_resolved_identity_path (client, path, metadata, nodeid);

    if (!S_ISDIR (metadata->st_mode))
        return ESTALE;

    pthread_mutex_lock (&client->node_lock);

    if (client->root_lookup_refs == UINT64_MAX)
    {
        pthread_mutex_unlock (&client->node_lock);
        return EOVERFLOW;
    }

    client->root_lookup_refs++;
    pthread_mutex_unlock (&client->node_lock);

    return 0;
}

static int handle_vget_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    struct usfs_vget_in request;

    int rc = parse_vget_request (context, payload, payload_length, &request);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (client->ops.resolve_id == NULL || client->ops.export_id == NULL)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    char path[USFS_PATH_MAX];
    struct stat metadata;

    rc = resolve_vget_identity (client, request.token, path, &metadata);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    uint64_t nodeid;
    rc = materialize_vget_node (client, path, &metadata, &nodeid);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    struct usfs_vget_out reply = { 0 };
    reply.token = request.token;
    reply.entry.nodeid = nodeid;
    convert_node_attributes (client, &metadata, nodeid, &reply.entry.attr);

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int read_handle_attributes (
    struct usfs_client * client,
    const uint64_t nodeid,
    const char * path,
    const uint64_t handle,
    struct stat * metadata
)
{
    if (handle == 0)
        return read_path_attributes (client, path, metadata);

    struct usfs_open_file file_info = { 0 };

    file_info.value = backend_handle_value (client, nodeid, handle);

    return read_backend_attributes (client, path, metadata, &file_info);
}

static int refresh_node_identity (struct usfs_client * client, const uint64_t nodeid, const struct stat * metadata)
{
    if (nodeid == USFS_ROOT_ID)
        return 0;

    pthread_mutex_lock (&client->node_lock);
    struct client_node * node = find_node_by_id (client, nodeid);
    const int rc = node != NULL ? update_node_backend_identity (client, node, metadata) : EIO;

    pthread_mutex_unlock (&client->node_lock);

    return rc;
}

static int send_node_attributes (const struct request_context * context, const struct stat * metadata)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_attr_out reply = { 0 };

    convert_node_attributes (client, metadata, request_header->nodeid, &reply.attr);

    reply.parent = request_header->nodeid == USFS_ROOT_ID ? USFS_ROOT_ID : get_node_parent_id (client, request_header->nodeid);

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int handle_getattr_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_getattr_in request = { 0 };

    if (payload_length >= sizeof (request))
        memcpy (&request, payload, sizeof (request));

    struct resolved_handle_path resolved = { 0 };

    int rc = resolve_node_handle_path (client, request_header->nodeid, request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct stat metadata;

    omit_handle_callback_path (client, request.fh != 0, &resolved);
    rc = read_handle_attributes (client, request_header->nodeid, resolved.path, request.fh, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (!is_backend_stat_valid (&metadata))
        return send_reply (context, EIO, NULL, 0);

    rc = refresh_node_identity (client, request_header->nodeid, &metadata);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return send_node_attributes (context, &metadata);
}

static int handle_readlink_request (const struct request_context * context)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    char path[USFS_PATH_MAX];

    int rc = build_node_path (client, request_header->nodeid, path, sizeof (path));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (client->ops.readlink == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    char link[USFS_MAX_LINK];

    memset (link, UCHAR_MAX, sizeof (link));

    rc = client->ops.readlink (get_callback_request (), path, link, sizeof (link));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    const char * terminator = memchr (link, '\0', sizeof (link));
    if (terminator == NULL)
        return send_reply (context, EIO, NULL, 0);

    return send_reply (context, 0, link, (uint32_t)(terminator - link));
}

static int send_filesystem_statistics (const struct request_context * context, const struct statvfs * metadata)
{
    if ((metadata->f_frsize != 0 ? metadata->f_frsize : metadata->f_bsize) > UINT32_MAX)
        return send_reply (context, EIO, NULL, 0);

    if (metadata->f_namemax > UINT32_MAX)
        return send_reply (context, EIO, NULL, 0);

    struct usfs_statfs_out statistics_reply = { 0 };

    statistics_reply.blocks = (uint64_t)metadata->f_blocks;
    statistics_reply.bfree = (uint64_t)metadata->f_bfree;
    statistics_reply.bavail = (uint64_t)metadata->f_bavail;
    statistics_reply.files = (uint64_t)metadata->f_files;
    statistics_reply.ffree = (uint64_t)metadata->f_ffree;
    statistics_reply.bsize = (uint32_t)(metadata->f_frsize != 0 ? metadata->f_frsize : metadata->f_bsize);
    statistics_reply.namemax = (uint32_t)metadata->f_namemax;

    if (!usfs_statfs_out_valid (&statistics_reply))
        return send_reply (context, EIO, NULL, 0);

    return send_reply (context, 0, &statistics_reply, (uint32_t)sizeof (statistics_reply));
}

static int handle_statfs_request (const struct request_context * context)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    if (client->ops.statfs == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    char path[USFS_PATH_MAX];

    int rc = build_node_path (client, request_header->nodeid, path, sizeof (path));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct statvfs metadata = { 0 };

    rc = client->ops.statfs (get_callback_request (), path, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    return send_filesystem_statistics (context, &metadata);
}

/* ----------------------------------------------------------- *
 * Mutating operations                                         *
 *                                                             *
 * A file system that does not implement one of these is read  *
 * only, so the answer is EROFS rather than ENOSYS: that is    *
 * what the operation means to the caller.                     *
 * ----------------------------------------------------------- */

static int handle_mkdir_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_mkdir_in mkdir_request;

    if (payload_length < sizeof (mkdir_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&mkdir_request, payload, sizeof (mkdir_request));

    const char * name = payload + sizeof (mkdir_request);

    if (!is_payload_null_terminated (name, payload_length - (uint32_t)sizeof (mkdir_request)))
        return send_reply (context, EINVAL, NULL, 0);

    if (client->ops.mkdir == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };

    int rc = build_child_path (client, request_header->nodeid, name, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = client->ops.mkdir (get_callback_request (), path.data, (mode_t)mkdir_request.mode);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    invalidate_node_directory_cache (client, request_header->nodeid);

    return send_reply (context, 0, NULL, 0);
}

struct create_resources
{
    const char * name;                  // Borrowed name in the validated request payload.
    struct path_buffer path;            // Resolved backend path for creation and failure cleanup.
    struct open_handle_state opened;    // Backend handle state to publish after node adoption.
    struct client_node * prepared_node; // Owned reservation until adoption or destruction.
    struct client_handle * handle;      // Owned client handle until publication or failure cleanup.
};

static void read_created_file_attributes (
    const struct usfs_client * client,
    const struct usfs_create_in * request,
    struct create_resources * resources,
    struct stat * metadata
)
{
    const int rc = read_backend_attributes (client, resources->path.data, metadata, &resources->opened.file_info);
    if (rc == 0 && is_backend_stat_valid (metadata))
        return;

    memset (metadata, 0, sizeof (*metadata));
    metadata->st_mode = S_IFREG | ((mode_t)request->mode & CLIENT_CREATE_PERMISSION_MASK);
    metadata->st_nlink = 1;
    metadata->st_uid = get_callback_request ()->uid;
    metadata->st_gid = get_callback_request ()->gid;
    metadata->st_blksize = CLIENT_DEFAULT_BLOCK_SIZE;
}

static int finish_file_creation (const struct request_context * context, const struct usfs_create_in * request, struct create_resources * resources)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct stat metadata;
    int rc;

    read_created_file_attributes (client, request, resources, &metadata);

    pthread_mutex_lock (&client->node_lock);
    const struct client_node * node = adopt_prepared_node (client, &resources->prepared_node, &metadata, &rc);

    pthread_mutex_unlock (&client->node_lock);
    destroy_node (resources->prepared_node);

    if (node == NULL)
    {
        if (client->ops.release != NULL)
            client->ops.release (get_callback_request (), resources->path.data, &resources->opened.file_info);

        free (resources->handle);
        fail_session (client, rc);

        return send_reply (context, rc, NULL, 0);
    }

    resources->opened.nodeid = node->id;
    publish_client_handle (client, resources->handle, &resources->opened);

    invalidate_node_directory_cache (client, request_header->nodeid);

    struct usfs_create_out create_reply = { 0 };

    create_reply.nodeid = node->id;
    create_reply.fh = resources->opened.wire_fh;
    convert_node_attributes (client, &metadata, node->id, &create_reply.attr);

    return send_reply (context, 0, &create_reply, (uint32_t)sizeof (create_reply));
}

static int create_prepared_file (const struct request_context * context, const struct usfs_create_in * request, struct create_resources * resources)
{
    struct usfs_client * client = context->client;

    const int rc = client->ops.create (get_callback_request (), resources->path.data, (mode_t)request->mode, &resources->opened.file_info);
    if (rc != 0)
    {
        free (resources->handle);
        destroy_node (resources->prepared_node);

        return send_reply (context, -rc, NULL, 0);
    }

    return finish_file_creation (context, request, resources);
}

static int parse_create_request (
    const char * payload,
    const uint32_t payload_length,
    struct usfs_create_in * request,
    struct create_resources * resources
)
{
    if (payload_length < sizeof (*request))
        return EINVAL;

    memcpy (request, payload, sizeof (*request));

    if ((request->flags & _FEXEC) != 0)
        return EINVAL;

    memset (&resources->opened.file_info, 0, sizeof (resources->opened.file_info));
    resources->opened.file_info.open_flags = translate_open_flags (request->flags);

    if (resources->opened.file_info.open_flags < 0)
        return -resources->opened.file_info.open_flags;

    resources->name = payload + sizeof (*request);

    if (!is_payload_null_terminated (resources->name, payload_length - (uint32_t)sizeof (*request)))
        return EINVAL;

    return 0;
}

static int prepare_create_resources (const struct usfs_client * client, const uint64_t parent, struct create_resources * resources)
{
    if (client->next_id == UINT64_MAX)
        return EOVERFLOW;

    resources->prepared_node = prepare_node (parent, resources->name);

    if (resources->prepared_node == NULL)
        return ENOMEM;

    resources->handle = calloc (1, sizeof (*resources->handle));

    if (resources->handle == NULL)
    {
        destroy_node (resources->prepared_node);
        resources->prepared_node = NULL;

        return ENOMEM;
    }

    return 0;
}

static int handle_create_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_create_in request;
    struct create_resources resources = { 0 };

    int rc = parse_create_request (payload, payload_length, &request, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (client->ops.create == NULL)
        return send_reply (context, EROFS, NULL, 0);

    rc = build_child_path (client, request_header->nodeid, resources.name, &resources.path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = prepare_create_resources (client, request_header->nodeid, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return create_prepared_file (context, &request, &resources);
}

static void build_create_attr_initial_stat (const struct usfs_create_attr_in * request, struct stat * initial)
{
    memset (initial, 0, sizeof (*initial));
    initial->st_mode = S_IFREG | request->attr.mode;
    initial->st_uid = request->attr.valid & USFS_SET_UID ? request->attr.uid : get_callback_request ()->uid;
    initial->st_gid = request->attr.valid & USFS_SET_GID ? request->attr.gid : get_callback_request ()->gid;
    initial->st_size = (off_t)request->attr.size;
    initial->st_atim.tv_sec = request->attr.atime;
    initial->st_atim.tv_nsec = request->attr.atimensec;
    initial->st_mtim.tv_sec = request->attr.mtime;
    initial->st_mtim.tv_nsec = request->attr.mtimensec;
    initial->st_ctim.tv_sec = request->attr.ctime;
    initial->st_ctim.tv_nsec = request->attr.ctimensec;
}

static int parse_create_attr_request (
    const char * payload,
    const uint32_t payload_length,
    struct usfs_create_attr_in * request,
    struct create_resources * resources
)
{
    if (payload_length < sizeof (*request))
        return EINVAL;

    memcpy (request, payload, sizeof (*request));

    if (!usfs_create_attr_in_valid (request))
        return EINVAL;

    if ((request->flags & _FEXEC) != 0)
        return EINVAL;

    const uint32_t name_len = payload_length - (uint32_t)sizeof (*request);

    if (name_len > USFS_MAX_NAME)
        return ENAMETOOLONG;

    resources->name = payload + sizeof (*request);

    if (!is_payload_null_terminated (resources->name, name_len))
        return EINVAL;

    memset (&resources->opened.file_info, 0, sizeof (resources->opened.file_info));
    resources->opened.file_info.open_flags =
        request->activation != USFS_CREATE_OPEN && request->flags == 0 ? O_RDONLY : translate_open_flags (request->flags);

    if (resources->opened.file_info.open_flags < 0)
        return -resources->opened.file_info.open_flags;

    return 0;
}

static int handle_create_attr_fallback (
    const struct request_context * context,
    const struct usfs_create_attr_in * request,
    const char * name,
    const uint32_t name_len
)
{
    if (request->activation != USFS_CREATE_OPEN)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    if (request->attr.valid != USFS_SET_MODE)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    struct usfs_create_in create;

    create.flags = request->flags;
    create.mode = request->attr.mode;
    char ordinary[sizeof (struct usfs_create_in) + USFS_MAX_NAME];

    memcpy (ordinary, &create, sizeof (create));
    memcpy (ordinary + sizeof (create), name, name_len);

    return handle_create_request (context, ordinary, (uint32_t)sizeof (create) + name_len);
}

static int prepare_create_attr_resources (
    const struct usfs_client * client,
    const struct usfs_create_attr_in * request,
    const uint64_t parent,
    struct create_resources * resources
)
{
    resources->prepared_node = NULL;
    resources->handle = NULL;

    if (request->activation != USFS_CREATE_DEFAULT)
    {
        if (client->next_id == UINT64_MAX)
            return EOVERFLOW;

        resources->prepared_node = prepare_node (parent, resources->name);

        if (resources->prepared_node == NULL)
            return ENOMEM;
    }

    if (request->activation != USFS_CREATE_OPEN)
        return 0;

    resources->handle = calloc (1, sizeof (*resources->handle));

    if (resources->handle != NULL)
        return 0;

    destroy_node (resources->prepared_node);
    resources->prepared_node = NULL;

    return ENOMEM;
}

static int adopt_created_attributes (
    struct usfs_client * client,
    const struct stat * metadata,
    struct client_node ** prepared,
    struct client_node ** node
)
{
    if (!is_backend_stat_valid (metadata))
        return EIO;

    if (!S_ISREG (metadata->st_mode))
        return EIO;

    if (*prepared == NULL)
        return 0;

    int rc;

    pthread_mutex_lock (&client->node_lock);
    *node = adopt_prepared_node (client, prepared, metadata, &rc);
    pthread_mutex_unlock (&client->node_lock);

    return rc;
}

static int finish_attribute_creation (
    const struct request_context * context,
    const struct usfs_create_attr_in * request,
    struct create_resources * resources,
    const struct stat * metadata
)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct client_node * node = NULL;
    const int rc = adopt_created_attributes (client, metadata, &resources->prepared_node, &node);

    destroy_node (resources->prepared_node);

    if (rc != 0)
    {
        if (request->activation == USFS_CREATE_OPEN && client->ops.release != NULL)
            client->ops.release (get_callback_request (), resources->path.data, &resources->opened.file_info);

        fail_session (client, rc);
        free (resources->handle);

        return send_reply (context, rc, NULL, 0);
    }

    if (resources->handle != NULL)
    {
        resources->opened.nodeid = node->id;
        publish_client_handle (client, resources->handle, &resources->opened);
    }

    invalidate_node_directory_cache (client, request_header->nodeid);

    struct usfs_create_out reply = { 0 };

    reply.nodeid = node != NULL ? node->id : 0;
    reply.fh = resources->opened.wire_fh;
    convert_stat_to_attributes (metadata, USFS_ROOT_ID, &reply.attr);
    subtract_managed_hidden_links (client, reply.nodeid, &reply.attr);

    return send_reply (context, 0, &reply, sizeof (reply));
}

static int execute_prepared_create_attr (
    const struct request_context * context,
    const struct usfs_create_attr_in * request,
    struct create_resources * resources
)
{
    struct usfs_client * client = context->client;

    struct stat initial;
    struct stat result;

    build_create_attr_initial_stat (request, &initial);
    memset (&result, 0, sizeof (result));
    const int rc = client->ops.create_attr (
        get_callback_request (),
        resources->path.data,
        &initial,
        request->attr.valid,
        (enum usfs_create_action)request->activation,
        &result,
        request->activation == USFS_CREATE_OPEN ? &resources->opened.file_info : NULL
    );
    if (rc != 0)
    {
        free (resources->handle);
        destroy_node (resources->prepared_node);

        return send_reply (context, normalize_callback_error (rc), NULL, 0);
    }

    return finish_attribute_creation (context, request, resources, &result);
}

static int handle_create_attr_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_create_attr_in create_request;
    struct create_resources resources = { 0 };

    int rc = parse_create_attr_request (payload, payload_length, &create_request, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (client->ops.create_attr == NULL)
        return handle_create_attr_fallback (context, &create_request, resources.name, payload_length - (uint32_t)sizeof (create_request));

    rc = build_child_path (client, request_header->nodeid, resources.name, &resources.path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = prepare_create_attr_resources (client, &create_request, request_header->nodeid, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return execute_prepared_create_attr (context, &create_request, &resources);
}

static void publish_removed_entry (struct usfs_client * client, const uint64_t parent, const char * name)
{
    uint64_t removed_id = 0;

    pthread_mutex_lock (&client->node_lock);
    struct client_alias * removed = find_alias (client, parent, name);

    if (removed != NULL)
    {
        removed_id = removed->node->id;
        remove_alias (client, removed);
    }

    pthread_mutex_unlock (&client->node_lock);

    if (removed_id != 0)
        invalidate_node_directory_cache (client, removed_id);

    invalidate_node_directory_cache (client, parent);
}

struct hidden_entry_operation
{
    struct client_alias * alias; // Alias retained by exclusive namespace admission.
    char * original_name;        // Owned rollback name, prepared before the backend mutation.
    struct path_buffer path;     // Hidden backend pathname selected for this transaction.
    int was_hidden;              // Whether the original alias was already managed.
};

static int select_hidden_name (
    struct usfs_client * client,
    const struct client_alias * alias,
    struct hidden_entry_operation * operation,
    char ** prepared_name
)
{
    for (unsigned attempt = 0; attempt < CLIENT_HIDDEN_NAME_ATTEMPTS; attempt++)
    {
        char name[CLIENT_HIDDEN_NAME_CAPACITY];

        pthread_mutex_lock (&client->node_lock);
        client->hidden_generation++;
        snprintf (
            name,
            sizeof (name),
            ".fuse_hidden%016llx%016llx",
            (unsigned long long)alias->node->id,
            (unsigned long long)client->hidden_generation
        );
        const int occupied = find_alias (client, alias->parent, name) != NULL;

        pthread_mutex_unlock (&client->node_lock);

        if (occupied)
            continue;

        int rc = build_child_path (client, alias->parent, name, &operation->path);
        if (rc != 0)
            return rc;

        struct stat metadata = { 0 };

        rc = read_path_attributes (client, operation->path.data, &metadata);
        if (rc == 0)
            continue;

        if (rc != -ENOENT)
            return rc;

        *prepared_name = strdup (name);

        return *prepared_name != NULL ? 0 : -ENOMEM;
    }

    return -EBUSY;
}

static void publish_hidden_alias (
    struct usfs_client * client,
    struct client_alias * alias,
    char * prepared_name,
    struct hidden_entry_operation * operation
)
{
    pthread_mutex_lock (&client->node_lock);
    operation->alias = alias;
    operation->was_hidden = alias->managed_hidden;
    relink_alias_with_prepared_name (client, alias, alias->parent, prepared_name);

    if (!alias->managed_hidden)
    {
        alias->managed_hidden = true;
        alias->node->hidden_names++;
    }

    alias->cleanup_attempted = false;
    alias->node->detached = alias->node->names == alias->node->hidden_names;
    pthread_mutex_unlock (&client->node_lock);
    invalidate_node_directory_cache (client, alias->parent);
    invalidate_node_directory_cache (client, alias->node->id);
}

/* The namespace writer owns the alias through selection, callbacks and publication. */
static int hide_open_entry (
    struct usfs_client * client,
    const uint64_t parent,
    const char * name,
    const char * path,
    struct hidden_entry_operation * operation
)
{
    if ((client->behavior.remove_policy == USFS_REMOVE_IMMEDIATE))
        return 0;

    pthread_mutex_lock (&client->node_lock);
    struct client_alias * alias = find_alias (client, parent, name);
    const int should_hide = alias != NULL && alias->node->open_refs != 0 && !S_ISDIR (alias->node->backend_type);

    pthread_mutex_unlock (&client->node_lock);

    if (!should_hide)
        return 0;

    if (client->ops.rename == NULL || client->ops.unlink == NULL || client->ops.getattr == NULL)
        return -ENOSYS;

    operation->original_name = strdup (name);
    if (operation->original_name == NULL)
        return -ENOMEM;

    char * prepared_name = NULL;
    int rc = select_hidden_name (client, alias, operation, &prepared_name);
    if (rc != 0)
        return rc;

    rc = client->ops.rename (get_callback_request (), path, operation->path.data);
    if (rc != 0)
    {
        free (prepared_name);
        return rc;
    }

    publish_hidden_alias (client, alias, prepared_name, operation);

    return 1;
}

static int rollback_hidden_entry (struct usfs_client * client, const char * original_path, struct hidden_entry_operation * operation)
{
    struct stat metadata = { 0 };
    int rc = read_path_attributes (client, original_path, &metadata);
    if (rc != -ENOENT)
        return rc == 0 ? -EEXIST : rc;

    rc = client->ops.rename (get_callback_request (), operation->path.data, original_path);
    if (rc != 0)
        return rc;

    pthread_mutex_lock (&client->node_lock);
    struct client_alias * alias = operation->alias;

    relink_alias_with_prepared_name (client, alias, alias->parent, operation->original_name);
    operation->original_name = NULL;

    if (!operation->was_hidden)
    {
        alias->managed_hidden = false;
        alias->node->hidden_names--;
    }

    alias->node->detached = alias->node->names == alias->node->hidden_names;
    pthread_mutex_unlock (&client->node_lock);
    invalidate_node_directory_cache (client, alias->parent);

    return 0;
}

static int handle_remove_entry_request (
    const struct request_context * context,
    const char * payload,
    const uint32_t payload_length,
    int (*op) (const struct usfs_client_request *, const char *)
)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    if (!is_payload_null_terminated (payload, payload_length))
        return send_reply (context, EINVAL, NULL, 0);

    if (op == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };
    int rc = build_child_path (client, request_header->nodeid, payload, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = op (get_callback_request (), path.data);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    publish_removed_entry (client, request_header->nodeid, payload);

    return send_reply (context, 0, NULL, 0);
}

static int handle_unlink_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;

    if (!is_payload_null_terminated (payload, payload_length))
        return send_reply (context, EINVAL, NULL, 0);

    if (client->ops.unlink == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };
    int rc = build_child_path (client, context->header->nodeid, payload, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct hidden_entry_operation hidden = { 0 };

    rc = hide_open_entry (client, context->header->nodeid, payload, path.data, &hidden);
    free (hidden.original_name);

    if (rc < 0)
        return send_reply (context, normalize_callback_error (rc), NULL, 0);

    if (rc > 0)
        return send_reply (context, 0, NULL, 0);

    return handle_remove_entry_request (context, payload, payload_length, client->ops.unlink);
}

static int handle_rmdir_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;

    return handle_remove_entry_request (context, payload, payload_length, client->ops.rmdir);
}

struct rename_operation
{
    struct usfs_rename_in request; // Decoded rename flags and destination parent.
    uint64_t old_parent;           // Source parent from the request header.
    const char * old_name;         // Borrowed source name in the request payload.
    const char * new_name;         // Borrowed destination name in the request payload.
    struct path_buffer old_path;   // Resolved source backend path.
    struct path_buffer new_path;   // Resolved destination backend path.
};

static int parse_rename_request (const char * payload, const uint32_t payload_length, struct rename_operation * operation)
{
    if (payload_length < sizeof (operation->request))
        return EINVAL;

    memcpy (&operation->request, payload, sizeof (operation->request));

    const uint32_t names_length = payload_length - (uint32_t)sizeof (operation->request);

    operation->old_name = payload + sizeof (operation->request);

    if (operation->request.oldnamelen > names_length)
        return EINVAL;

    if (!is_payload_null_terminated (operation->old_name, operation->request.oldnamelen))
        return EINVAL;

    operation->new_name = operation->old_name + operation->request.oldnamelen;

    if (!is_payload_null_terminated (operation->new_name, names_length - operation->request.oldnamelen))
        return EINVAL;

    return 0;
}

static int relink_renamed_entry_locked (struct usfs_client * client, const struct rename_operation * operation, char * prepared_name)
{
    struct client_alias * alias = find_alias (client, operation->old_parent, operation->old_name);

    if (alias == NULL)
        return false;

    struct client_alias * replaced = find_alias (client, operation->request.newparent, operation->new_name);

    if (replaced != NULL)
    {
        if (replaced->node == alias->node)
            return false;

        remove_alias (client, replaced);
    }

    relink_alias_with_prepared_name (client, alias, operation->request.newparent, prepared_name);

    return true;
}

static void publish_renamed_entry (struct usfs_client * client, const struct rename_operation * operation, char * prepared_name)
{
    pthread_mutex_lock (&client->node_lock);
    const int name_adopted = relink_renamed_entry_locked (client, operation, prepared_name);

    pthread_mutex_unlock (&client->node_lock);

    if (!name_adopted)
        free (prepared_name);
}

static int rename_targets_same_object (struct usfs_client * client, const struct rename_operation * operation)
{
    pthread_mutex_lock (&client->node_lock);
    const struct client_alias * source = find_alias (client, operation->old_parent, operation->old_name);
    const struct client_alias * destination = find_alias (client, operation->request.newparent, operation->new_name);
    const int same_object = source != NULL && destination != NULL && source->node == destination->node;

    pthread_mutex_unlock (&client->node_lock);

    return same_object || strcmp (operation->old_path.data, operation->new_path.data) == 0;
}

static int rename_with_hidden_destination (struct usfs_client * client, const struct rename_operation * operation)
{
    struct hidden_entry_operation hidden = { 0 };
    int rc = hide_open_entry (client, operation->request.newparent, operation->new_name, operation->new_path.data, &hidden);
    if (rc < 0)
    {
        free (hidden.original_name);
        return rc;
    }

    rc = client->ops.rename (get_callback_request (), operation->old_path.data, operation->new_path.data);
    if (rc != 0)
    {
        if (hidden.alias != NULL)
        {
            const int rollback_error = rollback_hidden_entry (client, operation->new_path.data, &hidden);
            if (rollback_error != 0)
            {
                emit_diagnostic (
                    client,
                    "Failed to replace %s: error %d; failed to restore hidden destination %s: error %d\n",
                    operation->new_path.data,
                    normalize_callback_error (rc),
                    hidden.path.data,
                    normalize_callback_error (rollback_error)
                );
                rc = -EIO;
            }
        }
    }

    free (hidden.original_name);

    return rc;
}

static int rename_entry_and_reply (const struct request_context * context, const struct rename_operation * operation)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    if (rename_targets_same_object (client, operation))
        return send_reply (context, 0, NULL, 0);

    char * prepared_name = strdup (operation->new_name);
    if (prepared_name == NULL)
        return send_reply (context, ENOMEM, NULL, 0);

    const int rc = rename_with_hidden_destination (client, operation);
    if (rc != 0)
    {
        free (prepared_name);
        return send_reply (context, normalize_callback_error (rc), NULL, 0);
    }

    publish_renamed_entry (client, operation, prepared_name);

    invalidate_node_directory_cache (client, request_header->nodeid);

    if (operation->request.newparent != request_header->nodeid)
        invalidate_node_directory_cache (client, operation->request.newparent);

    return send_reply (context, 0, NULL, 0);
}

static int handle_rename_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct rename_operation operation = { 0 };

    operation.old_parent = request_header->nodeid;

    int rc = parse_rename_request (payload, payload_length, &operation);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (client->ops.rename == NULL)
        return send_reply (context, EROFS, NULL, 0);

    rc = build_child_path (client, operation.old_parent, operation.old_name, &operation.old_path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = build_child_path (client, operation.request.newparent, operation.new_name, &operation.new_path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    return rename_entry_and_reply (context, &operation);
}

/*
 * One request can carry several changes, and the high-level API splits them
 * across chmod/chown/truncate/utimens. Each is applied in turn and the first
 * failure is reported, so a partly applied change is still visible rather than
 * silently rolled back -- which is also what a sequence of separate calls to
 * these callbacks would leave behind.
 */
static int has_requested_setattr_callbacks (const struct usfs_client * client, const uint32_t valid)
{
    if ((valid & USFS_SET_MODE) != 0 && client->ops.chmod == NULL)
        return false;

    if ((valid & (USFS_SET_UID | USFS_SET_GID)) != 0 && client->ops.chown == NULL)
        return false;

    if ((valid & USFS_SET_SIZE) != 0 && client->ops.truncate == NULL)
        return false;

    if ((valid & (USFS_SET_ATIME | USFS_SET_MTIME)) != 0 && client->ops.utimens == NULL)
        return false;

    return true;
}

enum attribute_time_slot
{
    ATTRIBUTE_ACCESS_TIME = 0,
    ATTRIBUTE_MODIFICATION_TIME = 1,
    ATTRIBUTE_TIME_COUNT = 2
};

static void build_setattr_times (const struct usfs_setattr_in * request, struct timespec times[ATTRIBUTE_TIME_COUNT])
{
    memset (times, 0, ATTRIBUTE_TIME_COUNT * sizeof (*times));

    if (request->valid & USFS_SET_TIMES_NOW)
    {
        times[ATTRIBUTE_ACCESS_TIME].tv_nsec = USFS_TIME_NOW;
        times[ATTRIBUTE_MODIFICATION_TIME].tv_nsec = USFS_TIME_NOW;

        return;
    }

    times[ATTRIBUTE_ACCESS_TIME].tv_sec = (time_t)request->atime;
    times[ATTRIBUTE_ACCESS_TIME].tv_nsec = (long)request->atimensec;
    times[ATTRIBUTE_MODIFICATION_TIME].tv_sec = (time_t)request->mtime;
    times[ATTRIBUTE_MODIFICATION_TIME].tv_nsec = (long)request->mtimensec;

    if ((request->valid & USFS_SET_ATIME) == 0)
        times[ATTRIBUTE_ACCESS_TIME].tv_nsec = USFS_TIME_OMIT;
    if ((request->valid & USFS_SET_MTIME) == 0)
        times[ATTRIBUTE_MODIFICATION_TIME].tv_nsec = USFS_TIME_OMIT;
}

static int apply_requested_owner (
    const struct usfs_client * client,
    const char * path,
    const struct usfs_setattr_in * request,
    struct usfs_open_file * file_info
)
{
    if ((request->valid & (USFS_SET_UID | USFS_SET_GID)) == 0)
        return 0;

    /* -1 preserves the owner or group not included in this request. */
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;

    if (request->valid & USFS_SET_UID)
        uid = (uid_t)request->uid;

    if (request->valid & USFS_SET_GID)
        gid = (gid_t)request->gid;

    return client->ops.chown (get_callback_request (), path, uid, gid, file_info);
}

static int apply_requested_times (
    const struct usfs_client * client,
    const char * path,
    const struct usfs_setattr_in * request,
    struct usfs_open_file * file_info
)
{
    if ((request->valid & (USFS_SET_ATIME | USFS_SET_MTIME)) == 0)
        return 0;

    struct timespec requested_times[ATTRIBUTE_TIME_COUNT];

    build_setattr_times (request, requested_times);

    return client->ops.utimens (get_callback_request (), path, requested_times, file_info);
}

static int apply_requested_attributes (
    const struct usfs_client * client,
    const char * path,
    const struct usfs_setattr_in * request,
    struct usfs_open_file * file_info
)
{
    if (request->valid & USFS_SET_MODE)
    {
        const int rc = client->ops.chmod (get_callback_request (), path, (mode_t)request->mode, file_info);

        if (rc != 0)
            return rc;
    }

    const int rc = apply_requested_owner (client, path, request, file_info);

    if (rc != 0)
        return rc;

    if (request->valid & USFS_SET_SIZE)
    {
        const int truncate_error = client->ops.truncate (get_callback_request (), path, (off_t)request->size, file_info);

        if (truncate_error != 0)
            return truncate_error;
    }

    return apply_requested_times (client, path, request, file_info);
}

static int set_attributes_and_reply (const struct request_context * context, const char * path, const struct usfs_setattr_in * request)
{
    struct usfs_client * client = context->client;

    struct usfs_open_file file_info;
    struct usfs_open_file * file_info_pointer = NULL;

    if (request->fh != 0)
    {
        memset (&file_info, 0, sizeof (file_info));
        file_info.value = backend_handle_value (client, context->header->nodeid, request->fh);
        file_info_pointer = &file_info;
    }

    if (!has_requested_setattr_callbacks (client, request->valid))
        return send_reply (context, EROFS, NULL, 0);

    const int rc = apply_requested_attributes (client, path, request, file_info_pointer);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    return send_reply (context, 0, NULL, 0);
}

static int handle_setattr_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_setattr_in setattr_request;

    if (payload_length < sizeof (setattr_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&setattr_request, payload, sizeof (setattr_request));

    if (setattr_request.valid & USFS_SET_CTIME)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    struct resolved_handle_path resolved = { 0 };
    const int rc = resolve_node_handle_path (client, request_header->nodeid, setattr_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    omit_handle_callback_path (client, setattr_request.fh != 0, &resolved);
    return set_attributes_and_reply (context, resolved.path, &setattr_request);
}

static int parse_symlink_request (const char * payload, const uint32_t payload_length, const char ** link_name, const char ** target)
{
    struct usfs_symlink_in request;

    if (payload_length < sizeof (request))
        return EINVAL;

    memcpy (&request, payload, sizeof (request));
    const uint32_t names_length = payload_length - (uint32_t)sizeof (request);

    *link_name = payload + sizeof (request);

    if (request.namelen > names_length)
        return EINVAL;

    if (!is_payload_null_terminated (*link_name, request.namelen))
        return EINVAL;

    *target = *link_name + request.namelen;

    if (!is_payload_null_terminated (*target, names_length - request.namelen))
        return EINVAL;

    return 0;
}

static int handle_symlink_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    const char * link_name;
    const char * target;

    int rc = parse_symlink_request (payload, payload_length, &link_name, &target);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (client->ops.symlink == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };

    rc = build_child_path (client, request_header->nodeid, link_name, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = client->ops.symlink (get_callback_request (), target, path.data);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    invalidate_node_directory_cache (client, request_header->nodeid);

    return send_reply (context, 0, NULL, 0);
}

static int validate_link_source (struct usfs_client * client, struct client_node * node, const char * path)
{
    struct stat metadata;
    int rc = read_path_attributes (client, path, &metadata);

    if (rc != 0)
        return -rc;

    if (!is_backend_stat_valid (&metadata))
        return EIO;

    if (metadata.st_ino == 0)
        return EIO;

    if (metadata.st_nlink == 0)
        return EIO;

    if (S_ISDIR (metadata.st_mode))
        return EIO;

    pthread_mutex_lock (&client->node_lock);
    rc = update_node_backend_identity (client, node, &metadata);
    pthread_mutex_unlock (&client->node_lock);

    return rc;
}

struct link_operation
{
    const struct usfs_link_in * request; // Borrowed decoded link request.
    const char * name;                   // Borrowed destination name in the request payload.
    struct client_node * node;           // Source node retained by the request dispatcher.
    struct path_buffer old_path;         // Resolved source backend path.
    struct path_buffer new_path;         // Resolved destination backend path.
};

static int link_prepared_name (const struct request_context * context, const struct link_operation * operation)
{
    struct usfs_client * client = context->client;

    struct client_alias * prepared = prepare_alias (operation->request->newparent, operation->name);

    if (prepared == NULL)
        return send_reply (context, ENOMEM, NULL, 0);

    const int rc = client->ops.link (get_callback_request (), operation->old_path.data, operation->new_path.data);
    if (rc != 0)
    {
        free (prepared->name);
        free (prepared);

        return send_reply (context, -rc, NULL, 0);
    }

    pthread_mutex_lock (&client->node_lock);
    attach_alias (client, operation->node, prepared);
    operation->node->backend_links++;
    pthread_mutex_unlock (&client->node_lock);

    invalidate_node_directory_cache (client, operation->request->newparent);

    return send_reply (context, 0, NULL, 0);
}

static int select_link_source_locked (const struct usfs_client * client, const uint64_t nodeid, struct link_operation * operation)
{
    operation->node = find_node_by_id (client, nodeid);

    if (operation->node == NULL)
        return ENOENT;

    if (operation->node->detached)
        return ENOENT;

    if (find_alias (client, operation->request->newparent, operation->name) != NULL)
        return EEXIST;

    return 0;
}

static int create_link_and_reply (const struct request_context * context, const struct usfs_link_in * request, const char * name)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct link_operation operation = { 0 };

    operation.request = request;
    operation.name = name;

    pthread_mutex_lock (&client->node_lock);
    int rc = select_link_source_locked (client, request_header->nodeid, &operation);

    pthread_mutex_unlock (&client->node_lock);

    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    rc = build_node_path (client, request_header->nodeid, operation.old_path.data, sizeof (operation.old_path.data));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = build_child_path (client, request->newparent, name, &operation.new_path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = validate_link_source (client, operation.node, operation.old_path.data);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return link_prepared_name (context, &operation);
}

static int handle_link_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;

    struct usfs_link_in request;

    if (payload_length < sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&request, payload, sizeof (request));
    const char * name = payload + sizeof (request);

    if (!is_payload_null_terminated (name, payload_length - (uint32_t)sizeof (request)))
        return send_reply (context, EINVAL, NULL, 0);

    if (client->ops.link == NULL)
        return send_reply (context, EROFS, NULL, 0);

    return create_link_and_reply (context, &request, name);
}

static int open_backend_handle (const struct usfs_client * client, const char * path, const uint32_t is_directory, struct usfs_open_file * file_info)
{
    if (is_directory)
    {
        if (client->ops.opendir == NULL)
            return 0;

        return client->ops.opendir (get_callback_request (), path, file_info);
    }

    if (client->ops.open == NULL)
        return 0;

    return client->ops.open (get_callback_request (), path, file_info);
}

static int open_prepared_handle (
    const struct request_context * context,
    const char * path,
    struct open_handle_state * opened,
    struct client_handle * handle
)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    const int rc = open_backend_handle (client, path, opened->is_directory, &opened->file_info);
    if (rc != 0)
    {
        free (handle);

        return send_reply (context, -rc, NULL, 0);
    }

    opened->nodeid = request_header->nodeid;
    publish_client_handle (client, handle, opened);

    struct usfs_open_out reply = { 0 };

    reply.fh = opened->wire_fh;

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int handle_open_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_open_in open_request;

    if (payload_length < sizeof (open_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&open_request, payload, sizeof (open_request));

    char path[USFS_PATH_MAX];
    const int rc = build_node_path (client, request_header->nodeid, path, sizeof (path));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct open_handle_state opened = { 0 };

    opened.file_info.open_flags = translate_open_flags (open_request.flags);

    if (opened.file_info.open_flags < 0)
        return send_reply (context, -opened.file_info.open_flags, NULL, 0);

    struct client_handle * handle = calloc (1, sizeof (*handle));

    if (handle == NULL)
        return send_reply (context, ENOMEM, NULL, 0);

    opened.is_directory = open_request.isdir;

    return open_prepared_handle (context, path, &opened, handle);
}

static int read_file_and_reply (const struct request_context * context, const char * path, const struct usfs_read_in * request, char * data)
{
    struct usfs_client * client = context->client;
    struct usfs_open_file file_info = { 0 };

    file_info.open_flags = O_RDONLY;
    file_info.value = backend_handle_value (client, context->header->nodeid, request->fh);
    memset (data, 0, request->size);

    const ssize_t transferred = client->ops.read (get_callback_request (), path, data, (size_t)request->size, (off_t)request->offset, &file_info);
    if (transferred < 0)
    {
        const int error = transferred < -INT_MAX ? EIO : (int)-transferred;
        return send_reply (context, error, NULL, 0);
    }

    if ((uint64_t)transferred > request->size)
        return send_reply (context, EIO, NULL, 0);

    return send_reply (context, 0, data, (uint32_t)transferred);
}

static int handle_read_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_read_in read_request;
    char * data = get_reply_buffer (client) + sizeof (struct usfs_reply_header);

    if (payload_length < sizeof (read_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&read_request, payload, sizeof (read_request));

    if (read_request.size > USFS_MAX_DATA)
        read_request.size = USFS_MAX_DATA;

    struct resolved_handle_path resolved = { 0 };
    const int rc = resolve_node_handle_path (client, request_header->nodeid, read_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (client->ops.read == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    omit_handle_callback_path (client, true, &resolved);
    return read_file_and_reply (context, resolved.path, &read_request, data);
}

static int resolve_append_position (
    const struct usfs_client * client,
    const char * path,
    struct usfs_write_in * request,
    struct usfs_open_file * file_info
)
{
    if ((request->flags & USFS_WRITE_APPEND) == 0)
        return 0;

    file_info->open_flags |= O_APPEND;
    struct stat metadata;
    const int rc = read_backend_attributes (client, path, &metadata, file_info);

    if (rc != 0)
        return -rc;

    if (!is_backend_stat_valid (&metadata))
        return EIO;

    request->offset = (uint64_t)metadata.st_size;

    return 0;
}

static int prepare_write_position (
    const struct usfs_client * client,
    const char * path,
    struct usfs_write_in * request,
    struct usfs_open_file * file_info
)
{
    const int rc = resolve_append_position (client, path, request, file_info);

    if (rc != 0)
        return rc;

    if (request->offset > (uint64_t)INT64_MAX)
        return EFBIG;

    if (!usfs_file_offset_count_valid ((int64_t)request->offset, request->size))
        return EFBIG;

    return 0;
}

static int write_file_and_reply (const struct request_context * context, const char * path, struct usfs_write_in * request, const char * data)
{
    struct usfs_client * client = context->client;
    struct usfs_open_file file_info = { 0 };

    file_info.open_flags = O_WRONLY;
    file_info.value = backend_handle_value (client, context->header->nodeid, request->fh);

    const int position_error = prepare_write_position (client, path, request, &file_info);
    if (position_error != 0)
        return send_reply (context, position_error, NULL, 0);

    const ssize_t transferred = client->ops.write (get_callback_request (), path, data, (size_t)request->size, (off_t)request->offset, &file_info);
    if (transferred < 0)
    {
        const int error = transferred < -INT_MAX ? EIO : (int)-transferred;
        return send_reply (context, error, NULL, 0);
    }

    if ((uint64_t)transferred > request->size)
        return send_reply (context, EIO, NULL, 0);

    struct usfs_write_out reply = { 0 };
    reply.written = (uint32_t)transferred;
    reply.offset = request->offset;

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int handle_write_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_write_in request;

    if (payload_length < sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&request, payload, sizeof (request));

    if (request.size > payload_length - (uint32_t)sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    const char * data = payload + sizeof (request);

    if (client->ops.write == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct resolved_handle_path resolved = { 0 };
    const int rc = resolve_node_handle_path (client, request_header->nodeid, request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    omit_handle_callback_path (client, true, &resolved);
    return write_file_and_reply (context, resolved.path, &request, data);
}

static int client_handle_has_path (const struct client_handle * handle, const struct client_node * node)
{
    if (handle->nodeid == USFS_ROOT_ID)
        return true;

    if (node == NULL)
        return false;

    return !node->detached || node->hidden_names != 0;
}

static int cleanup_hidden_alias (struct usfs_client * client, struct client_alias * alias, const int shutdown)
{
    struct path_buffer path = { 0 };
    int rc = build_child_path (client, alias->parent, alias->name, &path);

    if (rc == 0)
        rc = client->ops.unlink != NULL ? client->ops.unlink (get_callback_request (), path.data) : -ENOSYS;

    if (rc != 0 && rc != -ENOENT)
    {
        const int error = normalize_callback_error (rc);

        emit_diagnostic (
            client,
            "Failed to remove owned hidden file %s: %s%s\n",
            path.data[0] != '\0' ? path.data : alias->name,
            strerror (error),
            shutdown ? "; shutdown cleanup unresolved" : "; retained for shutdown retry"
        );
        alias->cleanup_attempted = true;

        return error;
    }

    const uint64_t parent = alias->parent;

    pthread_mutex_lock (&client->node_lock);
    remove_alias (client, alias);
    pthread_mutex_unlock (&client->node_lock);
    invalidate_node_directory_cache (client, parent);

    return 0;
}

/* Called with exclusive namespace admission, or after all workers have stopped. */
static int cleanup_hidden_aliases (struct usfs_client * client, struct client_node * node, const int shutdown)
{
    if (node->open_refs != 0)
        return 0;

    int first_error = 0;

    for (struct client_alias * alias = node->aliases; alias != NULL;)
    {
        struct client_alias * next = alias->next;

        if (alias->attached && alias->managed_hidden && (shutdown || !alias->cleanup_attempted))
        {
            const int error = cleanup_hidden_alias (client, alias, shutdown);
            if (first_error == 0)
                first_error = error;
        }

        alias = next;
    }

    return first_error;
}

static void release_backend_handle (const struct usfs_client * client, const struct client_handle * handle, const char * path)
{
    struct usfs_open_file file_info = { 0 };

    file_info.open_flags = handle->flags;
    file_info.value = handle->fh;

    if (handle->isdir)
    {
        if (client->ops.releasedir != NULL)
            client->ops.releasedir (get_callback_request (), path, &file_info);

        return;
    }

    if (client->ops.release != NULL)
        client->ops.release (get_callback_request (), path, &file_info);
}

static void dispose_client_handle (struct usfs_client * client, struct client_handle * handle)
{
    pthread_mutex_lock (&client->dircache_lock);

    if (handle->isdir)
        invalidate_handle_directory_cache_locked (client, handle->nodeid, handle->wire_fh);

    pthread_mutex_unlock (&client->dircache_lock);

    char path_buffer[USFS_PATH_MAX];
    const char * path = NULL;

    pthread_mutex_lock (&client->node_lock);
    struct client_node * node = find_node_by_id (client, handle->nodeid);

    if (client_handle_has_path (handle, node) && build_node_path_locked (client, handle->nodeid, path_buffer, sizeof (path_buffer)) == 0)
        path = path_buffer;

    pthread_mutex_unlock (&client->node_lock);
    if ((client->behavior.handle_paths == USFS_PATH_OMIT_FOR_HANDLE))
        path = NULL;

    release_backend_handle (client, handle, path);

    pthread_mutex_lock (&client->node_lock);

    if (node != NULL)
    {
        node->open_refs--;
        queue_node_for_collection (client, node);
    }

    pthread_mutex_unlock (&client->node_lock);
    if (node != NULL && node->hidden_names != 0)
        cleanup_hidden_aliases (client, node, false);

    free (handle);
}

static int client_handle_matches_release (
    const struct client_handle * handle,
    const uint64_t nodeid,
    const struct usfs_release_in * request,
    const int flags
)
{
    if (handle->nodeid != nodeid)
        return false;

    if (handle->wire_fh != request->fh)
        return false;

    if (handle->isdir != (int)request->isdir)
        return false;

    return handle->flags == flags;
}

static struct client_handle * detach_released_handle (
    struct usfs_client * client,
    const uint64_t nodeid,
    const struct usfs_release_in * request,
    const int flags
)
{
    pthread_mutex_lock (&client->node_lock);
    struct client_handle ** cursor = &client->handles;

    while (*cursor != NULL)
    {
        if (client_handle_matches_release (*cursor, nodeid, request, flags))
            break;

        cursor = &(*cursor)->next;
    }

    struct client_handle * handle = *cursor;

    if (handle != NULL)
        *cursor = handle->next;

    pthread_mutex_unlock (&client->node_lock);

    return handle;
}

static int handle_release_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_release_in request;

    if (payload_length != sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&request, payload, sizeof (request));
    const int flags = translate_open_flags (request.flags);

    if (flags < 0)
        return send_reply (context, EINVAL, NULL, 0);

    if (request.isdir > 1)
        return send_reply (context, EINVAL, NULL, 0);

    struct client_handle * handle = detach_released_handle (client, request_header->nodeid, &request, flags);

    if (handle == NULL)
        return send_reply (context, EINVAL, NULL, 0);

    dispose_client_handle (client, handle);

    return send_reply (context, 0, NULL, 0);
}

static int handle_flush_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_flush_in flush_request;

    if (payload_length != sizeof (flush_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&flush_request, payload, sizeof (flush_request));

    if (flush_request.pad != 0)
        return send_reply (context, EINVAL, NULL, 0);

    struct resolved_handle_path resolved = { 0 };
    int rc = resolve_node_handle_path (client, request_header->nodeid, flush_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (client->ops.flush == NULL)
        return send_reply (context, 0, NULL, 0);

    struct usfs_open_file file_info = { 0 };

    file_info.open_flags = translate_open_flags (flush_request.flags);

    if (file_info.open_flags < 0)
        return send_reply (context, -file_info.open_flags, NULL, 0);

    file_info.value = backend_handle_value (client, request_header->nodeid, flush_request.fh);
    omit_handle_callback_path (client, true, &resolved);
    rc = client->ops.flush (get_callback_request (), resolved.path, &file_info);

    return send_reply (context, normalize_callback_error (rc), NULL, 0);
}

static int is_fsync_request_valid (const struct usfs_fsync_in * request)
{
    const uint32_t allowed_flags = USFS_FSYNC_DATASYNC | USFS_FSYNC_RANGE | USFS_FSYNC_DIRECTORY;

    if ((request->flags & ~allowed_flags) != 0)
        return false;

    if (request->pad != 0)
        return false;

    if ((request->flags & USFS_FSYNC_RANGE) == 0)
    {
        if (request->offset != 0)
            return false;

        return request->length == 0;
    }

    if (request->offset > (uint64_t)INT64_MAX)
        return false;

    if (request->length > (uint64_t)INT64_MAX)
        return false;

    if (request->length == 0)
        return true;

    return request->offset <= (uint64_t)INT64_MAX - request->length;
}

static int parse_fsync_request (const char * payload, const uint32_t payload_length, struct usfs_fsync_in * request)
{
    if (payload_length != sizeof (*request))
        return EINVAL;

    memcpy (request, payload, sizeof (*request));

    if (!is_fsync_request_valid (request))
        return EINVAL;

    return 0;
}

static int sync_backend_handle (const struct request_context * context, const char * path, const struct usfs_fsync_in * request)
{
    struct usfs_client * client = context->client;
    struct usfs_open_file file_info = { 0 };

    file_info.value = backend_handle_value (client, context->header->nodeid, request->fh);

    const int datasync = (request->flags & USFS_FSYNC_DATASYNC) != 0;

    if ((request->flags & USFS_FSYNC_DIRECTORY) != 0)
    {
        if (client->ops.fsyncdir == NULL)
            return -ENOSYS;

        return client->ops.fsyncdir (get_callback_request (), path, datasync, &file_info);
    }

    if (client->ops.fsync == NULL)
        return -ENOSYS;

    return client->ops.fsync (get_callback_request (), path, datasync, &file_info);
}

static int handle_fsync_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_fsync_in fsync_request;
    int rc = parse_fsync_request (payload, payload_length, &fsync_request);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    struct resolved_handle_path resolved = { 0 };

    rc = resolve_node_handle_path (client, request_header->nodeid, fsync_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    omit_handle_callback_path (client, true, &resolved);
    rc = sync_backend_handle (context, resolved.path, &fsync_request);

    return send_reply (context, normalize_callback_error (rc), NULL, 0);
}

static int handle_syncfs_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;

    struct usfs_syncfs_in syncfs_request;

    if (payload_length != sizeof (syncfs_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&syncfs_request, payload, sizeof (syncfs_request));

    if (syncfs_request.pad != 0 || syncfs_request.mode > USFS_SYNCFS_QUIESCE)
        return send_reply (context, EINVAL, NULL, 0);

    if (client->ops.syncfs == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    const int rc = client->ops.syncfs (get_callback_request (), "/");

    return send_reply (context, normalize_callback_error (rc), NULL, 0);
}

/* The callback constructs one snapshot before it becomes visible to readers. */
struct usfs_directory_sink
{
    struct usfs_client * client; // Daemon that owns temporary-storage accounting.
    char * data;                 // In-memory serialized entries until spilling.
    size_t len;                  // Number of serialized bytes.
    size_t cap;                  // Allocated memory capacity.
    uint64_t auto_ino;           // Next synthetic inode number.
    uint32_t count;              // Number of serialized entries.
    int spool_fd;                // Unlinked temporary file when spooled.
    size_t * checkpoints;        // Byte offset of every indexed entry.
    size_t checkpoint_count;     // Number of populated checkpoint offsets.
    size_t checkpoint_cap;       // Allocated checkpoint capacity.
    size_t spool_reserved;       // Bytes charged to the daemon's spool quota.
    bool spooled;                // Whether spool_fd owns a temporary file.
    int error;                   // First directory fill error.
};

static int directory_cache_keys_match (const struct client_dircache_key * left, const struct client_dircache_key * right)
{
    if (left->nodeid != right->nodeid)
        return false;

    if (left->fh != right->fh)
        return false;

    if (left->generation != right->generation)
        return false;

    if (left->uid != right->uid)
        return false;

    if (left->gid != right->gid)
        return false;

    if (left->pid != right->pid)
        return false;

    return true;
}

static uint64_t get_directory_handle_generation (struct usfs_client * client, const uint64_t nodeid, const uint64_t fh)
{
    uint64_t generation = 0;
    unsigned matches = 0;

    if (fh == 0)
        return 0;

    pthread_mutex_lock (&client->node_lock);

    for (const struct client_handle * handle = client->handles; handle != NULL; handle = handle->next)
    {
        if (handle->isdir && handle->nodeid == nodeid && handle->wire_fh == fh)
        {
            generation = handle->generation;
            ++matches;
        }
    }

    pthread_mutex_unlock (&client->node_lock);

    return matches == 1 ? generation : 0;
}

static struct client_dircache * find_directory_cache_entry (
    struct usfs_client * client,
    const struct client_dircache_key * key,
    const uint64_t snapshot_id
)
{
    for (int slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; slot_index++)
        if (client->dircache[slot_index].snapshot_id == snapshot_id && directory_cache_keys_match (&client->dircache[slot_index].key, key))
            return &client->dircache[slot_index];

    return NULL;
}

static void release_directory_cache_resources (struct usfs_client * client, struct client_dircache * slot)
{
    if (slot->owns_node_ref)
    {
        pthread_mutex_lock (&client->node_lock);

        struct client_node * node = find_node_by_id (client, slot->key.nodeid);
        if (node != NULL)
        {
            node->cache_refs--;
            queue_node_for_collection (client, node);
        }

        pthread_mutex_unlock (&client->node_lock);
    }

    free (slot->data);
    client->dircache_ram_bytes -= slot->data_capacity;

    if (slot->spooled)
        close (slot->spool_fd);

    free (slot->checkpoints);
    client->dircache_spool_bytes -= slot->spool_reserved;
    slot->data = NULL;
    slot->data_capacity = 0;
    slot->checkpoints = NULL;
    slot->checkpoint_count = 0;
    slot->spool_reserved = 0;
    slot->spooled = false;
    slot->owns_node_ref = false;
}

static void clear_directory_cache_slot (struct usfs_client * client, struct client_dircache * slot)
{
    if (slot->snapshot_id == 0)
        return;

    release_directory_cache_resources (client, slot);
    memset (slot, 0, sizeof (*slot));
}

static void invalidate_node_directory_cache (struct usfs_client * client, const uint64_t nodeid)
{
    pthread_mutex_lock (&client->dircache_lock);

    for (int slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; ++slot_index)
        if (client->dircache[slot_index].snapshot_id != 0 && client->dircache[slot_index].key.nodeid == nodeid)
            clear_directory_cache_slot (client, &client->dircache[slot_index]);

    pthread_mutex_unlock (&client->dircache_lock);
}

static void invalidate_handle_directory_cache_locked (struct usfs_client * client, const uint64_t nodeid, const uint64_t fh)
{
    for (int slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; ++slot_index)
    {
        struct client_dircache * slot = &client->dircache[slot_index];

        if (slot->snapshot_id == 0)
            continue;

        if (slot->key.nodeid != nodeid)
            continue;

        if (slot->key.fh != fh)
            continue;

        clear_directory_cache_slot (client, slot);
    }
}

static struct client_dircache * select_directory_cache_slot (struct usfs_client * client)
{
    struct client_dircache * slot = &client->dircache[0];

    for (int slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; slot_index++)
    {
        struct client_dircache * candidate = &client->dircache[slot_index];

        if (candidate->snapshot_id == 0)
        {
            slot = candidate;
            break;
        }

        if (candidate->seq < slot->seq)
            slot = candidate;
    }

    return slot;
}

static int spill_directory_buffer (struct usfs_directory_sink * directory_buffer);

static int publish_directory_snapshot (
    struct usfs_client * client,
    const struct client_dircache_key * key,
    struct usfs_directory_sink * directory_buffer,
    uint64_t * snapshot_id
)
{
    for (;;)
    {
        pthread_mutex_lock (&client->dircache_lock);

        if (key->generation != 0 && get_directory_handle_generation (client, key->nodeid, key->fh) != key->generation)
        {
            pthread_mutex_unlock (&client->dircache_lock);
            return ESTALE;
        }

        if (client->next_snapshot_id >= USFS_DIRECTORY_CURSOR_ID_MAX)
        {
            pthread_mutex_unlock (&client->dircache_lock);
            return EOVERFLOW;
        }

        struct client_dircache * slot = select_directory_cache_slot (client);
        const size_t retained_ram_bytes = client->dircache_ram_bytes - slot->data_capacity;

        if (!directory_buffer->spooled && directory_buffer->cap > CLIENT_DIRCACHE_RAM_MAX - retained_ram_bytes)
        {
            pthread_mutex_unlock (&client->dircache_lock);

            const int spill_rc = spill_directory_buffer (directory_buffer);
            if (spill_rc != 0)
                return spill_rc;

            continue;
        }

        clear_directory_cache_slot (client, slot);

        pthread_mutex_lock (&client->node_lock);
        struct client_node * node = find_node_by_id (client, key->nodeid);
        if (node != NULL)
            node->cache_refs++;
        pthread_mutex_unlock (&client->node_lock);

        slot->key = *key;
        slot->owns_node_ref = node != NULL;
        slot->snapshot_id = ++client->next_snapshot_id;
        slot->data = directory_buffer->data;
        slot->data_capacity = directory_buffer->cap;
        slot->len = directory_buffer->len;
        slot->count = directory_buffer->count;
        slot->spool_fd = directory_buffer->spool_fd;
        slot->checkpoints = directory_buffer->checkpoints;
        slot->checkpoint_count = directory_buffer->checkpoint_count;
        slot->spool_reserved = directory_buffer->spool_reserved;
        slot->spooled = directory_buffer->spooled;
        slot->seq = ++client->dircache_seq;
        client->dircache_ram_bytes += slot->data_capacity;

        *snapshot_id = slot->snapshot_id;
        directory_buffer->data = NULL;
        directory_buffer->cap = 0;
        directory_buffer->checkpoints = NULL;
        directory_buffer->spooled = false;
        directory_buffer->spool_reserved = 0;

        pthread_mutex_unlock (&client->dircache_lock);
        return 0;
    }
}

static void release_directory_buffer (struct usfs_directory_sink * directory_buffer)
{
    free (directory_buffer->data);
    free (directory_buffer->checkpoints);

    if (directory_buffer->spooled)
        close (directory_buffer->spool_fd);

    if (directory_buffer->spool_reserved != 0)
    {
        pthread_mutex_lock (&directory_buffer->client->dircache_lock);
        directory_buffer->client->dircache_spool_bytes -= directory_buffer->spool_reserved;
        pthread_mutex_unlock (&directory_buffer->client->dircache_lock);
    }
}

static int reserve_directory_spool (struct usfs_directory_sink * directory_buffer, const size_t needed)
{
    if (needed > CLIENT_DIRSPOOL_MAX)
        return ENOSPC;

    const size_t rounded = (needed + CLIENT_DIRSPOOL_RESERVATION - 1u) & ~(size_t)(CLIENT_DIRSPOOL_RESERVATION - 1u);

    if (rounded <= directory_buffer->spool_reserved)
        return 0;

    struct usfs_client * client = directory_buffer->client;
    const size_t extra = rounded - directory_buffer->spool_reserved;

    pthread_mutex_lock (&client->dircache_lock);

    while (extra > CLIENT_DIRSPOOL_MAX - client->dircache_spool_bytes)
    {
        struct client_dircache * oldest_handleless = NULL;

        for (int slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; slot_index++)
        {
            struct client_dircache * candidate = &client->dircache[slot_index];

            if (candidate->snapshot_id == 0 || candidate->key.generation != 0 || candidate->spool_reserved == 0)
                continue;

            if (oldest_handleless == NULL || candidate->seq < oldest_handleless->seq)
                oldest_handleless = candidate;
        }

        if (oldest_handleless == NULL)
        {
            pthread_mutex_unlock (&client->dircache_lock);
            return ENOSPC;
        }

        clear_directory_cache_slot (client, oldest_handleless);
    }

    client->dircache_spool_bytes += extra;
    directory_buffer->spool_reserved = rounded;
    pthread_mutex_unlock (&client->dircache_lock);

    return 0;
}

static void clear_directory_cache (struct usfs_client * client)
{
    for (int slot_index = 0; slot_index < CLIENT_DIRCACHE_SLOTS; slot_index++)
    {
        clear_directory_cache_slot (client, &client->dircache[slot_index]);
    }
}

static int create_directory_spool (const struct usfs_client * client, int * spool_fd)
{
    const char * temporary_directory = getenv ("TMPDIR");
    if (temporary_directory == NULL || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";

    char resolved_directory[USFS_PATH_MAX];
    if (realpath (temporary_directory, resolved_directory) == NULL)
        return EIO;

    if (client->mountpoint != NULL)
    {
        const size_t mountpoint_length = strlen (client->mountpoint);

        if (strncmp (resolved_directory, client->mountpoint, mountpoint_length) == 0 &&
            (resolved_directory[mountpoint_length] == '\0' || resolved_directory[mountpoint_length] == '/'))
            return EINVAL;
    }

    char spool_path[USFS_PATH_MAX];
    const int path_length = snprintf (spool_path, sizeof (spool_path), "%s/usfs-dir-XXXXXX", resolved_directory);
    if (path_length < 0 || (size_t)path_length >= sizeof (spool_path))
        return ENAMETOOLONG;

    const int fd = mkstemp (spool_path);
    if (fd < 0)
        return EIO;

    int unlink_rc;
    do
    {
        unlink_rc = unlink (spool_path);
    }
    while (unlink_rc != 0 && errno == EINTR);

    if (unlink_rc != 0)
    {
        close (fd);
        unlink (spool_path);
        return EIO;
    }

    if (fcntl (fd, F_SETFD, FD_CLOEXEC) != 0)
    {
        close (fd);
        return EIO;
    }

    *spool_fd = fd;
    return 0;
}

static int write_directory_spool (const int fd, const void * data, const size_t length, const size_t offset)
{
    const char * bytes = data;
    size_t written = 0;

    while (written < length)
    {
        const ssize_t count = pwrite (fd, bytes + written, length - written, (off_t)(offset + written));

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return EIO;

        written += (size_t)count;
    }

    return 0;
}

static int read_directory_spool (const int fd, void * data, const size_t length, const size_t offset)
{
    char * bytes = data;
    size_t received = 0;

    while (received < length)
    {
        const ssize_t count = pread (fd, bytes + received, length - received, (off_t)(offset + received));

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            return EIO;

        received += (size_t)count;
    }

    return 0;
}

static int append_directory_checkpoint (struct usfs_directory_sink * directory_buffer, const size_t offset)
{
    if (directory_buffer->checkpoint_count == directory_buffer->checkpoint_cap)
    {
        const size_t capacity = directory_buffer->checkpoint_cap == 0 ? 16u : directory_buffer->checkpoint_cap * 2u;

        if (capacity > SIZE_MAX / sizeof (*directory_buffer->checkpoints))
            return ENOMEM;

        size_t * checkpoints = realloc (directory_buffer->checkpoints, capacity * sizeof (*checkpoints));
        if (checkpoints == NULL)
            return ENOMEM;

        directory_buffer->checkpoints = checkpoints;
        directory_buffer->checkpoint_cap = capacity;
    }

    directory_buffer->checkpoints[directory_buffer->checkpoint_count++] = offset;
    return 0;
}

static int spill_directory_buffer (struct usfs_directory_sink * directory_buffer)
{
    if (directory_buffer->client == NULL)
        return ENOSPC;

    int rc = reserve_directory_spool (directory_buffer, directory_buffer->len);
    if (rc != 0)
        return rc;

    rc = create_directory_spool (directory_buffer->client, &directory_buffer->spool_fd);
    if (rc != 0)
        return rc;

    directory_buffer->spooled = true;

    size_t position = 0;
    for (uint32_t entry_index = 0; entry_index < directory_buffer->count; entry_index++)
    {
        if (entry_index % CLIENT_DIRECTORY_INDEX_STRIDE == 0)
        {
            rc = append_directory_checkpoint (directory_buffer, position);
            if (rc != 0)
                return rc;
        }

        struct usfs_dirent entry;
        memcpy (&entry, directory_buffer->data + position, sizeof (entry));
        position += entry.reclen;
    }

    rc = write_directory_spool (directory_buffer->spool_fd, directory_buffer->data, directory_buffer->len, 0);
    if (rc != 0)
        return rc;

    free (directory_buffer->data);
    directory_buffer->data = NULL;
    directory_buffer->cap = 0;

    return 0;
}

static int reserve_directory_buffer (struct usfs_directory_sink * directory_buffer, const size_t needed)
{
    if (needed > CLIENT_DIRCACHE_MAX)
        return 1;

    if (needed <= directory_buffer->cap)
        return 0;

    size_t new_capacity = directory_buffer->cap ? directory_buffer->cap : CLIENT_DIRBUF_INITIAL_CAPACITY;

    while (new_capacity < needed)
    {
        if (new_capacity > SIZE_MAX / CLIENT_DIRBUF_GROWTH_FACTOR)
        {
            new_capacity = needed;
            break;
        }
        new_capacity *= CLIENT_DIRBUF_GROWTH_FACTOR;
    }

    if (new_capacity > CLIENT_DIRCACHE_MAX)
        new_capacity = CLIENT_DIRCACHE_MAX;

    char * new_data = realloc (directory_buffer->data, new_capacity);

    if (new_data == NULL)
    {
        directory_buffer->error = ENOMEM;
        return 1;
    }

    directory_buffer->data = new_data;
    directory_buffer->cap = new_capacity;

    return 0;
}

int usfs_directory_add (struct usfs_directory_sink * directory_buffer, const char * name, const struct stat * entry_metadata)
{
    if (directory_buffer == NULL)
        return -EINVAL;

    if (name == NULL)
    {
        if (directory_buffer->error == 0)
            directory_buffer->error = EINVAL;

        return -directory_buffer->error;
    }

    struct usfs_dirent directory_entry;

    if (directory_buffer->error != 0)
        return -directory_buffer->error;

    const char * terminator = memchr (name, '\0', USFS_MAX_NAME);
    if (terminator == NULL)
    {
        directory_buffer->error = ENAMETOOLONG;
        return -directory_buffer->error;
    }

    const size_t name_length = (size_t)(terminator - name);

    if (name_length >= USFS_MAX_NAME)
    {
        directory_buffer->error = ENAMETOOLONG;
        return -directory_buffer->error;
    }

    const uint16_t record_length = USFS_DIRENT_SIZE (name_length);

    if (directory_buffer->count >= USFS_DIRECTORY_CURSOR_INDEX_MASK || directory_buffer->len > SIZE_MAX - record_length)
    {
        directory_buffer->error = EOVERFLOW;
        return -directory_buffer->error;
    }

    const size_t needed = directory_buffer->len + record_length;

    if (!directory_buffer->spooled && needed > CLIENT_DIRCACHE_MAX)
    {
        directory_buffer->error = spill_directory_buffer (directory_buffer);
        if (directory_buffer->error != 0)
            return -directory_buffer->error;
    }

    if (directory_buffer->spooled)
    {
        directory_buffer->error = reserve_directory_spool (directory_buffer, needed);
        if (directory_buffer->error != 0)
            return -directory_buffer->error;

        if (directory_buffer->count % CLIENT_DIRECTORY_INDEX_STRIDE == 0)
        {
            directory_buffer->error = append_directory_checkpoint (directory_buffer, directory_buffer->len);
            if (directory_buffer->error != 0)
                return -directory_buffer->error;
        }
    }
    else if (reserve_directory_buffer (directory_buffer, needed) != 0)
        return -directory_buffer->error;

    memset (&directory_entry, 0, sizeof (directory_entry));
    directory_entry.namelen = (uint16_t)name_length;
    directory_entry.reclen = record_length;

    if (entry_metadata != NULL && entry_metadata->st_ino != 0)
    {
        directory_entry.ino = (uint64_t)entry_metadata->st_ino;
        directory_entry.type = ((uint32_t)entry_metadata->st_mode & CLIENT_DIRENT_MODE_MASK) >> CLIENT_DIRENT_TYPE_SHIFT;
    }
    else
    {
        directory_entry.ino = directory_buffer->auto_ino++;
        directory_entry.type = 0;
    }

    char record[USFS_DIRENT_SIZE (USFS_MAX_NAME - 1u)];
    char * destination = directory_buffer->spooled ? record : directory_buffer->data + directory_buffer->len;

    memcpy (destination, &directory_entry, sizeof (directory_entry));
    memset (destination + sizeof (directory_entry), 0, record_length - sizeof (directory_entry));
    memcpy (destination + sizeof (directory_entry), name, name_length);

    if (directory_buffer->spooled)
    {
        directory_buffer->error = write_directory_spool (directory_buffer->spool_fd, record, record_length, directory_buffer->len);
        if (directory_buffer->error != 0)
            return -directory_buffer->error;
    }

    directory_buffer->len += record_length;
    directory_buffer->count += 1;

    return 0;
}

static int load_readdir_snapshot (
    const struct request_context * context,
    const struct usfs_readdir_in * request,
    const char * path,
    const struct client_dircache_key * key,
    uint64_t * snapshot_id
)
{
    struct usfs_client * client = context->client;
    struct usfs_directory_sink directory_buffer = { 0 };
    struct usfs_open_file file_info = { 0 };

    directory_buffer.client = client;
    directory_buffer.auto_ino = CLIENT_SYNTHETIC_INODE_BASE + context->header->nodeid;
    file_info.value = backend_handle_value (client, context->header->nodeid, request->fh);

    const int callback_result = client->ops.readdir (get_callback_request (), path, &file_info, &directory_buffer);

    int rc = directory_buffer.error != 0 ? directory_buffer.error : normalize_callback_error (callback_result);
    if (rc == 0)
        rc = publish_directory_snapshot (client, key, &directory_buffer, snapshot_id);

    release_directory_buffer (&directory_buffer);
    return rc;
}

static int read_directory_entry (const struct client_dircache * snapshot, const size_t position, struct usfs_dirent * entry)
{
    if (position > snapshot->len || snapshot->len - position < sizeof (*entry))
        return EIO;

    if (snapshot->spooled)
    {
        if (read_directory_spool (snapshot->spool_fd, entry, sizeof (*entry), position) != 0)
            return EIO;
    }
    else
        memcpy (entry, snapshot->data + position, sizeof (*entry));

    if (entry->reclen < sizeof (*entry) + entry->namelen + 1u || entry->reclen > snapshot->len - position)
        return EIO;

    return 0;
}

static int find_directory_entry_position (const struct client_dircache * snapshot, const uint32_t start_index, size_t * position)
{
    uint32_t entry_index = 0;
    *position = 0;

    if (snapshot->spooled && start_index != 0)
    {
        const size_t checkpoint_index = start_index / CLIENT_DIRECTORY_INDEX_STRIDE;

        if (checkpoint_index >= snapshot->checkpoint_count)
            return EIO;

        *position = snapshot->checkpoints[checkpoint_index];
        entry_index = (uint32_t)(checkpoint_index * CLIENT_DIRECTORY_INDEX_STRIDE);
    }

    while (entry_index < start_index)
    {
        struct usfs_dirent entry;
        const int rc = read_directory_entry (snapshot, *position, &entry);
        if (rc != 0)
            return rc;

        *position += entry.reclen;
        entry_index++;
    }

    return 0;
}

static int copy_readdir_window (
    const struct client_dircache * snapshot,
    const uint32_t start_index,
    const uint32_t maximum_bytes,
    struct usfs_readdir_out * reply,
    uint32_t * reply_length
)
{
    reply->snapshot_id = snapshot->snapshot_id;
    reply->count = 0;
    reply->pad = 0;

    uint32_t remaining_bytes = maximum_bytes;
    char * destination = (char *)reply + sizeof (*reply);

    if (start_index >= snapshot->count)
    {
        *reply_length = sizeof (*reply);
        return 0;
    }

    size_t position;
    int rc = find_directory_entry_position (snapshot, start_index, &position);
    if (rc != 0)
        return rc;

    for (uint32_t entry_index = start_index; entry_index < snapshot->count; entry_index++)
    {
        struct usfs_dirent entry;
        rc = read_directory_entry (snapshot, position, &entry);
        if (rc != 0)
            return rc;

        if (entry.reclen > remaining_bytes)
        {
            if (reply->count == 0)
                return EINVAL;

            break;
        }

        if (snapshot->spooled)
        {
            rc = read_directory_spool (snapshot->spool_fd, destination, entry.reclen, position);
            if (rc != 0)
                return rc;
        }
        else
            memcpy (destination, snapshot->data + position, entry.reclen);

        destination += entry.reclen;
        position += entry.reclen;
        remaining_bytes -= entry.reclen;
        reply->count++;
    }

    *reply_length = (uint32_t)(sizeof (*reply) + maximum_bytes - remaining_bytes);
    return 0;
}

static int send_readdir_window (
    const struct request_context * context,
    const struct client_dircache_key * key,
    const uint64_t snapshot_id,
    const uint32_t start_index,
    const uint32_t maximum_bytes
)
{
    struct usfs_client * client = context->client;
    struct usfs_readdir_out * reply = (struct usfs_readdir_out *)(get_reply_buffer (client) + sizeof (struct usfs_reply_header));
    uint32_t reply_length = 0;

    pthread_mutex_lock (&client->dircache_lock);

    struct client_dircache * snapshot = find_directory_cache_entry (client, key, snapshot_id);
    if (snapshot == NULL)
    {
        pthread_mutex_unlock (&client->dircache_lock);
        return send_reply (context, ESTALE, NULL, 0);
    }

    if (snapshot->completed && start_index < snapshot->count)
    {
        pthread_mutex_unlock (&client->dircache_lock);
        return send_reply (context, ESTALE, NULL, 0);
    }

    int rc = 0;
    if (snapshot->completed)
    {
        reply->snapshot_id = snapshot_id;
        reply->count = 0;
        reply->pad = 0;
        reply_length = sizeof (*reply);
    }
    else
        rc = copy_readdir_window (snapshot, start_index, maximum_bytes, reply, &reply_length);

    if (rc == EIO)
        clear_directory_cache_slot (client, snapshot);
    else if (rc == 0 && key->generation == 0 && reply->count == 0 && start_index >= snapshot->count)
    {
        release_directory_cache_resources (client, snapshot);
        snapshot->completed = true;
    }

    if (rc != EIO)
        snapshot->seq = ++client->dircache_seq;

    pthread_mutex_unlock (&client->dircache_lock);

    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return send_reply (context, 0, reply, reply_length);
}

static int handle_readdir_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;
    struct usfs_readdir_in readdir_request;

    if (payload_length < sizeof (readdir_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&readdir_request, payload, sizeof (readdir_request));

    if (readdir_request.size == 0 || readdir_request.size > USFS_MAX_DATA)
        readdir_request.size = USFS_MAX_DATA;

    struct resolved_handle_path resolved = { 0 };
    const int path_rc = resolve_node_handle_path (client, request_header->nodeid, readdir_request.fh, &resolved);
    if (path_rc != 0)
        return send_reply (context, -path_rc, NULL, 0);

    if (client->ops.readdir == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    omit_handle_callback_path (client, true, &resolved);
    struct client_dircache_key key = { 0 };
    key.nodeid = request_header->nodeid;
    key.fh = readdir_request.fh;
    key.generation = get_directory_handle_generation (client, key.nodeid, key.fh);
    key.uid = request_header->uid;
    key.gid = request_header->gid;
    key.pid = request_header->pid;

    uint64_t snapshot_id = readdir_request.cookie >> USFS_DIRECTORY_CURSOR_INDEX_BITS;
    const uint32_t start_index = (uint32_t)(readdir_request.cookie & USFS_DIRECTORY_CURSOR_INDEX_MASK);

    if (readdir_request.cookie != 0 && snapshot_id == 0)
        return send_reply (context, EINVAL, NULL, 0);

    if (readdir_request.cookie == 0)
    {
        const int rc = load_readdir_snapshot (context, &readdir_request, resolved.path, &key, &snapshot_id);
        if (rc != 0)
            return send_reply (context, rc, NULL, 0);
    }

    return send_readdir_window (context, &key, snapshot_id, start_index, readdir_request.size);
}

#ifndef USFS_OBJECT_WAITING
    #define USFS_OBJECT_WAITING(nodeid) ((void)(nodeid))
#endif

static int handle_forget_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct usfs_client * client = context->client;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_forget_in body;
    uint64_t * lookup_references;
    int error = 0;

    if (payload_length != sizeof (body))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&body, payload, sizeof (body));
    pthread_mutex_lock (&client->node_lock);

    struct client_node * node = find_node_by_id (client, request_header->nodeid);

    if (request_header->nodeid == USFS_ROOT_ID)
    {
        lookup_references = &client->root_lookup_refs;
    }
    else if (node != NULL)
    {
        lookup_references = &node->lookup_refs;
    }
    else
    {
        lookup_references = NULL;
    }
    if (lookup_references == NULL || !usfs_forget_in_valid (&body, *lookup_references))
    {
        error = EINVAL;
    }
    else
    {
        *lookup_references -= body.count;
        queue_node_for_collection (client, node);
    }

    pthread_mutex_unlock (&client->node_lock);

    return send_reply (context, error, NULL, 0);
}

static struct client_node * retain_request_node (struct usfs_client * client, const uint64_t nodeid)
{
    pthread_mutex_lock (&client->node_lock);
    struct client_node * node = find_node_by_id (client, nodeid);

    if (node != NULL)
        node->operation_refs++;

    pthread_mutex_unlock (&client->node_lock);

    return node;
}

static int lock_request_node_for_mutation (const struct usfs_in_hdr * request_header, struct client_node * retained, struct client_node ** object)
{
    *object = NULL;

    if (request_header->nodeid == USFS_ROOT_ID || (request_header->opcode != USFS_OP_WRITE && request_header->opcode != USFS_OP_SETATTR))
        return 0;

    if (retained == NULL)
        return ESTALE;

    int error = pthread_mutex_trylock (&retained->mutation_lock);

    if (error == EBUSY)
    {
        USFS_OBJECT_WAITING (request_header->nodeid);
        error = pthread_mutex_lock (&retained->mutation_lock);
    }

    if (error == 0)
        *object = retained;

    return error;
}

static void dispatch_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    const struct usfs_in_hdr * request_header = context->header;

    switch (request_header->opcode)
    {
        case USFS_OP_FORGET:
            handle_forget_request (context, payload, payload_length);
            break;
        case USFS_OP_LOOKUP:
            handle_lookup_request (context, payload, payload_length);
            break;
        case USFS_OP_FID:
            handle_fid_request (context, payload_length);
            break;
        case USFS_OP_VGET:
            handle_vget_request (context, payload, payload_length);
            break;
        case USFS_OP_GETATTR:
            handle_getattr_request (context, payload, payload_length);
            break;
        case USFS_OP_OPEN:
            handle_open_request (context, payload, payload_length);
            break;
        case USFS_OP_READ:
            handle_read_request (context, payload, payload_length);
            break;
        case USFS_OP_RELEASE:
            handle_release_request (context, payload, payload_length);
            break;
        case USFS_OP_READDIR:
            handle_readdir_request (context, payload, payload_length);
            break;
        case USFS_OP_READLINK:
            handle_readlink_request (context);
            break;
        case USFS_OP_STATFS:
            handle_statfs_request (context);
            break;
        case USFS_OP_CREATE:
            handle_create_request (context, payload, payload_length);
            break;
        case USFS_OP_CREATE_ATTR:
            handle_create_attr_request (context, payload, payload_length);
            break;
        case USFS_OP_MKDIR:
            handle_mkdir_request (context, payload, payload_length);
            break;
        case USFS_OP_WRITE:
            handle_write_request (context, payload, payload_length);
            break;
        case USFS_OP_UNLINK:
            handle_unlink_request (context, payload, payload_length);
            break;
        case USFS_OP_RMDIR:
            handle_rmdir_request (context, payload, payload_length);
            break;
        case USFS_OP_RENAME:
            handle_rename_request (context, payload, payload_length);
            break;
        case USFS_OP_SYMLINK:
            handle_symlink_request (context, payload, payload_length);
            break;
        case USFS_OP_LINK:
            handle_link_request (context, payload, payload_length);
            break;
        case USFS_OP_SETATTR:
            handle_setattr_request (context, payload, payload_length);
            break;
        case USFS_OP_FLUSH:
            handle_flush_request (context, payload, payload_length);
            break;
        case USFS_OP_FSYNC:
            handle_fsync_request (context, payload, payload_length);
            break;
        case USFS_OP_SYNCFS:
            handle_syncfs_request (context, payload, payload_length);
            break;
        default:
            send_reply (context, ENOSYS, NULL, 0);
            break;
    }
}

static void finish_request (struct usfs_client * client, struct client_node * object, struct client_node * retained, const int exclusive)
{
    if (object != NULL)
        pthread_mutex_unlock (&object->mutation_lock);

    pthread_mutex_lock (&client->node_lock);

    if (retained != NULL)
    {
        retained->operation_refs--;
        queue_node_for_collection (client, retained);
    }

    collect_unreferenced_nodes (client);
    pthread_mutex_unlock (&client->node_lock);
    release_namespace_access (client, exclusive);
}

static int release_requires_namespace_writer (struct usfs_client * client, const struct usfs_in_hdr * request, const struct client_node * node)
{
    if (request->opcode != USFS_OP_RELEASE || node == NULL)
        return false;

    pthread_mutex_lock (&client->node_lock);
    const int hidden = node->hidden_names != 0;

    pthread_mutex_unlock (&client->node_lock);

    return hidden;
}

static void handle_request (struct usfs_client * client, const char * message, const size_t message_length)
{
    struct client_context_scope scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };
    struct usfs_in_hdr request_header;
    struct client_node * object = NULL;
    struct client_node * retained = NULL;
    const char * payload = message + sizeof (request_header);

    if (message_length < sizeof (request_header))
    {
        fail_session (client, EIO);
        return;
    }

    memcpy (&request_header, message, sizeof (request_header));

    if (request_header.version != USFS_PROTOCOL_VERSION || request_header.len > message_length || request_header.len < sizeof (request_header))
    {
        fail_session (client, EIO);
        return;
    }

    struct request_context context = { 0 };

    context.client = client;
    context.header = &request_header;

    const uint32_t payload_length = request_header.len - (uint32_t)sizeof (request_header);

    int error = enter_callback_context (client, &scope);
    if (error != 0)
    {
        fail_session (client, error);
        return;
    }

    set_request_context (client, &request_header);
    int exclusive = request_mutates_namespace (request_header.opcode);

    error = acquire_namespace_access (client, exclusive);
    if (error != 0)
    {
        send_reply (&context, error, NULL, 0);
        return;
    }

    retained = retain_request_node (client, request_header.nodeid);
    if (!exclusive && release_requires_namespace_writer (client, &request_header, retained))
    {
        release_namespace_access (client, false);
        exclusive = true;
        error = acquire_namespace_access (client, exclusive);

        if (error != 0)
        {
            pthread_mutex_lock (&client->node_lock);
            retained->operation_refs--;
            queue_node_for_collection (client, retained);
            pthread_mutex_unlock (&client->node_lock);
            send_reply (&context, error, NULL, 0);
            return;
        }
    }

    error = lock_request_node_for_mutation (&request_header, retained, &object);
    if (error != 0)
        send_reply (&context, error, NULL, 0);
    else
        dispatch_request (&context, payload, payload_length);

    finish_request (client, object, retained, exclusive);
}

/* ----------------------------------------------------------- *
 * Lifecycle                                                   *
 * ----------------------------------------------------------- */

static void destroy_session_synchronization (struct usfs_client * client)
{
    pthread_cond_destroy (&client->namespace.changed);
    pthread_mutex_destroy (&client->namespace.lock);
    pthread_mutex_destroy (&client->reply_lock);
    pthread_mutex_destroy (&client->dircache_lock);
    pthread_mutex_destroy (&client->node_lock);
}

static int initialize_session_synchronization (struct usfs_client * client)
{
    int error = pthread_mutex_init (&client->node_lock, NULL);
    if (error != 0)
        return -error;

    error = pthread_mutex_init (&client->dircache_lock, NULL);
    if (error != 0)
    {
        pthread_mutex_destroy (&client->node_lock);
        return -error;
    }

    error = pthread_mutex_init (&client->reply_lock, NULL);
    if (error != 0)
    {
        pthread_mutex_destroy (&client->dircache_lock);
        pthread_mutex_destroy (&client->node_lock);
        return -error;
    }

    error = pthread_mutex_init (&client->namespace.lock, NULL);
    if (error != 0)
    {
        pthread_mutex_destroy (&client->reply_lock);
        pthread_mutex_destroy (&client->dircache_lock);
        pthread_mutex_destroy (&client->node_lock);
        return -error;
    }

    error = pthread_cond_init (&client->namespace.changed, NULL);
    if (error != 0)
    {
        pthread_mutex_destroy (&client->namespace.lock);
        pthread_mutex_destroy (&client->reply_lock);
        pthread_mutex_destroy (&client->dircache_lock);
        pthread_mutex_destroy (&client->node_lock);
        return -error;
    }

    return 0;
}

static void initialize_connection_info (struct usfs_client * client)
{
    client->limits.max_write = USFS_MAX_DATA;
    client->limits.max_read = USFS_MAX_DATA;
}

static int initialize_message_io (struct usfs_client * client)
{
    client->readbuf = malloc (USFS_MSG_MAX);
    client->writebuf = malloc (USFS_MSG_MAX);
    if (client->readbuf == NULL || client->writebuf == NULL)
    {
        free (client->readbuf);
        free (client->writebuf);
        return -ENOMEM;
    }

    if (create_session_wake_pipe (&client->session) == 0)
        return 0;

    const int error = current_system_error ();
    free (client->readbuf);
    free (client->writebuf);

    return -error;
}

static int is_behavior_valid (const struct usfs_behavior * behavior)
{
    if (behavior->remove_policy != USFS_REMOVE_DEFERRED && behavior->remove_policy != USFS_REMOVE_IMMEDIATE)
        return false;

    return behavior->handle_paths == USFS_PATH_DEFAULT || behavior->handle_paths == USFS_PATH_OMIT_FOR_HANDLE;
}

static int are_client_options_valid (const struct usfs_client_options * options)
{
    if (!is_behavior_valid (&options->behavior))
        return false;

    switch (options->mount_access)
    {
        case USFS_MOUNT_AUTOMATIC:
        case USFS_MOUNT_READ_ONLY:
        case USFS_MOUNT_READ_WRITE:
            return true;
        default:
            return false;
    }
}

static int is_operations_size_valid (const size_t operations_size)
{
    static const size_t complete_members[] = { 0,
                                               offsetof (struct usfs_operations, initialize) + sizeof (((struct usfs_operations *)0)->initialize),
                                               offsetof (struct usfs_operations, shutdown) + sizeof (((struct usfs_operations *)0)->shutdown),
                                               offsetof (struct usfs_operations, getattr) + sizeof (((struct usfs_operations *)0)->getattr),
                                               offsetof (struct usfs_operations, statfs) + sizeof (((struct usfs_operations *)0)->statfs),
                                               offsetof (struct usfs_operations, readlink) + sizeof (((struct usfs_operations *)0)->readlink),
                                               offsetof (struct usfs_operations, chmod) + sizeof (((struct usfs_operations *)0)->chmod),
                                               offsetof (struct usfs_operations, chown) + sizeof (((struct usfs_operations *)0)->chown),
                                               offsetof (struct usfs_operations, truncate) + sizeof (((struct usfs_operations *)0)->truncate),
                                               offsetof (struct usfs_operations, utimens) + sizeof (((struct usfs_operations *)0)->utimens),
                                               offsetof (struct usfs_operations, mkdir) + sizeof (((struct usfs_operations *)0)->mkdir),
                                               offsetof (struct usfs_operations, rmdir) + sizeof (((struct usfs_operations *)0)->rmdir),
                                               offsetof (struct usfs_operations, unlink) + sizeof (((struct usfs_operations *)0)->unlink),
                                               offsetof (struct usfs_operations, rename) + sizeof (((struct usfs_operations *)0)->rename),
                                               offsetof (struct usfs_operations, link) + sizeof (((struct usfs_operations *)0)->link),
                                               offsetof (struct usfs_operations, symlink) + sizeof (((struct usfs_operations *)0)->symlink),
                                               offsetof (struct usfs_operations, create) + sizeof (((struct usfs_operations *)0)->create),
                                               offsetof (struct usfs_operations, create_attr) + sizeof (((struct usfs_operations *)0)->create_attr),
                                               offsetof (struct usfs_operations, open) + sizeof (((struct usfs_operations *)0)->open),
                                               offsetof (struct usfs_operations, read) + sizeof (((struct usfs_operations *)0)->read),
                                               offsetof (struct usfs_operations, write) + sizeof (((struct usfs_operations *)0)->write),
                                               offsetof (struct usfs_operations, flush) + sizeof (((struct usfs_operations *)0)->flush),
                                               offsetof (struct usfs_operations, fsync) + sizeof (((struct usfs_operations *)0)->fsync),
                                               offsetof (struct usfs_operations, release) + sizeof (((struct usfs_operations *)0)->release),
                                               offsetof (struct usfs_operations, syncfs) + sizeof (((struct usfs_operations *)0)->syncfs),
                                               offsetof (struct usfs_operations, opendir) + sizeof (((struct usfs_operations *)0)->opendir),
                                               offsetof (struct usfs_operations, readdir) + sizeof (((struct usfs_operations *)0)->readdir),
                                               offsetof (struct usfs_operations, fsyncdir) + sizeof (((struct usfs_operations *)0)->fsyncdir),
                                               offsetof (struct usfs_operations, releasedir) + sizeof (((struct usfs_operations *)0)->releasedir),
                                               offsetof (struct usfs_operations, export_id) + sizeof (((struct usfs_operations *)0)->export_id),
                                               offsetof (struct usfs_operations, resolve_id) + sizeof (((struct usfs_operations *)0)->resolve_id) };

    if (operations_size >= sizeof (struct usfs_operations))
        return true;

    for (size_t member = 0; member < sizeof (complete_members) / sizeof (complete_members[0]); member++)
        if (operations_size == complete_members[member])
            return true;

    return false;
}

static int prepare_client_storage (struct usfs_client * client)
{
    const int synchronization_error = initialize_session_synchronization (client);
    if (synchronization_error != 0)
        return synchronization_error;

    initialize_connection_info (client);
    const int message_error = initialize_message_io (client);
    if (message_error != 0)
        destroy_session_synchronization (client);

    return message_error;
}

int usfs_client_create (
    const unsigned int api_version,
    const struct usfs_operations * operations,
    const size_t operations_size,
    const struct usfs_client_options * options,
    void * application_data,
    struct usfs_client ** result
)
{
    if (result == NULL)
        return -EINVAL;

    *result = NULL;
    if (api_version != USFS_CLIENT_API_VERSION)
        return -EPROTONOSUPPORT;

    if (operations == NULL && operations_size != 0)
        return -EINVAL;

    if (!is_operations_size_valid (operations_size))
        return -EINVAL;

    const struct usfs_client_options defaults = { 0 };
    const struct usfs_client_options * selected_options = options != NULL ? options : &defaults;
    if (!are_client_options_valid (selected_options))
        return -EINVAL;

    struct usfs_client * client = calloc (1, sizeof (*client));
    if (client == NULL)
        return -ENOMEM;

    size_t copied_size = operations_size;
    if (copied_size > sizeof (client->ops))
        copied_size = sizeof (client->ops);

    if (copied_size != 0)
        memcpy (&client->ops, operations, copied_size);

    client->fd = INVALID_FILE_DESCRIPTOR;
    client->session.client = client;
    client->session.wake_read = INVALID_FILE_DESCRIPTOR;
    client->session.wake_write = INVALID_FILE_DESCRIPTOR;
    client->mount_mode = selected_options->mount_access;
    client->behavior = selected_options->behavior;
    client->diagnostic = selected_options->diagnostic;
    client->diagnostic_data = selected_options->diagnostic_data;
    client->user_data = application_data;
    client->next_id = USFS_ROOT_ID + 1;

    const int error = prepare_client_storage (client);
    if (error != 0)
    {
        free (client);
        return error;
    }

    *result = client;
    return 0;
}

static int enter_destroy_callback_context (struct usfs_client * client, struct client_context_scope * scope)
{
    if (client->handles == NULL && client->nodes == NULL && (!client->init_done || client->ops.shutdown == NULL))
        return 0;

    return enter_callback_context (client, scope);
}

static int destroy_user_state (struct usfs_client * client)
{
    while (client->handles != NULL)
    {
        struct client_handle * handle = client->handles;

        client->handles = handle->next;
        dispose_client_handle (client, handle);
    }

    int first_error = 0;

    for (struct client_node * node = client->nodes; node != NULL; node = node->next)
    {
        const int error = cleanup_hidden_aliases (client, node, true);
        if (first_error == 0)
            first_error = error;
    }

    if (client->init_done && client->ops.shutdown != NULL)
        client->ops.shutdown (get_callback_request ());

    return first_error;
}

static void close_transport (struct usfs_client * client)
{
    if (client->fd >= 0)
        close (client->fd); /* triggers ddclose -> connection teardown */

    remove_signal_handlers (&client->session);

    close (client->session.wake_read);
    close (client->session.wake_write);
}

static void destroy_namespace (struct usfs_client * client)
{
    clear_directory_cache (client);

    struct client_node * node = client->nodes;

    while (node != NULL)
    {
        struct client_node * next = node->next;

        destroy_node (node);
        node = next;
    }
}

static void free_session_storage (struct usfs_client * client)
{
    free (client->mountpoint);
    free (client->readbuf);
    free (client->writebuf);
    destroy_session_synchronization (client);
    free (client);
}

int usfs_client_destroy (struct usfs_client ** client_pointer)
{
    struct client_context_scope scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };

    if (client_pointer == NULL)
        return -EINVAL;

    struct usfs_client * client = *client_pointer;
    if (client == NULL)
        return 0;

    if (client->mounted)
    {
        emit_diagnostic (client, "Failed to destroy filesystem: mount cleanup is unresolved\n");
        return -EBUSY;
    }

    const int error = enter_destroy_callback_context (client, &scope);
    if (error != 0)
        return -error;

    const int cleanup_error = destroy_user_state (client);
    close_transport (client);
    destroy_namespace (client);
    free_session_storage (client);
    *client_pointer = NULL;

    return -cleanup_error;
}

/*
 * Reports whether the file system can be modified, which is simply whether it
 * implements any operation that would modify it. The kernel extension is told
 * at mount time so that a read-only file system can refuse writes outright
 * instead of asking a daemon that would only refuse them too.
 */
static int is_filesystem_writable (const struct usfs_client * client)
{
    if (client->mount_mode != USFS_MOUNT_AUTOMATIC)
        return client->mount_mode == USFS_MOUNT_READ_WRITE;

    if (client->ops.create != NULL)
        return true;

    if (client->ops.create_attr != NULL)
        return true;

    if (client->ops.mkdir != NULL)
        return true;

    if (client->ops.write != NULL || client->ops.unlink != NULL)
        return true;

    if (client->ops.rmdir != NULL)
        return true;

    if (client->ops.rename != NULL)
        return true;

    if (client->ops.symlink != NULL || client->ops.link != NULL)
        return true;

    if (client->ops.chmod != NULL)
        return true;

    if (client->ops.chown != NULL)
        return true;

    if (client->ops.truncate != NULL || client->ops.utimens != NULL)
        return true;

    return false;
}

#define CLIENT_VMOUNT_ALIGNMENT_BYTES       4
#define CLIENT_VMOUNT_BUFFER_SIZE           4096
#define CLIENT_MOUNT_INFO_BUFFER_SIZE       128
#define CLIENT_MOUNT_INVENTORY_INITIAL_SIZE 8192
#define CLIENT_UNMOUNT_MAX_ATTEMPTS         20
#define CLIENT_UNMOUNT_RETRY_DELAY_US       250000

static void align_mount_data_cursor (char ** cursor)
{
    while ((uintptr_t)(*cursor) % CLIENT_VMOUNT_ALIGNMENT_BYTES != 0)
        *cursor += 1;
}

static void append_mount_field (struct vmount * vm, char ** data, const int field, const char * str)
{
    align_mount_data_cursor (data);
    vm->vmt_data[field].vmt_off = (int)(*data - (char *)vm);
    vm->vmt_data[field].vmt_size = (int)strlen (str) + 1;
    strcpy (*data, str);
    *data += strlen (str) + 1;
}

static int request_connection (struct usfs_client * client)
{
    if (ioctl (client->fd, USFS_IOC_CONNECTION_REQUEST, &client->info) != 0)
    {
        emit_diagnostic (client, "Failed to establish USFS connection: %s\n", strerror (errno));
        return -1;
    }

    if (client->info.protocol_version != USFS_PROTOCOL_VERSION)
    {
        emit_diagnostic (
            client,
            "Failed to establish USFS connection: protocol version mismatch (kernel %u, library %u)\n",
            (unsigned)client->info.protocol_version,
            (unsigned)USFS_PROTOCOL_VERSION
        );
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int submit_mount (struct usfs_client const * client, const char * mountpoint, const char * mount_info)
{
    const size_t buffer_size = CLIENT_VMOUNT_BUFFER_SIZE;
    char * buffer = calloc (1, buffer_size);
    char * data;

    if (buffer == NULL)
        return -1;

    struct vmount * vm = (struct vmount *)buffer;
    data = buffer + sizeof (struct vmount);

    vm->vmt_revision = VMT_REVISION;
    vm->vmt_flags = is_filesystem_writable (client) ? 0 : MNT_READONLY;
    vm->vmt_gfstype = client->info.fs_type;

    append_mount_field (vm, &data, VMT_OBJECT, "usfs");
    append_mount_field (vm, &data, VMT_STUB, mountpoint);
    append_mount_field (vm, &data, VMT_HOST, "localhost");
    append_mount_field (vm, &data, VMT_HOSTNAME, "localhost.localdomain");
    append_mount_field (vm, &data, VMT_INFO, mount_info);
    append_mount_field (vm, &data, VMT_ARGS, mount_info);

    align_mount_data_cursor (&data);
    vm->vmt_length = (int)(data - buffer);

    const int result = vmount (vm, vm->vmt_length);
    free (buffer);
    if (result != 0)
        emit_diagnostic (client, "Failed to mount %s: %s\n", mountpoint, strerror (errno));

    return result;
}

static int connect_and_mount_filesystem (struct usfs_client * client, const char * resolved_mountpoint, char ** cleanup_mountpoint)
{
    char mount_info[CLIENT_MOUNT_INFO_BUFFER_SIZE];

    if (request_connection (client) != 0)
        return -1;

    snprintf (
        mount_info,
        sizeof (mount_info),
        "fd=%d,chan=%d,cookie=%016llx,rw=%d",
        client->fd,
        (int)client->info.channel,
        (unsigned long long)client->info.cookie,
        is_filesystem_writable (client)
    );
    *cleanup_mountpoint = strdup (resolved_mountpoint);

    if (*cleanup_mountpoint == NULL)
        return -1;

    if (submit_mount (client, resolved_mountpoint, mount_info) != 0)
        return -1;

    return 0;
}

int usfs_client_mount (struct usfs_client * client, const char * mountpoint)
{
    if (client == NULL || mountpoint == NULL)
        return -EINVAL;

    if (client->mounted || client->fd >= 0)
        return -EBUSY;

    char resolved_mountpoint[PATH_MAX];
    char * cleanup_mountpoint = NULL;
    if (realpath (mountpoint, resolved_mountpoint) == NULL)
    {
        const int error = current_system_error ();
        emit_diagnostic (client, "Failed to resolve mountpoint `%s': %s\n", mountpoint, strerror (error));
        return -error;
    }

    client->fd = open (USFS_DEVICE_PATH, O_RDWR | O_NONBLOCK | _FCLOEXEC);
    if (client->fd < 0)
    {
        const int error = current_system_error ();
        emit_diagnostic (client, "Failed to open %s: %s\n", USFS_DEVICE_PATH, strerror (error));
        return -error;
    }

    if (connect_and_mount_filesystem (client, resolved_mountpoint, &cleanup_mountpoint) != 0)
    {
        const int error = current_system_error ();
        free (cleanup_mountpoint);
        close (client->fd);
        client->fd = INVALID_FILE_DESCRIPTOR;
        return -error;
    }

    client->mountpoint = cleanup_mountpoint;
    client->mounted = 1;
    return 0;
}

static int read_mount_inventory_buffer (char ** buffer, const int buffer_size)
{
    char * resized_buffer = realloc (*buffer, (size_t)buffer_size);

    if (resized_buffer == NULL)
        return -1;

    *buffer = resized_buffer;

    return mntctl (MCTL_QUERY, buffer_size, *buffer);
}

static int read_complete_mount_inventory (char ** buffer, int * buffer_size)
{
    /* mntctl semantics: > 0 = number of vmount entries in the buffer,
       0 = buffer too small (required size in the first word). */
    for (;;)
    {
        const int mount_count = read_mount_inventory_buffer (buffer, *buffer_size);
        if (mount_count > 0)
            return mount_count;

        if (mount_count < 0)
        {
            free (*buffer);
            *buffer = NULL;

            return -1;
        }

        const int required_size = *(int *)*buffer; // todo: map to struct explicitly

        if (required_size <= *buffer_size)
        {
            free (*buffer);
            *buffer = NULL;
            errno = EIO;

            return -1;
        }

        *buffer_size = required_size;
    }
}

struct mount_inventory
{
    char * buffer;    // Owned vmount records returned by mntctl.
    int record_count; // Number of records returned by mntctl.
    int buffer_size;  // Allocated buffer capacity in bytes.
};

static int query_mount_inventory (struct mount_inventory * inventory)
{
    int buffer_size = CLIENT_MOUNT_INVENTORY_INITIAL_SIZE;
    char * buffer = NULL;
    const int mount_count = read_complete_mount_inventory (&buffer, &buffer_size);

    if (mount_count < 0)
        return -1;

    inventory->buffer = buffer;
    inventory->record_count = mount_count;
    inventory->buffer_size = buffer_size;

    return 0;
}

static int check_mount_record_ownership (const struct usfs_client * client, const char * record, const struct vmount * mount, int * matches)
{
    *matches = 0;

    if (mount->vmt_gfstype != client->info.fs_type)
        return 0;

    const struct vmt_data * field = &mount->vmt_data[VMT_INFO];

    if (!usfs_bounded_region_valid (mount->vmt_length, field->vmt_off, field->vmt_size, sizeof (*mount)))
        return -1;

    const char * info = record + field->vmt_off;
    uint64_t cookie;
    const int cookie_parse_result = usfs_info_field_u64 (info, field->vmt_size, "cookie", sizeof ("cookie") - 1, CLIENT_HEXADECIMAL_RADIX, &cookie);
    if (cookie_parse_result != 1)
        return -1;

    uint64_t channel;
    const int channel_parse_result = usfs_info_field_u64 (info, field->vmt_size, "chan", sizeof ("chan") - 1, CLIENT_DECIMAL_RADIX, &channel);
    if (channel_parse_result != 1)
        return -1;

    *matches = cookie == client->info.cookie && channel == (uint64_t)client->info.channel;

    return 0;
}

static int find_owned_mount (const struct usfs_client * client, const struct mount_inventory * inventory, int * number)
{
    int found = 0;
    const char * cursor = inventory->buffer;
    size_t remaining = (size_t)inventory->buffer_size;

    if ((size_t)inventory->record_count > remaining / sizeof (struct vmount))
        return -1;

    for (int mount_index = 0; mount_index < inventory->record_count; ++mount_index)
    {
        const struct vmount * mount_record = (const struct vmount *)cursor;
        int matches;

        if (remaining < sizeof (*mount_record))
            return -1;

        if (mount_record->vmt_revision != VMT_REVISION)
            return -1;

        if (mount_record->vmt_length < sizeof (*mount_record))
            return -1;

        if (mount_record->vmt_length > remaining)
            return -1;

        if (check_mount_record_ownership (client, cursor, mount_record, &matches) != 0)
            return -1;

        if (matches)
        {
            if (found || mount_record->vmt_vfsnumber <= 0)
                return -1;

            *number = mount_record->vmt_vfsnumber;
            found = 1;
        }

        cursor += mount_record->vmt_length;
        remaining -= mount_record->vmt_length;
    }

    return found;
}

static int query_owned_mount_number (const struct usfs_client * client, int * number)
{
    struct mount_inventory inventory = { 0 };

    if (query_mount_inventory (&inventory) != 0)
        return -1;

    const int found = find_owned_mount (client, &inventory, number);

    free (inventory.buffer);

    if (found < 0)
        errno = EIO;

    return found;
}

static void close_exited_channel (struct usfs_client * client)
{
    if (is_session_exited (&client->session) && client->fd >= 0)
    {
        close (client->fd);
        client->fd = INVALID_FILE_DESCRIPTOR;
    }
}

static int unmount_owned_mount (const struct usfs_client * client, int number)
{
    /* AIX may briefly retain the mount after its last access. Every retry uses
     * the verified unique mount number, never its pathname. */
    for (int attempt = 0; attempt < CLIENT_UNMOUNT_MAX_ATTEMPTS; ++attempt)
    {
        if (uvmount (number, 0) == 0)
            return 0;

        if (errno != EBUSY)
        {
            const int error = errno;

            /* External unmount can finish between the inventory read and this
             * call. Only verified absence releases ownership. */
            if (error == EINVAL && query_owned_mount_number (client, &number) == 0)
                return 0;

            emit_diagnostic (client, "Failed to unmount %s: %s\n", client->mountpoint, strerror (error));
            errno = error;

            return -1;
        }

        if (attempt + 1 < CLIENT_UNMOUNT_MAX_ATTEMPTS)
            usleep (CLIENT_UNMOUNT_RETRY_DELAY_US);
    }

    emit_diagnostic (client, "Failed to unmount %s: still busy\n", client->mountpoint);
    errno = EBUSY;

    return -1;
}

int usfs_client_unmount (struct usfs_client * client)
{
    if (client == NULL)
        return -EINVAL;

    if (!client->mounted)
        return 0;

    if (client->mountpoint == NULL)
        return -EINVAL;

    int number = -1;
    const int found = query_owned_mount_number (client, &number);
    if (found < 0)
        return -current_system_error ();

    close_exited_channel (client);
    if (found && unmount_owned_mount (client, number) != 0)
        return -current_system_error ();

    client->mounted = 0;
    return 0;
}

static int initialize_filesystem_once (struct usfs_client * client)
{
    struct client_context_scope scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };
    const int error = enter_callback_context (client, &scope);
    if (error != 0)
        return -error;

    if (client->init_done)
        return 0;

    if (client->ops.initialize != NULL)
    {
        const int result = client->ops.initialize (get_callback_request (), &client->limits, &client->behavior);
        if (result != 0)
            return -normalize_callback_error (result);
    }

    if (!is_behavior_valid (&client->behavior))
        return -EINVAL;

    client->init_done = 1;
    return 0;
}

static int fail_session_on_device_error (struct usfs_client * client, const int error)
{
    fail_session (client, error);

    return resolve_session_result (client, -error);
}

static int read_and_handle_request (struct usfs_client * client)
{
    const ssize_t request_length = read (client->fd, client->readbuf, USFS_MSG_MAX);

    if (request_length < 0)
    {
        if (errno == EINTR || errno == EAGAIN)
            return 0; /* The request loop re-checks the session exit state. */

        return fail_session_on_device_error (client, errno);
    }

    if (request_length == 0)
    {
        usfs_client_request_stop (client);
        return 0;
    }

    if (!is_session_exited (&client->session))
        handle_request (client, client->readbuf, (size_t)request_length);

    return 0;
}

static int run_request_loop (struct usfs_client * client)
{
    while (!is_session_exited (&client->session))
    {
        const int ready = wait_for_session_event (&client->session, client->fd);

        if (ready != 0)
        {
            fail_session (client, -ready);
            return resolve_session_result (client, ready);
        }

        if (is_session_exited (&client->session))
            break;

        const int rc = read_and_handle_request (client);
        if (rc != 0)
            return rc;
    }

    return resolve_session_result (client, 0);
}

static int run_singlethreaded_session (struct usfs_client * client)
{
    if (client == NULL)
        return -EINVAL;

    const int init_error = initialize_filesystem_once (client);

    if (init_error != 0)
    {
        fail_session (client, -init_error);
        return resolve_session_result (client, init_error);
    }

    return run_request_loop (client);
}

#define CLIENT_MT_QUEUE_FACTOR 2u

struct client_mt_slot
{
    size_t len;              // Number of request bytes in this slot.
    char data[USFS_MSG_MAX]; // Owned fixed-capacity request storage.
};

struct client_mt_pool
{
    struct usfs_client * client;   // Session served by the workers.
    pthread_mutex_t lock;          // Protects queue and worker-start state.
    pthread_cond_t started;        // Notifies the reader of worker initialization.
    pthread_cond_t readable;       // Notifies workers when requests arrive or stop is requested.
    pthread_cond_t writable;       // Notifies queue capacity changes.
    struct client_mt_slot * slots; // Owned bounded request queue.
    size_t capacity;               // Number of available queue slots.
    size_t head;                   // Next queue slot to consume.
    size_t tail;                   // Next queue slot to publish.
    size_t count;                  // Number of queued requests.
    size_t ready;                  // Number of workers that completed initialization.
    int worker_error;              // Worker context initialization error.
    int stopping;                  // Whether workers should stop after draining the queue.
};

struct client_mt_worker
{
    struct client_mt_pool * pool;        // Borrowed shared queue.
    struct client_thread_context thread; // Worker-owned reply buffer holder.
    char * readbuf;                      // Owned request buffer of USFS_MSG_MAX bytes.
};

struct client_mt_runtime
{
    struct client_mt_pool pool;        // Shared worker queue.
    struct client_mt_worker * workers; // Owned worker storage.
    pthread_t * threads;               // Owned thread identifiers.
    unsigned int worker_count;         // Number of configured workers.
    unsigned int started;              // Number of successfully created threads.
};

struct client_mt_signal_mask
{
    sigset_t previous; // Reader signal mask saved before worker creation.
    int blocked;       // Whether the original mask still needs restoration.
};

static int initialize_request_worker (const struct client_mt_worker * worker, struct client_thread_context ** thread)
{
    struct client_mt_pool * pool = worker->pool;
    const int rc = acquire_thread_context (thread);

    if (rc == 0)
        (*thread)->writebuf = worker->thread.writebuf;

    pthread_mutex_lock (&pool->lock);
    pool->ready += 1;

    if (rc != 0)
        pool->worker_error = rc;

    pthread_cond_broadcast (&pool->started);
    pthread_mutex_unlock (&pool->lock);

    return rc;
}

static int dequeue_worker_request (const struct client_mt_worker * worker, size_t * request_length)
{
    struct client_mt_pool * pool = worker->pool;

    pthread_mutex_lock (&pool->lock);

    while (pool->count == 0 && !pool->stopping)
        pthread_cond_wait (&pool->readable, &pool->lock);

    if (pool->count == 0 && pool->stopping)
    {
        pthread_mutex_unlock (&pool->lock);

        return false;
    }

    *request_length = pool->slots[pool->head].len;
    memcpy (worker->readbuf, pool->slots[pool->head].data, *request_length);
    pool->head = (pool->head + 1) % pool->capacity;
    pool->count -= 1;
    pthread_cond_signal (&pool->writable);
    pthread_mutex_unlock (&pool->lock);

    notify_session (&pool->client->session);

    return true;
}

static void * run_request_worker (void * opaque)
{
    const struct client_mt_worker * worker = opaque;
    const struct client_mt_pool * pool = worker->pool;
    struct client_thread_context * thread;

    if (initialize_request_worker (worker, &thread) != 0)
        return NULL;

    size_t request_length;

    while (dequeue_worker_request (worker, &request_length))
    {
        if (!is_session_exited (&pool->client->session))
            handle_request (pool->client, worker->readbuf, request_length);
    }

    thread->writebuf = NULL; /* The worker pool owns the buffer, TLS owns only context. */

    return NULL;
}

static int enqueue_worker_request (struct client_mt_pool * pool, const char * request, const size_t request_length)
{
    int result = 0;

    pthread_mutex_lock (&pool->lock);

    while (pool->count == pool->capacity && !is_session_exited (&pool->client->session))
    {
        pthread_mutex_unlock (&pool->lock);
        result = wait_for_session_event (&pool->client->session, -1);
        pthread_mutex_lock (&pool->lock);

        if (result != 0)
            break;
    }

    if (!is_session_exited (&pool->client->session) && result == 0)
    {
        pool->slots[pool->tail].len = request_length;
        memcpy (pool->slots[pool->tail].data, request, request_length);
        pool->tail = (pool->tail + 1) % pool->capacity;
        pool->count += 1;
        pthread_cond_signal (&pool->readable);
    }

    pthread_mutex_unlock (&pool->lock);

    return result;
}

static int read_and_enqueue_requests (struct usfs_client * client, struct client_mt_pool * pool)
{
    while (!is_session_exited (&client->session))
    {
        int result = wait_for_session_event (&client->session, client->fd);
        if (result != 0 || is_session_exited (&client->session))
            return result;

        const ssize_t request_length = read (client->fd, client->readbuf, USFS_MSG_MAX);

        if (request_length < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return fail_session_on_device_error (client, errno);
        }

        if (request_length == 0)
        {
            usfs_client_request_stop (client);
            return 0;
        }

        result = enqueue_worker_request (pool, client->readbuf, (size_t)request_length);
        if (result != 0)
            return result;
    }

    return 0;
}

static void free_worker_storage (struct client_mt_runtime * runtime)
{
    free (runtime->threads);
    free (runtime->workers);
    free (runtime->pool.slots);
}

static int initialize_worker_synchronization (struct client_mt_pool * pool)
{
    if (pthread_mutex_init (&pool->lock, NULL) != 0)
    {
        return -EAGAIN;
    }

    if (pthread_cond_init (&pool->readable, NULL) != 0)
    {
        pthread_mutex_destroy (&pool->lock);
        return -EAGAIN;
    }

    if (pthread_cond_init (&pool->writable, NULL) != 0)
    {
        pthread_cond_destroy (&pool->readable);
        pthread_mutex_destroy (&pool->lock);
        return -EAGAIN;
    }

    if (pthread_cond_init (&pool->started, NULL) != 0)
    {
        pthread_cond_destroy (&pool->writable);
        pthread_cond_destroy (&pool->readable);
        pthread_mutex_destroy (&pool->lock);
        return -EAGAIN;
    }

    return 0;
}

static int initialize_worker_runtime (struct client_mt_runtime * runtime, struct usfs_client * client, unsigned int worker_count)
{
    memset (runtime, 0, sizeof (*runtime));

    struct client_mt_pool * pool = &runtime->pool;
    pool->client = client;
    pool->capacity = (size_t)worker_count * CLIENT_MT_QUEUE_FACTOR;
    runtime->worker_count = worker_count;

    pool->slots = calloc (pool->capacity, sizeof (*pool->slots));
    runtime->workers = calloc (worker_count, sizeof (*runtime->workers));
    runtime->threads = calloc (worker_count, sizeof (*runtime->threads));

    if (pool->slots == NULL || runtime->workers == NULL || runtime->threads == NULL)
    {
        free_worker_storage (runtime);
        return -ENOMEM;
    }

    const int rc = initialize_worker_synchronization (pool);
    if (rc != 0)
        free_worker_storage (runtime);

    return rc;
}

static void destroy_worker_runtime (struct client_mt_runtime * runtime)
{
    pthread_cond_destroy (&runtime->pool.started);
    pthread_cond_destroy (&runtime->pool.writable);
    pthread_cond_destroy (&runtime->pool.readable);
    pthread_mutex_destroy (&runtime->pool.lock);
    free_worker_storage (runtime);
}

static int block_worker_exit_signals (struct client_mt_signal_mask * mask)
{
    sigset_t exit_signals;

    sigemptyset (&exit_signals);
    sigaddset (&exit_signals, SIGHUP);
    sigaddset (&exit_signals, SIGINT);
    sigaddset (&exit_signals, SIGTERM);

    if (pthread_sigmask (SIG_BLOCK, &exit_signals, &mask->previous) != 0)
        return -EAGAIN;

    mask->blocked = 1;

    return 0;
}

static int restore_thread_signal_mask (struct client_mt_signal_mask * mask)
{
    if (!mask->blocked)
        return 0;

    if (pthread_sigmask (SIG_SETMASK, &mask->previous, NULL) != 0)
        return -EAGAIN;

    mask->blocked = 0;

    return 0;
}

static int start_request_workers (struct client_mt_runtime * runtime)
{
    for (unsigned int worker_index = 0; worker_index < runtime->worker_count; worker_index++)
    {
        struct client_mt_worker * worker = &runtime->workers[worker_index];

        worker->pool = &runtime->pool;
        worker->readbuf = malloc (USFS_MSG_MAX);
        worker->thread.writebuf = malloc (USFS_MSG_MAX);

        if (worker->readbuf == NULL || worker->thread.writebuf == NULL ||
            pthread_create (&runtime->threads[worker_index], NULL, run_request_worker, worker) != 0)
            return -EAGAIN;

        runtime->started += 1;
    }

    return 0;
}

static int wait_for_worker_startup (struct client_mt_runtime * runtime)
{
    struct client_mt_pool * pool = &runtime->pool;

    pthread_mutex_lock (&pool->lock);
    while (pool->ready < runtime->started)
        pthread_cond_wait (&pool->started, &pool->lock);

    const int result = pool->worker_error == 0 ? 0 : -pool->worker_error;
    pthread_mutex_unlock (&pool->lock);

    return result;
}

static void stop_request_workers (struct client_mt_runtime * runtime)
{
    struct client_mt_pool * pool = &runtime->pool;
    unsigned int worker_index;

    pthread_mutex_lock (&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast (&pool->readable);
    pthread_cond_broadcast (&pool->writable);
    pthread_mutex_unlock (&pool->lock);

    for (worker_index = 0; worker_index < runtime->started; worker_index++)
        pthread_join (runtime->threads[worker_index], NULL);

    for (worker_index = 0; worker_index < runtime->worker_count; worker_index++)
    {
        free (runtime->workers[worker_index].readbuf);
        free (runtime->workers[worker_index].thread.writebuf);
    }
}

static int run_worker_runtime (struct usfs_client * client, struct client_mt_runtime * runtime)
{
    struct client_mt_signal_mask signal_mask = { 0 };
    int result = block_worker_exit_signals (&signal_mask);

    if (result == 0)
    {
        result = start_request_workers (runtime);

        if (restore_thread_signal_mask (&signal_mask) != 0)
            result = -EAGAIN;

        const int worker_error = wait_for_worker_startup (runtime);
        if (worker_error != 0)
            result = worker_error;

        if (runtime->started == runtime->worker_count && result == 0)
            result = read_and_enqueue_requests (client, &runtime->pool);

        stop_request_workers (runtime);
        (void)restore_thread_signal_mask (&signal_mask);
    }

    return result;
}

static int run_multithreaded_session (struct usfs_client * client, const unsigned int thread_count)
{
    struct client_mt_runtime runtime;
    int result = 0;

    if (client == NULL || thread_count == 0 || thread_count > USFS_CLIENT_MAX_WORKERS)
        return -EINVAL;

    const int init_error = initialize_filesystem_once (client);

    if (init_error != 0)
    {
        fail_session (client, -init_error);
        return resolve_session_result (client, init_error);
    }

    result = initialize_worker_runtime (&runtime, client, thread_count);

    if (result != 0)
    {
        fail_session (client, -result);
        return resolve_session_result (client, result);
    }

    result = run_worker_runtime (client, &runtime);
    destroy_worker_runtime (&runtime);
    if (result != 0)
        fail_session (client, -result);

    return resolve_session_result (client, result);
}

struct usfs_client * usfs_request_client (const struct usfs_client_request * request)
{
    return request != NULL ? request->client : NULL;
}

void * usfs_request_user_data (const struct usfs_client_request * request)
{
    return request != NULL ? request->user_data : NULL;
}

uid_t usfs_request_uid (const struct usfs_client_request * request)
{
    return request != NULL ? request->uid : 0;
}

gid_t usfs_request_gid (const struct usfs_client_request * request)
{
    return request != NULL ? request->gid : 0;
}

pid_t usfs_request_pid (const struct usfs_client_request * request)
{
    return request != NULL ? request->pid : 0;
}

int usfs_client_run (struct usfs_client * client, const unsigned int worker_count)
{
    if (client == NULL)
        return -EINVAL;

    if (worker_count == 0 || worker_count > USFS_CLIENT_MAX_WORKERS)
        return -EINVAL;

    const int fatal_error = __atomic_load_n (&client->session.fatal_error, __ATOMIC_ACQUIRE);
    if (fatal_error != 0)
        return -fatal_error;

    if (worker_count == 1)
        return run_singlethreaded_session (client);

    return run_multithreaded_session (client, worker_count);
}

int usfs_client_install_signals (struct usfs_client * client)
{
    if (client == NULL)
        return -EINVAL;

    if (install_signal_handlers (&client->session) != 0)
        return -current_system_error ();

    return 0;
}

void usfs_client_remove_signals (struct usfs_client * client)
{
    if (client != NULL)
        remove_signal_handlers (&client->session);
}
