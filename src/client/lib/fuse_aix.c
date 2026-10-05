// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#define FUSE_USE_VERSION 31

#include "fuse.h"

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

#define FUSE_DEFAULT_BLOCK_SIZE     4096
#define FUSE_NANOSECONDS_PER_SECOND 1000000000L
#define FUSE_DECIMAL_RADIX          10
#define FUSE_HEXADECIMAL_RADIX      16

enum fuse_mount_mode
{
    FUSE_MOUNT_MODE_AUTOMATIC = 0,
    FUSE_MOUNT_MODE_READ_ONLY = 1,
    FUSE_MOUNT_MODE_READ_WRITE = 2
};

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

    /* High-level FUSE callbacks return negative errno. Do not let a buggy
       positive return escape as an invalid negative wire errno. */
    if (result > 0 || result == INT_MIN)
        return EIO;

    return -result;
}

struct fuse_session
{
    volatile sig_atomic_t exited; // Atomic exit request shared with signal handlers.
    int fatal_error;              // First fatal session error.
    int wake_read;                // Owned notification read descriptor.
    int wake_write;               // Owned notification write descriptor.
};

_Static_assert (__atomic_always_lock_free (sizeof (sig_atomic_t), 0), "signal exit state requires lock-free atomics");

static int is_session_exited (const struct fuse_session * session)
{
    return __atomic_load_n (&session->exited, __ATOMIC_ACQUIRE);
}

static void notify_session (const struct fuse_session * session)
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

static void open_session_wake_fifo (struct fuse_session * session, const char * path)
{
    if (mkfifo (path, S_IRUSR | S_IWUSR) != 0)
        return;

    session->wake_read = open (path, O_RDONLY | O_NONBLOCK | _FCLOEXEC);

    if (session->wake_read < 0)
        return;

    session->wake_write = open (path, O_WRONLY | O_NONBLOCK | _FCLOEXEC);
}

static int detach_session_wake_fifo (struct fuse_session * session, const char * path, const char * directory)
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

static int create_session_wake_pipe (struct fuse_session * session)
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

#define FUSE_WAKE_DRAIN_BUFFER_SIZE 128
#define FUSE_POLL_WAIT_FOREVER      -1

/* The sole reader uses this for both device availability and queue capacity. */
static int wait_for_session_event (const struct fuse_session * session, const int device_descriptor)
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

    const int rc = poll (poll_descriptors, device_descriptor < 0 ? 1 : SESSION_POLL_SLOT_COUNT, FUSE_POLL_WAIT_FOREVER);

    if (rc < 0)
        return errno == EINTR ? 0 : -errno;

    if (poll_descriptors[SESSION_POLL_WAKE].revents & POLLIN)
    {
        char pending_notifications[FUSE_WAKE_DRAIN_BUFFER_SIZE];

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
#define FUSE_NODE_BUCKETS 4096

#define FUSE_ID_HASH_MULTIPLIER   1000003u
#define FUSE_NAME_HASH_MULTIPLIER 31u

/* Retained directory data uses at most 16 MiB of RAM. Additional snapshots
   spill to unlinked files, with a separate per-daemon temporary-storage cap. */
#define FUSE_DIRCACHE_SLOTS         64
#define FUSE_DIRCACHE_MAX           (4u * 1024 * 1024)
#define FUSE_DIRCACHE_RAM_MAX       (16u * 1024 * 1024)
#define FUSE_DIRSPOOL_MAX           (256u * 1024 * 1024)
#define FUSE_DIRSPOOL_RESERVATION   (1024u * 1024u)
#define FUSE_DIRECTORY_INDEX_STRIDE 1024u

#define FUSE_DIRBUF_INITIAL_CAPACITY 4096
#define FUSE_DIRBUF_GROWTH_FACTOR    2
#define FUSE_DIRENT_MODE_MASK        0170000u
#define FUSE_DIRENT_TYPE_SHIFT       12
#define FUSE_SYNTHETIC_INODE_BASE    0xF0000000u
#define FUSE_CREATE_PERMISSION_MASK  07777

struct fuse_alias
{
    struct fuse_alias * next;  // Next name belonging to the object.
    struct fuse_alias * hnext; // Next entry in the parent/name hash bucket.
    struct fuse_node * node;   // Object identified by this name.
    uint64_t parent;           // Parent node identifier.
    char * name;               // Owned name within the parent directory.
    int attached;              // Whether this alias is present in the indexes.
};

struct fuse_node
{
    pthread_mutex_t mutation_lock;    // Serializes mutations of this object.
    struct fuse_node * next;          // Next retained object.
    struct fuse_node * hnext_id;      // Next entry in the node-ID hash bucket.
    struct fuse_node * hnext_backend; // Next entry in the backend-identity hash bucket.
    uint64_t id;                      // Stable wire node identifier.
    uint64_t lookup_refs;             // Kernel lookup references.
    uint64_t open_refs;               // Open handle references.
    uint64_t operation_refs;          // In-flight request references.
    uint64_t ancestor_refs;           // References from named descendants.
    uint64_t cache_refs;              // Directory snapshot references.
    struct fuse_node * gc_next;       // Next pending garbage-collection candidate.
    int gc_queued;                    // Whether collection is already queued.
    struct fuse_alias * aliases;      // Names belonging to this object.
    uint64_t backend_dev;             // Backend device identity.
    uint64_t backend_ino;             // Backend inode identity.
    uint64_t backend_links;           // Last observed backend hard-link count.
    mode_t backend_type;              // Backend file type bits.
    unsigned names;                   // Number of attached names.
    int backend_indexed;              // Whether backend identity is indexed.
    int backend_retired;              // Whether the backend identity was retired.
    int detached;                     // Whether retained identity has no resolving name.
};

struct fuse_handle
{
    struct fuse_handle * next; // Next open handle.
    uint64_t nodeid;           // Wire node identifier.
    uint64_t fh;               // Backend callback handle.
    uint64_t generation;       // Unique handle lifetime identifier.
    int flags;                 // Open flags supplied to callbacks.
    int isdir;                 // Whether this is a directory handle.
};

struct fuse_dircache_key
{
    uint64_t nodeid;     // Directory node identifier.
    uint64_t fh;         // Backend directory handle.
    uint64_t generation; // Handle lifetime used for identity checks.
    uint32_t uid;        // Request user identity.
    uint32_t gid;        // Request group identity.
    uint32_t pid;        // Request process identity.
};

struct fuse_dircache
{
    struct fuse_dircache_key key; // Snapshot identity and credentials.
    uint64_t seq;                 // Least-recently-used sequence number.
    uint64_t snapshot_id;         // Opaque cursor identity for this listing.
    char * data;                  // Owned serialized directory entries.
    size_t data_capacity;         // Retained RAM charged for this snapshot.
    size_t len;                   // Number of serialized bytes.
    uint32_t count;               // Number of serialized entries.
    int spool_fd;                 // Unlinked temporary file when spooled.
    size_t * checkpoints;         // Byte offset of every 1024th entry.
    size_t checkpoint_count;      // Number of populated checkpoint offsets.
    size_t spool_reserved;        // Bytes charged to the daemon's spool quota.
    bool spooled;                 // Whether spool_fd owns a temporary file.
    bool owns_node_ref;           // Whether this snapshot retains its node.
    bool completed;               // Entry storage was released after EOF.
};

/* Admission counts are protected by lock; the lock itself is never held
 * across a callback. A queued writer prevents newly arriving readers from
 * overtaking it. Dispatch is nonrecursive: callbacks must not synchronously
 * access this mount through its kernel interface. */
struct fuse_namespace
{
    pthread_mutex_t lock;     // Protects admission counters.
    pthread_cond_t changed;   // Notifies waiters when admission changes.
    unsigned readers;         // Number of admitted readers.
    unsigned waiting_writers; // Writers waiting for exclusive admission.
    int writer;               // Whether a writer is admitted.
};

struct fuse
{
    int fd;                                                // Owned connection device descriptor.
    struct usfs_dev_info info;                             // Kernel connection identity and filesystem type.
    struct fuse_operations ops;                            // Filesystem callback table.
    void * user_data;                                      // Filesystem-owned callback state.
    char * mountpoint;                                     // Owned canonical mount pathname.
    int mounted;                                           // Whether this connection still owns a mount.
    int mount_mode;                                        // Requested fuse_mount_mode; automatic derives access from callbacks.
    int init_done;                                         // Whether initialization callback has run.
    struct fuse_session session;                           // Exit and notification state.
    struct fuse_conn_info conn;                            // Negotiated connection capabilities.
    struct fuse_config config;                             // High-level callback configuration.
    struct fuse_node * nodes;                              // Retained object list.
    struct fuse_node * by_id[FUSE_NODE_BUCKETS];           // Objects indexed by wire node ID.
    struct fuse_alias * by_parent_name[FUSE_NODE_BUCKETS]; // Aliases indexed by parent and name.
    struct fuse_node * by_backend[FUSE_NODE_BUCKETS];      // Objects indexed by backend identity.
    uint64_t next_id;                                      // Next available wire node identifier.
    uint64_t root_lookup_refs;                             // Lookup references held on the implicit root.
    struct fuse_node * gc_pending;                         // Objects pending collection.
    struct fuse_handle * handles;                          // Open callback handles.
    uint64_t handle_generation;                            // Next handle lifetime sequence number.
    struct fuse_dircache dircache[FUSE_DIRCACHE_SLOTS];    // Owned directory snapshot slots.
    uint64_t dircache_seq;                                 // Next snapshot recency sequence number.
    size_t dircache_ram_bytes;                             // Retained directory data allocation.
    uint64_t next_snapshot_id;                             // Next positive READDIR cursor identity.
    size_t dircache_spool_bytes;                           // Reserved temporary storage across snapshots.
    pthread_mutex_t node_lock;                             // Protects object identities and references.
    pthread_mutex_t dircache_lock;                         // Protects directory snapshots.
    pthread_mutex_t reply_lock;                            // Serializes complete wire replies.
    struct fuse_namespace namespace;                       // Request namespace admission state.
    char * readbuf;                                        // Owned request buffer of USFS_MSG_MAX bytes.
    char * writebuf;                                       // Owned fallback reply buffer of USFS_MSG_MAX bytes.
};

struct request_context
{
    struct fuse * fuse;                // Session serving this request.
    const struct usfs_in_hdr * header; // Borrowed validated request header.
};

struct fuse_thread_context
{
    struct fuse_context context; // Current callback credentials and private state.
    char * writebuf;             // Borrowed worker reply buffer, or NULL.
    int active;                  // Whether callback context is active.
};

static pthread_key_t fuse_context_key;
static pthread_mutex_t fuse_context_key_lock = PTHREAD_MUTEX_INITIALIZER;
static int fuse_context_key_ready;
static pthread_mutex_t signal_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fuse_session * signal_session;
static unsigned signal_handlers_active;
static struct sigaction previous_signal_actions[4];
static struct sigaction installed_signal_actions[4];
_Static_assert (__atomic_always_lock_free (sizeof (signal_session), 0), "signal session publication must be lock free");
_Static_assert (__atomic_always_lock_free (sizeof (signal_handlers_active), 0), "signal handler lifetime counter must be lock free");

/* Deterministic unit barrier, compiled away in ordinary clients. */
#ifndef USFS_NAMESPACE_WAITING
    #define USFS_NAMESPACE_WAITING(exclusive) ((void)0)
#endif

static int namespace_access_must_wait (const struct fuse_namespace * admission_state, const int exclusive)
{
    if (admission_state->writer)
        return true;

    if (exclusive)
        return admission_state->readers != 0;

    return admission_state->waiting_writers != 0;
}

static int wait_for_namespace_access_locked (struct fuse_namespace * admission_state, const int exclusive)
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

static int acquire_namespace_access (struct fuse * fuse, const int exclusive)
{
    struct fuse_namespace * admission_state = &fuse->namespace;
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

static void release_namespace_access (struct fuse * fuse, const int exclusive)
{
    struct fuse_namespace * admission_state = &fuse->namespace;

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
    if (__atomic_load_n (&fuse_context_key_ready, __ATOMIC_ACQUIRE))
        return 0;

    int error = pthread_mutex_lock (&fuse_context_key_lock);

    if (error != 0)
        return error;

    if (!__atomic_load_n (&fuse_context_key_ready, __ATOMIC_RELAXED))
    {
        error = pthread_key_create (&fuse_context_key, free);

        if (error == 0)
            __atomic_store_n (&fuse_context_key_ready, 1, __ATOMIC_RELEASE);
    }

    pthread_mutex_unlock (&fuse_context_key_lock);

    return error;
}

static struct fuse_thread_context * get_thread_context (void)
{
    if (ensure_thread_context_key () != 0)
        return NULL;

    return pthread_getspecific (fuse_context_key);
}

static int acquire_thread_context (struct fuse_thread_context ** out)
{
    int error = ensure_thread_context_key ();
    if (error != 0)
        return error;

    *out = pthread_getspecific (fuse_context_key);
    if (*out != NULL)
        return 0;

    struct fuse_thread_context * thread = calloc (1, sizeof (*thread));
    if (thread == NULL)
        return ENOMEM;

    error = pthread_setspecific (fuse_context_key, thread);
    if (error != 0)
    {
        free (thread);

        return error;
    }

    *out = thread;

    return 0;
}

struct fuse_context_scope
{
    struct fuse_thread_context * thread; // Thread context restored when the scope ends.
    struct fuse_context saved;           // Saved outer callback context.
    int active;                          // Saved outer active state.
};

static void restore_callback_context (const struct fuse_context_scope * scope)
{
    if (scope->thread == NULL)
        return;

    scope->thread->context = scope->saved;
    scope->thread->active = scope->active;
}

static int enter_callback_context (struct fuse * fuse, struct fuse_context_scope * scope)
{
    struct fuse_thread_context * thread;
    const int error = acquire_thread_context (&thread);

    if (error != 0)
        return error;

    scope->thread = thread;
    scope->saved = thread->context;
    scope->active = thread->active;
    memset (&thread->context, 0, sizeof (thread->context));
    thread->context.fuse = fuse;
    thread->context.private_data = fuse->user_data;
    thread->active = 1;

    return 0;
}

/* ----------------------------------------------------------- *
 * Small public helpers                                        *
 * ----------------------------------------------------------- */

int fuse_version (void)
{
    return FUSE_VERSION;
}

const char * fuse_pkgversion (void)
{
    return "3.19.0-aix-usfs"; // todo: parametrize, remove hardcode
}

int fuse_daemonize (const int foreground)
{
    /* Backgrounding is not implemented in this milestone; daemons run in
       the foreground (use shell job control instead). */ // todo: needs to be done
    (void)foreground;

    return 0;
}

struct fuse_context * fuse_get_context (void)
{
    struct fuse_thread_context * thread = get_thread_context ();

    if (thread == NULL)
        return NULL;

    if (!thread->active)
        return NULL;

    return &thread->context;
}

struct fuse_session * fuse_get_session (const struct fuse * fuse)
{
    return &((struct fuse *)(uintptr_t)fuse)->session;
}

void fuse_exit (struct fuse * fuse)
{
    __atomic_store_n (&fuse->session.exited, 1, __ATOMIC_RELEASE);
    notify_session (&fuse->session);
}

/* ----------------------------------------------------------- *
 * Signal handling                                             *
 * ----------------------------------------------------------- */

static void handle_exit_signal (const int signal_number)
{
    const int saved = errno;

    (void)signal_number;

    __atomic_add_fetch (&signal_handlers_active, 1, __ATOMIC_SEQ_CST);
    struct fuse_session * session = __atomic_load_n (&signal_session, __ATOMIC_SEQ_CST);

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

static void restore_signal_actions (const size_t installed_count)
{
    static const int signal_numbers[] = { SIGHUP, SIGINT, SIGTERM, SIGPIPE };

    for (size_t signal_index = installed_count; signal_index != 0; signal_index--)
    {
        const size_t action_index = signal_index - 1u;
        struct sigaction current_action = { 0 };
        if (sigaction (signal_numbers[action_index], NULL, &current_action) != 0)
        {
            fuse_log (FUSE_LOG_ERR, "Failed to inspect signal disposition %d: %s\n", signal_numbers[action_index], strerror (errno));
            continue;
        }

        const struct sigaction * installed_action = &installed_signal_actions[action_index];
        if (current_action.sa_handler != installed_action->sa_handler || current_action.sa_flags != installed_action->sa_flags ||
            memcmp (&current_action.sa_mask, &installed_action->sa_mask, sizeof (current_action.sa_mask)) != 0)
            continue;

        if (sigaction (signal_numbers[action_index], &previous_signal_actions[action_index], NULL) != 0)
            fuse_log (FUSE_LOG_ERR, "Failed to restore signal disposition %d: %s\n", signal_numbers[action_index], strerror (errno));
    }
}

static int install_signal_actions (void)
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
            restore_signal_actions (signal_index + 1u);
            errno = inspect_error;
            return -1;
        }

        const int install_error = errno;
        restore_signal_actions (signal_index);
        errno = install_error;
        return -1;
    }

    return 0;
}

int fuse_set_signal_handlers (struct fuse_session * session)
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

    struct fuse_session * owner = __atomic_load_n (&signal_session, __ATOMIC_SEQ_CST);
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

    if (install_signal_actions () != 0)
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

void fuse_remove_signal_handlers (const struct fuse_session * session)
{
    if (session == NULL)
        return;

    const int lock_error = pthread_mutex_lock (&signal_owner_lock);
    if (lock_error != 0)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to lock signal handler ownership: %s\n", strerror (lock_error));
        return;
    }

    if (__atomic_load_n (&signal_session, __ATOMIC_SEQ_CST) == session)
    {
        restore_signal_actions (sizeof (previous_signal_actions) / sizeof (previous_signal_actions[0]));
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
    return id % FUSE_NODE_BUCKETS;
}

static size_t hash_parent_name (const uint64_t parent, const char * name)
{
    uint64_t hash = parent * FUSE_ID_HASH_MULTIPLIER;

    while (*name != '\0')
        hash = (hash * FUSE_NAME_HASH_MULTIPLIER) + (unsigned char)*name++;

    return hash % FUSE_NODE_BUCKETS;
}

static struct fuse_node * find_node_by_id (const struct fuse * fuse, const uint64_t id)
{
    for (struct fuse_node * node = fuse->by_id[hash_node_id (id)]; node != NULL; node = node->hnext_id)
        if (node->id == id)
            return node;

    return NULL;
}

static int is_backend_stat_valid (const struct stat * metadata);

static void queue_node_for_collection (struct fuse * fuse, struct fuse_node * node)
{
    if (node == NULL)
        return;

    if (node->gc_queued)
        return;

    node->gc_queued = true;
    node->gc_next = fuse->gc_pending;
    fuse->gc_pending = node;
}

static struct fuse_alias * find_alias (const struct fuse * fuse, const uint64_t parent, const char * name)
{
    for (struct fuse_alias * alias = fuse->by_parent_name[hash_parent_name (parent, name)]; alias != NULL; alias = alias->hnext)
        if (alias->parent == parent && strcmp (alias->name, name) == 0)
            return alias;

    return NULL;
}

static struct fuse_node * find_child_node (const struct fuse * fuse, const uint64_t parent, const char * name)
{
    const struct fuse_alias * alias = find_alias (fuse, parent, name);

    return alias != NULL ? alias->node : NULL;
}

static struct fuse_alias * prepare_alias (const uint64_t parent, const char * name)
{
    struct fuse_alias * alias = calloc (1, sizeof (*alias));

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

static struct fuse_node * prepare_node (const uint64_t parent, const char * name)
{
    struct fuse_node * node = calloc (1, sizeof (*node));

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

static void destroy_node (struct fuse_node * node)
{
    if (node == NULL)
        return;

    pthread_mutex_destroy (&node->mutation_lock);

    while (node->aliases != NULL)
    {
        struct fuse_alias * alias = node->aliases;

        node->aliases = alias->next;
        free (alias->name);
        free (alias);
    }

    free (node);
}

static void index_alias (struct fuse * fuse, struct fuse_alias * alias)
{
    struct fuse_node * parent = find_node_by_id (fuse, alias->parent);
    const size_t bucket = hash_parent_name (alias->parent, alias->name);

    alias->hnext = fuse->by_parent_name[bucket];
    fuse->by_parent_name[bucket] = alias;
    alias->attached = 1;
    alias->node->names++;
    alias->node->detached = 0;

    if (parent != NULL)
        parent->ancestor_refs++;
}

static void attach_alias (struct fuse * fuse, struct fuse_node * node, struct fuse_alias * alias)
{
    alias->node = node;
    alias->next = node->aliases;
    node->aliases = alias;
    index_alias (fuse, alias);
}

static void unindex_alias (struct fuse * fuse, struct fuse_alias * alias)
{
    if (!alias->attached)
        return;

    struct fuse_alias ** cursor = &fuse->by_parent_name[hash_parent_name (alias->parent, alias->name)];

    while (*cursor != NULL && *cursor != alias)
        cursor = &(*cursor)->hnext;

    if (*cursor == NULL)
        return;

    *cursor = alias->hnext;
    alias->hnext = NULL;
    alias->attached = 0;
    alias->node->names--;
    alias->node->detached = alias->node->names == 0;

    struct fuse_node * parent = find_node_by_id (fuse, alias->parent);

    if (parent != NULL)
    {
        parent->ancestor_refs--;
        queue_node_for_collection (fuse, parent);
    }
}

static size_t hash_backend_identity (const uint64_t device, const uint64_t inode)
{
    return (size_t)((device * FUSE_ID_HASH_MULTIPLIER + inode) % FUSE_NODE_BUCKETS);
}

static void unindex_node_backend_identity (struct fuse * fuse, struct fuse_node * node)
{
    if (!node->backend_indexed)
        return;

    struct fuse_node ** cursor = &fuse->by_backend[hash_backend_identity (node->backend_dev, node->backend_ino)];

    while (*cursor != NULL && *cursor != node)
        cursor = &(*cursor)->hnext_backend;

    if (*cursor == node)
        *cursor = node->hnext_backend;

    node->hnext_backend = NULL;
    node->backend_indexed = false;
}

static void retire_node_backend_identity (struct fuse * fuse, struct fuse_node * node)
{
    unindex_node_backend_identity (fuse, node);
    node->backend_retired = true;
}

static struct fuse_node * find_node_by_backend_identity (const struct fuse * fuse, const struct stat * metadata)
{
    if (metadata == NULL)
        return NULL;

    if (metadata->st_ino == 0)
        return NULL;

    if (S_ISDIR (metadata->st_mode))
        return NULL;

    const size_t bucket = hash_backend_identity ((uint64_t)metadata->st_dev, (uint64_t)metadata->st_ino);

    for (struct fuse_node * node = fuse->by_backend[bucket]; node != NULL; node = node->hnext_backend)
    {
        if (node->backend_dev != (uint64_t)metadata->st_dev)
            continue;

        if (node->backend_ino == (uint64_t)metadata->st_ino)
            return node;
    }

    return NULL;
}

static int node_backend_identity_matches (const struct fuse_node * node, const struct stat * metadata)
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

static void index_node_backend_identity (struct fuse * fuse, struct fuse_node * node)
{
    if (S_ISDIR (node->backend_type))
        return;

    if (node->backend_ino == 0)
        return;

    if (node->backend_indexed)
        return;

    const size_t bucket = hash_backend_identity (node->backend_dev, node->backend_ino);

    node->hnext_backend = fuse->by_backend[bucket];
    fuse->by_backend[bucket] = node;
    node->backend_indexed = true;
}

static int update_node_backend_identity (struct fuse * fuse, struct fuse_node * node, const struct stat * metadata)
{
    if (metadata == NULL)
        return 0;

    if (!is_backend_stat_valid (metadata))
        return EIO;

    if (!node_backend_identity_matches (node, metadata))
        return EIO;

    const struct fuse_node * other = find_node_by_backend_identity (fuse, metadata);

    if (other != NULL && other != node)
        return EIO;

    node->backend_dev = (uint64_t)metadata->st_dev;
    node->backend_ino = (uint64_t)metadata->st_ino;
    node->backend_type = metadata->st_mode & S_IFMT;
    node->backend_links = (uint64_t)metadata->st_nlink;

    if (metadata->st_nlink == 0)
    {
        retire_node_backend_identity (fuse, node);

        return 0;
    }

    index_node_backend_identity (fuse, node);

    return 0;
}

static void remove_alias (struct fuse * fuse, struct fuse_alias * alias)
{
    struct fuse_node * node = alias->node;
    struct fuse_alias ** cursor = &node->aliases;

    unindex_alias (fuse, alias);

    if (node->backend_links != 0)
        node->backend_links--;

    if (node->backend_links == 0)
        retire_node_backend_identity (fuse, node);

    while (*cursor != NULL && *cursor != alias)
        cursor = &(*cursor)->next;

    if (*cursor == alias)
        *cursor = alias->next;

    free (alias->name);
    free (alias);
    queue_node_for_collection (fuse, node);
}

static int node_has_references (const struct fuse_node * node)
{
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

static void detach_node_aliases (struct fuse * fuse, struct fuse_node * node)
{
    while (node->aliases != NULL)
    {
        struct fuse_alias * alias = node->aliases;

        unindex_alias (fuse, alias);
        node->aliases = alias->next;
        free (alias->name);
        free (alias);
    }
}

static void unindex_node (struct fuse * fuse, const struct fuse_node * node)
{
    struct fuse_node ** cursor = &fuse->by_id[hash_node_id (node->id)];

    while (*cursor != node)
        cursor = &(*cursor)->hnext_id;

    *cursor = node->hnext_id;
    cursor = &fuse->nodes;

    while (*cursor != node)
        cursor = &(*cursor)->next;

    *cursor = node->next;
}

/* All reference changes and reclamation are serialized by node_lock. Each
 * attached alias retains its parent. Removing the last dependent can enqueue
 * another ancestor, so the work list releases the chain without recursion. */
static void collect_unreferenced_nodes (struct fuse * fuse)
{
    while (fuse->gc_pending != NULL)
    {
        struct fuse_node * node = fuse->gc_pending;

        fuse->gc_pending = node->gc_next;
        node->gc_queued = false;

        if (node_has_references (node))
            continue;

        detach_node_aliases (fuse, node);
        retire_node_backend_identity (fuse, node);
        unindex_node (fuse, node);
        destroy_node (node);
    }
}

static void invalidate_handle_directory_cache_locked (struct fuse *, uint64_t, uint64_t);

struct open_handle_state
{
    struct fuse_file_info file_info; // Flags and backend handle shared across open and publication.
    uint64_t nodeid;                 // Wire node identifier to retain at publication.
    uint32_t is_directory;           // Whether directory callbacks and cache invalidation apply.
};

static void publish_client_handle (struct fuse * fuse, struct fuse_handle * handle, const struct open_handle_state * opened)
{
    pthread_mutex_lock (&fuse->dircache_lock);

    if (opened->is_directory)
        invalidate_handle_directory_cache_locked (fuse, opened->nodeid, opened->file_info.fh);

    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_node * node = find_node_by_id (fuse, opened->nodeid);

    handle->nodeid = opened->nodeid;
    handle->fh = opened->file_info.fh;
    handle->flags = opened->file_info.flags;
    handle->isdir = opened->is_directory != 0;

    handle->generation = fuse->handle_generation == UINT64_MAX ? 0 : ++fuse->handle_generation;
    handle->next = fuse->handles;
    fuse->handles = handle;

    if (node != NULL)
        node->open_refs++;

    pthread_mutex_unlock (&fuse->node_lock);
    pthread_mutex_unlock (&fuse->dircache_lock);
}

static void relink_alias_with_prepared_name (struct fuse * fuse, struct fuse_alias * alias, const uint64_t parent, char * name)
{
    unindex_alias (fuse, alias);
    free (alias->name);
    alias->name = name;
    alias->parent = parent;
    index_alias (fuse, alias);
}

static struct fuse_node * select_node_for_adoption (
    const struct fuse * fuse,
    struct fuse_node * prepared,
    const struct stat * metadata,
    int * existing_name
)
{
    const struct fuse_alias * alias = prepared->aliases;
    struct fuse_node * node = find_child_node (fuse, alias->parent, alias->name);

    *existing_name = node != NULL;

    if (metadata != NULL && metadata->st_nlink == 0)
        return NULL;

    if (node == NULL)
        node = find_node_by_backend_identity (fuse, metadata);

    return node != NULL ? node : prepared;
}

static int prepare_node_adoption (struct fuse * fuse, struct fuse_node * node, const struct fuse_node * prepared, const struct stat * metadata)
{
    if (node->lookup_refs == UINT64_MAX)
        return EOVERFLOW;

    if (node == prepared && fuse->next_id == UINT64_MAX)
        return EOVERFLOW;

    return update_node_backend_identity (fuse, node, metadata);
}

static void publish_node_adoption (struct fuse * fuse, struct fuse_node * node, struct fuse_node ** prepared, const int existing_name)
{
    struct fuse_alias * alias = (*prepared)->aliases;

    if (node == *prepared)
    {
        node->id = fuse->next_id++;
        node->next = fuse->nodes;
        fuse->nodes = node;

        const size_t bucket = hash_node_id (node->id);

        node->hnext_id = fuse->by_id[bucket];
        fuse->by_id[bucket] = node;
        alias->node = node;
        index_alias (fuse, alias);
        *prepared = NULL;
    }
    else if (!existing_name)
    {
        (*prepared)->aliases = NULL;
        attach_alias (fuse, node, alias);
    }

    node->lookup_refs++;
}

static struct fuse_node * adopt_prepared_node (struct fuse * fuse, struct fuse_node ** prepared, const struct stat * metadata, int * error)
{
    int existing_name;
    struct fuse_node * node = select_node_for_adoption (fuse, *prepared, metadata, &existing_name);

    if (node == NULL)
    {
        *error = EIO;

        return NULL;
    }

    *error = prepare_node_adoption (fuse, node, *prepared, metadata);

    if (*error != 0)
        return NULL;

    publish_node_adoption (fuse, node, prepared, existing_name);

    return node;
}

static struct fuse_node * find_or_create_node (struct fuse * fuse, const uint64_t parent, const char * name, const struct stat * metadata)
{
    struct fuse_node * prepared = prepare_node (parent, name);
    int error;

    if (prepared == NULL)
    {
        errno = ENOMEM;

        return NULL;
    }

    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_node * node = adopt_prepared_node (fuse, &prepared, metadata, &error);

    pthread_mutex_unlock (&fuse->node_lock);

    destroy_node (prepared);

    if (node == NULL)
        errno = error;

    return node;
}

static struct fuse_alias * select_node_path_alias (const struct fuse_node * node)
{
    for (struct fuse_alias * alias = node->aliases; alias != NULL; alias = alias->next)
        if (alias->attached)
            return alias;

    return node->aliases; /* informational former path for retained handles */
}

static int collect_node_path_components (const struct fuse * fuse, uint64_t nodeid, const char * names[USFS_MAX_DEPTH], int * depth)
{
    *depth = 0;

    while (nodeid != USFS_ROOT_ID)
    {
        const struct fuse_node * node = find_node_by_id (fuse, nodeid);

        if (node == NULL)
            return -ENOENT;

        if (*depth >= USFS_MAX_DEPTH)
            return -ENOENT;

        const struct fuse_alias * alias = select_node_path_alias (node);

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

static int build_node_path_locked (const struct fuse * fuse, const uint64_t nodeid, char * path_buffer, const size_t buffer_size)
{
    const char * names[USFS_MAX_DEPTH];
    int depth;
    const int rc = collect_node_path_components (fuse, nodeid, names, &depth);

    if (rc != 0)
        return rc;

    return join_node_path_components (names, depth, path_buffer, buffer_size);
}

static int build_node_path (struct fuse * fuse, const uint64_t id, char * path_buffer, const size_t buffer_size)
{
    pthread_mutex_lock (&fuse->node_lock);
    const int result = build_node_path_locked (fuse, id, path_buffer, buffer_size);

    pthread_mutex_unlock (&fuse->node_lock);

    return result;
}

struct resolved_handle_path
{
    char buffer[USFS_PATH_MAX]; // Storage for the resolved node path.
    const char * path;          // Points into buffer, or NULL for a detached node.
};

static int resolve_node_handle_path (struct fuse * fuse, const uint64_t id, const uint64_t fh, struct resolved_handle_path * resolved)
{
    int result;

    pthread_mutex_lock (&fuse->node_lock);
    const struct fuse_node * node = find_node_by_id (fuse, id);

    resolved->path = resolved->buffer;

    if (node != NULL && node->detached)
    {
        resolved->path = NULL;
        result = fh != 0 ? 0 : -ESTALE;
    }
    else
    {
        result = build_node_path_locked (fuse, id, resolved->buffer, sizeof (resolved->buffer));
    }

    pthread_mutex_unlock (&fuse->node_lock);

    return result;
}

static uint64_t get_node_parent_id_locked (const struct fuse * fuse, const uint64_t nodeid)
{
    const struct fuse_node * node = find_node_by_id (fuse, nodeid);

    if (node == NULL)
        return USFS_ROOT_ID;

    const struct fuse_alias * alias = select_node_path_alias (node);

    if (alias == NULL)
        return USFS_ROOT_ID;

    return alias->parent;
}

static uint64_t get_node_parent_id (struct fuse * fuse, const uint64_t nodeid)
{
    pthread_mutex_lock (&fuse->node_lock);
    const uint64_t parent = get_node_parent_id_locked (fuse, nodeid);

    pthread_mutex_unlock (&fuse->node_lock);

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
    attr->blksize = (uint32_t)(metadata->st_blksize != 0 ? metadata->st_blksize : FUSE_DEFAULT_BLOCK_SIZE);
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

    if (metadata->st_atim.tv_nsec < 0 || metadata->st_atim.tv_nsec >= FUSE_NANOSECONDS_PER_SECOND)
        return false;

    if (metadata->st_mtim.tv_nsec < 0 || metadata->st_mtim.tv_nsec >= FUSE_NANOSECONDS_PER_SECOND)
        return false;

    if (metadata->st_ctim.tv_nsec < 0 || metadata->st_ctim.tv_nsec >= FUSE_NANOSECONDS_PER_SECOND)
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

static int resolve_session_result (const struct fuse * fuse, const int result)
{
    const int fatal = __atomic_load_n (&fuse->session.fatal_error, __ATOMIC_ACQUIRE);

    return fatal != 0 ? -fatal : result;
}

static void fail_session (struct fuse * fuse, int error)
{
    int expected = 0;

    if (error <= 0)
        error = EIO;

    (void)__atomic_compare_exchange_n (&fuse->session.fatal_error, &expected, error, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    fuse_exit (fuse);
}

static char * get_reply_buffer (const struct fuse * fuse)
{
    const struct fuse_thread_context * thread = get_thread_context ();

    if (thread == NULL)
        return fuse->writebuf;

    if (thread->writebuf == NULL)
        return fuse->writebuf;

    return thread->writebuf;
}

static int send_reply (const struct request_context * context, const int error, const void * body, uint32_t body_len)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;
    int reply_error = error;

    char * write_buf = get_reply_buffer (fuse);
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

    pthread_mutex_lock (&fuse->reply_lock);
    const int result = write_all_bytes (fuse->fd, write_buf, out->len);
    if (result != 0)
        fail_session (fuse, errno);

    pthread_mutex_unlock (&fuse->reply_lock);

    return result;
}

/* ----------------------------------------------------------- *
 * Request dispatch                                            *
 * ----------------------------------------------------------- */

static void set_request_context (struct fuse * fuse, const struct usfs_in_hdr * request_header)
{
    struct fuse_context * context = fuse_get_context ();

    context->fuse = fuse;
    context->uid = (uid_t)request_header->uid;
    context->gid = (gid_t)request_header->gid;
    context->pid = (pid_t)request_header->pid;
    context->private_data = fuse->user_data;
    context->umask = 0;
}

static int read_backend_attributes (const struct fuse * fuse, const char * path, struct stat * metadata, struct fuse_file_info * file_info)
{
    if (fuse->ops.getattr == NULL)
        return -ENOSYS;

    memset (metadata, 0, sizeof (*metadata));

    return fuse->ops.getattr (path, metadata, file_info);
}

static int read_path_attributes (const struct fuse * fuse, const char * path, struct stat * metadata)
{
    return read_backend_attributes (fuse, path, metadata, NULL);
}

static void invalidate_node_directory_cache (struct fuse * fuse, uint64_t nodeid);

struct path_buffer
{
    char data[USFS_PATH_MAX]; // Storage for a full backend path, including its terminator.
};

static int build_child_path (struct fuse * fuse, const uint64_t parent, const char * name, struct path_buffer * path)
{
    const size_t name_length = strlen (name);

    if (name_length + sizeof ("/") >= sizeof (path->data))
        return -ENAMETOOLONG;

    const int rc = build_node_path (fuse, parent, path->data, sizeof (path->data) - name_length - sizeof ("/"));
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
    convert_stat_to_attributes (metadata, nodeid, &entry.attr);

    return send_reply (context, 0, &entry, (uint32_t)sizeof (entry));
}

static int resolve_relative_lookup_path_locked (const struct fuse * fuse, const char * name, uint64_t * nodeid, struct path_buffer * path)
{
    const struct fuse_node * node = find_node_by_id (fuse, *nodeid);

    if (*nodeid != USFS_ROOT_ID)
    {
        if (node == NULL)
            return -ESTALE;

        if (node->detached)
            return -ESTALE;

        if (strcmp (name, "..") == 0)
            *nodeid = select_node_path_alias (node)->parent;
    }

    return build_node_path_locked (fuse, *nodeid, path->data, sizeof (path->data));
}

static int retain_lookup_reference_locked (struct fuse * fuse, const uint64_t nodeid, const struct stat * metadata)
{
    struct fuse_node * node = find_node_by_id (fuse, nodeid);

    if (nodeid != USFS_ROOT_ID)
    {
        if (node == NULL)
            return EIO;

        const int rc = update_node_backend_identity (fuse, node, metadata);

        if (rc != 0)
            return rc;
    }

    uint64_t * references = nodeid == USFS_ROOT_ID ? &fuse->root_lookup_refs : &node->lookup_refs;

    if (*references == UINT64_MAX)
        return EOVERFLOW;

    ++*references;

    return 0;
}

static int handle_lookup_self_or_parent_request (const struct request_context * context, const char * name)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct path_buffer path = { 0 };
    uint64_t nodeid = request_header->nodeid;

    pthread_mutex_lock (&fuse->node_lock);
    int rc = resolve_relative_lookup_path_locked (fuse, name, &nodeid, &path);

    pthread_mutex_unlock (&fuse->node_lock);

    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct stat metadata;

    rc = read_path_attributes (fuse, path.data, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (!is_backend_stat_valid (&metadata))
        return send_reply (context, EIO, NULL, 0);

    if (!S_ISDIR (metadata.st_mode))
        return send_reply (context, EIO, NULL, 0);

    pthread_mutex_lock (&fuse->node_lock);
    rc = retain_lookup_reference_locked (fuse, nodeid, &metadata);
    pthread_mutex_unlock (&fuse->node_lock);

    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return send_lookup_entry_reply (context, nodeid, &metadata);
}

static int handle_lookup_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    const char * name = payload;

    if (!is_payload_null_terminated (name, payload_length))
        return send_reply (context, EINVAL, NULL, 0);

    if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
        return handle_lookup_self_or_parent_request (context, name);

    struct path_buffer path = { 0 };

    int rc = build_child_path (fuse, request_header->nodeid, name, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct stat metadata;

    rc = read_path_attributes (fuse, path.data, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    const struct fuse_node * node = find_or_create_node (fuse, request_header->nodeid, name, &metadata);
    if (node == NULL)
        return send_reply (context, errno, NULL, 0);

    return send_lookup_entry_reply (context, node->id, &metadata);
}

static int refresh_node_identity (struct fuse * fuse, const uint64_t nodeid, const struct stat * metadata);

struct fid_identity
{
    char path[USFS_PATH_MAX];   // Current alias copied out of the node table.
    const char * callback_path; // Current path, or NULL when all known aliases disappeared.
    uint64_t device;            // Backend device identity supplied to export_id.
    uint64_t inode;             // Backend inode identity supplied to export_id.
    mode_t type;                // Backend object type supplied to export_id.
};

static int prepare_root_fid_identity (struct fuse * fuse, struct fid_identity * identity)
{
    struct stat metadata;
    const int rc = read_path_attributes (fuse, "/", &metadata);
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

static int copy_cached_fid_identity (
    struct fuse * fuse,
    const uint64_t nodeid,
    struct fid_identity * identity
)
{
    pthread_mutex_lock (&fuse->node_lock);

    struct fuse_node * node = find_node_by_id (fuse, nodeid);
    if (node == NULL || node->backend_retired || node->backend_links == 0)
    {
        pthread_mutex_unlock (&fuse->node_lock);
        return ESTALE;
    }

    identity->device = node->backend_dev;
    identity->inode = node->backend_ino;
    identity->type = node->backend_type;
    identity->callback_path = NULL;

    int rc = 0;
    if (!node->detached)
    {
        rc = build_node_path_locked (fuse, nodeid, identity->path, sizeof (identity->path));
        if (rc == 0)
            identity->callback_path = identity->path;
    }

    pthread_mutex_unlock (&fuse->node_lock);

    if (rc != 0)
        return -rc;

    if (identity->callback_path == NULL)
        return identity->inode != 0 && identity->type != 0 ? 0 : ESTALE;

    return 0;
}

static int refresh_named_fid_identity (
    struct fuse * fuse,
    const uint64_t nodeid,
    struct fid_identity * identity
)
{
    struct stat metadata;
    int rc = read_path_attributes (fuse, identity->callback_path, &metadata);
    if (rc != 0)
        return rc == -ENOENT ? ESTALE : -rc;

    if (!is_backend_stat_valid (&metadata))
        return EIO;

    if (metadata.st_nlink == 0)
        return ESTALE;

    rc = refresh_node_identity (fuse, nodeid, &metadata);
    if (rc != 0)
        return ESTALE;

    identity->device = (uint64_t)metadata.st_dev;
    identity->inode = (uint64_t)metadata.st_ino;
    identity->type = metadata.st_mode & S_IFMT;

    return 0;
}

static int prepare_fid_identity (
    struct fuse * fuse,
    const uint64_t nodeid,
    struct fid_identity * identity
)
{
    if (nodeid == USFS_ROOT_ID)
        return prepare_root_fid_identity (fuse, identity);

    const int rc = copy_cached_fid_identity (fuse, nodeid, identity);
    if (rc != 0)
        return rc;

    if (identity->callback_path == NULL)
        return 0;

    return refresh_named_fid_identity (fuse, nodeid, identity);
}

static int handle_fid_request (const struct request_context * context, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;

    if (payload_length != 0)
        return send_reply (context, EINVAL, NULL, 0);

    if (fuse->ops.export_id == NULL || fuse->ops.resolve_id == NULL)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    struct fid_identity identity = { 0 };
    const int rc = prepare_fid_identity (fuse, context->header->nodeid, &identity);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    struct usfs_fid_out reply = { 0 };
    const int callback_result = fuse->ops.export_id (identity.callback_path, identity.device, identity.inode, identity.type, &reply.token);
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

static void release_resolved_path_references (
    struct fuse * fuse,
    const uint64_t nodeids[USFS_MAX_DEPTH],
    const size_t count
)
{
    pthread_mutex_lock (&fuse->node_lock);

    for (size_t node_index = 0; node_index < count; node_index++)
    {
        struct fuse_node * node = find_node_by_id (fuse, nodeids[node_index]);
        if (node == NULL)
            continue;

        node->lookup_refs--;
        queue_node_for_collection (fuse, node);
    }

    collect_unreferenced_nodes (fuse);
    pthread_mutex_unlock (&fuse->node_lock);
}

static int adopt_resolved_identity_path (
    struct fuse * fuse,
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
            const int rc = read_path_attributes (fuse, path, &metadata);
            if (rc != 0)
                error = rc == -ENOENT ? ESTALE : -rc;
            else if (!is_backend_stat_valid (&metadata) || !S_ISDIR (metadata.st_mode))
                error = ESTALE;
        }

        if (error == 0)
        {
            const struct fuse_node * node = find_or_create_node (fuse, parent_id, component, &metadata);
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
        release_resolved_path_references (fuse, temporary_nodeids, adopted_count);
        return error;
    }

    *nodeid = parent_id;
    release_resolved_path_references (fuse, temporary_nodeids, adopted_count - 1u);

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
static int resolve_vget_identity (
    struct fuse * fuse,
    const uint64_t token,
    char path[USFS_PATH_MAX],
    struct stat * metadata
)
{
    memset (path, UCHAR_MAX, USFS_PATH_MAX);

    const int resolve_result = fuse->ops.resolve_id (token, path, USFS_PATH_MAX);
    const int resolve_error = normalize_callback_error (resolve_result);
    if (resolve_error != 0)
        return resolve_error;

    if (memchr (path, '\0', USFS_PATH_MAX) == NULL)
        return EIO;

    const int path_error = validate_resolved_identity_path (path);
    if (path_error != 0)
        return path_error;

    const int attribute_result = read_path_attributes (fuse, path, metadata);
    if (attribute_result != 0)
        return attribute_result == -ENOENT ? ESTALE : -attribute_result;

    if (!is_backend_stat_valid (metadata))
        return EIO;

    if (metadata->st_nlink == 0)
        return ESTALE;

    uint64_t current_token = 0;
    const int export_result = fuse->ops.export_id (
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
static int materialize_vget_node (
    struct fuse * fuse,
    char path[USFS_PATH_MAX],
    const struct stat * metadata,
    uint64_t * nodeid
)
{
    *nodeid = USFS_ROOT_ID;

    if (strcmp (path, "/") != 0)
        return adopt_resolved_identity_path (fuse, path, metadata, nodeid);

    if (!S_ISDIR (metadata->st_mode))
        return ESTALE;

    pthread_mutex_lock (&fuse->node_lock);

    if (fuse->root_lookup_refs == UINT64_MAX)
    {
        pthread_mutex_unlock (&fuse->node_lock);
        return EOVERFLOW;
    }

    fuse->root_lookup_refs++;
    pthread_mutex_unlock (&fuse->node_lock);

    return 0;
}

static int handle_vget_request (
    const struct request_context * context,
    const char * payload,
    const uint32_t payload_length
)
{
    struct fuse * fuse = context->fuse;
    struct usfs_vget_in request;

    int rc = parse_vget_request (context, payload, payload_length, &request);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (fuse->ops.resolve_id == NULL || fuse->ops.export_id == NULL)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    char path[USFS_PATH_MAX];
    struct stat metadata;

    rc = resolve_vget_identity (fuse, request.token, path, &metadata);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    uint64_t nodeid;
    rc = materialize_vget_node (fuse, path, &metadata, &nodeid);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    struct usfs_vget_out reply = { 0 };
    reply.token = request.token;
    reply.entry.nodeid = nodeid;
    convert_stat_to_attributes (&metadata, nodeid, &reply.entry.attr);

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int read_handle_attributes (const struct fuse * fuse, const char * path, const uint64_t handle, struct stat * metadata)
{
    if (handle == 0)
        return read_path_attributes (fuse, path, metadata);

    struct fuse_file_info file_info = { 0 };

    file_info.fh = handle;

    return read_backend_attributes (fuse, path, metadata, &file_info);
}

static int refresh_node_identity (struct fuse * fuse, const uint64_t nodeid, const struct stat * metadata)
{
    if (nodeid == USFS_ROOT_ID)
        return 0;

    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_node * node = find_node_by_id (fuse, nodeid);
    const int rc = node != NULL ? update_node_backend_identity (fuse, node, metadata) : EIO;

    pthread_mutex_unlock (&fuse->node_lock);

    return rc;
}

static int send_node_attributes (const struct request_context * context, const struct stat * metadata)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_attr_out reply = { 0 };

    convert_stat_to_attributes (metadata, request_header->nodeid, &reply.attr);

    reply.parent = request_header->nodeid == USFS_ROOT_ID ? USFS_ROOT_ID : get_node_parent_id (fuse, request_header->nodeid);

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int handle_getattr_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_getattr_in request = { 0 };

    if (payload_length >= sizeof (request))
        memcpy (&request, payload, sizeof (request));

    struct resolved_handle_path resolved = { 0 };

    int rc = resolve_node_handle_path (fuse, request_header->nodeid, request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct stat metadata;

    rc = read_handle_attributes (fuse, resolved.path, request.fh, &metadata);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (!is_backend_stat_valid (&metadata))
        return send_reply (context, EIO, NULL, 0);

    rc = refresh_node_identity (fuse, request_header->nodeid, &metadata);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return send_node_attributes (context, &metadata);
}

static int handle_readlink_request (const struct request_context * context)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    char path[USFS_PATH_MAX];

    int rc = build_node_path (fuse, request_header->nodeid, path, sizeof (path));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (fuse->ops.readlink == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    char link[USFS_MAX_LINK];

    memset (link, UCHAR_MAX, sizeof (link));

    rc = fuse->ops.readlink (path, link, sizeof (link));
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
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    if (fuse->ops.statfs == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    char path[USFS_PATH_MAX];

    int rc = build_node_path (fuse, request_header->nodeid, path, sizeof (path));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct statvfs metadata = { 0 };

    rc = fuse->ops.statfs (path, &metadata);
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
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_mkdir_in mkdir_request;

    if (payload_length < sizeof (mkdir_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&mkdir_request, payload, sizeof (mkdir_request));

    const char * name = payload + sizeof (mkdir_request);

    if (!is_payload_null_terminated (name, payload_length - (uint32_t)sizeof (mkdir_request)))
        return send_reply (context, EINVAL, NULL, 0);

    if (fuse->ops.mkdir == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };

    int rc = build_child_path (fuse, request_header->nodeid, name, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = fuse->ops.mkdir (path.data, (mode_t)mkdir_request.mode);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    invalidate_node_directory_cache (fuse, request_header->nodeid);

    return send_reply (context, 0, NULL, 0);
}

struct create_resources
{
    const char * name;                // Borrowed name in the validated request payload.
    struct path_buffer path;          // Resolved backend path for creation and failure cleanup.
    struct open_handle_state opened;  // Backend handle state to publish after node adoption.
    struct fuse_node * prepared_node; // Owned reservation until adoption or destruction.
    struct fuse_handle * handle;      // Owned client handle until publication or failure cleanup.
};

static void read_created_file_attributes (
    const struct fuse * fuse,
    const struct usfs_create_in * request,
    struct create_resources * resources,
    struct stat * metadata
)
{
    const int rc = read_backend_attributes (fuse, resources->path.data, metadata, &resources->opened.file_info);
    if (rc == 0 && is_backend_stat_valid (metadata))
        return;

    memset (metadata, 0, sizeof (*metadata));
    metadata->st_mode = S_IFREG | ((mode_t)request->mode & FUSE_CREATE_PERMISSION_MASK);
    metadata->st_nlink = 1;
    metadata->st_uid = fuse_get_context ()->uid;
    metadata->st_gid = fuse_get_context ()->gid;
    metadata->st_blksize = FUSE_DEFAULT_BLOCK_SIZE;
}

static int finish_file_creation (const struct request_context * context, const struct usfs_create_in * request, struct create_resources * resources)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct stat metadata;
    int rc;

    read_created_file_attributes (fuse, request, resources, &metadata);

    pthread_mutex_lock (&fuse->node_lock);
    const struct fuse_node * node = adopt_prepared_node (fuse, &resources->prepared_node, &metadata, &rc);

    pthread_mutex_unlock (&fuse->node_lock);
    destroy_node (resources->prepared_node);

    if (node == NULL)
    {
        if (fuse->ops.release != NULL)
            fuse->ops.release (resources->path.data, &resources->opened.file_info);

        free (resources->handle);
        fail_session (fuse, rc);

        return send_reply (context, rc, NULL, 0);
    }

    resources->opened.nodeid = node->id;
    publish_client_handle (fuse, resources->handle, &resources->opened);

    invalidate_node_directory_cache (fuse, request_header->nodeid);

    struct usfs_create_out create_reply = { 0 };

    create_reply.nodeid = node->id;
    create_reply.fh = resources->opened.file_info.fh;
    convert_stat_to_attributes (&metadata, node->id, &create_reply.attr);

    return send_reply (context, 0, &create_reply, (uint32_t)sizeof (create_reply));
}

static int create_prepared_file (const struct request_context * context, const struct usfs_create_in * request, struct create_resources * resources)
{
    struct fuse * fuse = context->fuse;

    const int rc = fuse->ops.create (resources->path.data, (mode_t)request->mode, &resources->opened.file_info);
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
    resources->opened.file_info.flags = translate_open_flags (request->flags);

    if (resources->opened.file_info.flags < 0)
        return -resources->opened.file_info.flags;

    resources->name = payload + sizeof (*request);

    if (!is_payload_null_terminated (resources->name, payload_length - (uint32_t)sizeof (*request)))
        return EINVAL;

    return 0;
}

static int prepare_create_resources (const struct fuse * fuse, const uint64_t parent, struct create_resources * resources)
{
    if (fuse->next_id == UINT64_MAX)
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
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_create_in request;
    struct create_resources resources = { 0 };

    int rc = parse_create_request (payload, payload_length, &request, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (fuse->ops.create == NULL)
        return send_reply (context, EROFS, NULL, 0);

    rc = build_child_path (fuse, request_header->nodeid, resources.name, &resources.path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = prepare_create_resources (fuse, request_header->nodeid, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return create_prepared_file (context, &request, &resources);
}

static void build_create_attr_initial_stat (const struct usfs_create_attr_in * request, struct stat * initial)
{
    memset (initial, 0, sizeof (*initial));
    initial->st_mode = S_IFREG | request->attr.mode;
    initial->st_uid = request->attr.valid & USFS_SET_UID ? request->attr.uid : fuse_get_context ()->uid;
    initial->st_gid = request->attr.valid & USFS_SET_GID ? request->attr.gid : fuse_get_context ()->gid;
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
    resources->opened.file_info.flags =
        request->activation != USFS_CREATE_OPEN && request->flags == 0 ? O_RDONLY : translate_open_flags (request->flags);

    if (resources->opened.file_info.flags < 0)
        return -resources->opened.file_info.flags;

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
    const struct fuse * fuse,
    const struct usfs_create_attr_in * request,
    const uint64_t parent,
    struct create_resources * resources
)
{
    resources->prepared_node = NULL;
    resources->handle = NULL;

    if (request->activation != USFS_CREATE_DEFAULT)
    {
        if (fuse->next_id == UINT64_MAX)
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

static int adopt_created_attributes (struct fuse * fuse, const struct stat * metadata, struct fuse_node ** prepared, struct fuse_node ** node)
{
    if (!is_backend_stat_valid (metadata))
        return EIO;

    if (!S_ISREG (metadata->st_mode))
        return EIO;

    if (*prepared == NULL)
        return 0;

    int rc;

    pthread_mutex_lock (&fuse->node_lock);
    *node = adopt_prepared_node (fuse, prepared, metadata, &rc);
    pthread_mutex_unlock (&fuse->node_lock);

    return rc;
}

static int finish_attribute_creation (
    const struct request_context * context,
    const struct usfs_create_attr_in * request,
    struct create_resources * resources,
    const struct stat * metadata
)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct fuse_node * node = NULL;
    const int rc = adopt_created_attributes (fuse, metadata, &resources->prepared_node, &node);

    destroy_node (resources->prepared_node);

    if (rc != 0)
    {
        if (request->activation == USFS_CREATE_OPEN && fuse->ops.release != NULL)
            fuse->ops.release (resources->path.data, &resources->opened.file_info);

        fail_session (fuse, rc);
        free (resources->handle);

        return send_reply (context, rc, NULL, 0);
    }

    if (resources->handle != NULL)
    {
        resources->opened.nodeid = node->id;
        publish_client_handle (fuse, resources->handle, &resources->opened);
    }

    invalidate_node_directory_cache (fuse, request_header->nodeid);

    struct usfs_create_out reply = { 0 };

    reply.nodeid = node != NULL ? node->id : 0;
    reply.fh = resources->opened.file_info.fh;
    convert_stat_to_attributes (metadata, USFS_ROOT_ID, &reply.attr);

    return send_reply (context, 0, &reply, sizeof (reply));
}

static int execute_prepared_create_attr (
    const struct request_context * context,
    const struct usfs_create_attr_in * request,
    struct create_resources * resources
)
{
    struct fuse * fuse = context->fuse;

    struct stat initial;
    struct stat result;

    build_create_attr_initial_stat (request, &initial);
    memset (&result, 0, sizeof (result));
    const int rc = fuse->ops.create_attr (
        resources->path.data,
        &initial,
        request->attr.valid,
        (enum fuse_create_activation)request->activation,
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
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_create_attr_in create_request;
    struct create_resources resources = { 0 };

    int rc = parse_create_attr_request (payload, payload_length, &create_request, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (fuse->ops.create_attr == NULL)
        return handle_create_attr_fallback (context, &create_request, resources.name, payload_length - (uint32_t)sizeof (create_request));

    rc = build_child_path (fuse, request_header->nodeid, resources.name, &resources.path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = prepare_create_attr_resources (fuse, &create_request, request_header->nodeid, &resources);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return execute_prepared_create_attr (context, &create_request, &resources);
}

static void publish_removed_entry (struct fuse * fuse, const uint64_t parent, const char * name)
{
    uint64_t removed_id = 0;

    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_alias * removed = find_alias (fuse, parent, name);

    if (removed != NULL)
    {
        removed_id = removed->node->id;
        remove_alias (fuse, removed);
    }

    pthread_mutex_unlock (&fuse->node_lock);

    if (removed_id != 0)
        invalidate_node_directory_cache (fuse, removed_id);

    invalidate_node_directory_cache (fuse, parent);
}

static int handle_remove_entry_request (
    const struct request_context * context,
    const char * payload,
    const uint32_t payload_length,
    int (*op) (const char *)
)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    if (!is_payload_null_terminated (payload, payload_length))
        return send_reply (context, EINVAL, NULL, 0);

    if (op == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };
    int rc = build_child_path (fuse, request_header->nodeid, payload, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = op (path.data);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    publish_removed_entry (fuse, request_header->nodeid, payload);

    return send_reply (context, 0, NULL, 0);
}

static int handle_unlink_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;

    return handle_remove_entry_request (context, payload, payload_length, fuse->ops.unlink);
}

static int handle_rmdir_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;

    return handle_remove_entry_request (context, payload, payload_length, fuse->ops.rmdir);
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

static int relink_renamed_entry_locked (struct fuse * fuse, const struct rename_operation * operation, char * prepared_name)
{
    struct fuse_alias * alias = find_alias (fuse, operation->old_parent, operation->old_name);

    if (alias == NULL)
        return false;

    struct fuse_alias * replaced = find_alias (fuse, operation->request.newparent, operation->new_name);

    if (replaced != NULL)
    {
        if (replaced->node == alias->node)
            return false;

        remove_alias (fuse, replaced);
    }

    relink_alias_with_prepared_name (fuse, alias, operation->request.newparent, prepared_name);

    return true;
}

static void publish_renamed_entry (struct fuse * fuse, const struct rename_operation * operation, char * prepared_name)
{
    pthread_mutex_lock (&fuse->node_lock);
    const int name_adopted = relink_renamed_entry_locked (fuse, operation, prepared_name);

    pthread_mutex_unlock (&fuse->node_lock);

    if (!name_adopted)
        free (prepared_name);
}

static int rename_entry_and_reply (const struct request_context * context, const struct rename_operation * operation)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    char * prepared_name = strdup (operation->new_name);

    if (prepared_name == NULL)
        return send_reply (context, ENOMEM, NULL, 0);

    const int rc = fuse->ops.rename (operation->old_path.data, operation->new_path.data, 0);
    if (rc != 0)
    {
        free (prepared_name);

        return send_reply (context, -rc, NULL, 0);
    }

    publish_renamed_entry (fuse, operation, prepared_name);

    invalidate_node_directory_cache (fuse, request_header->nodeid);

    if (operation->request.newparent != request_header->nodeid)
        invalidate_node_directory_cache (fuse, operation->request.newparent);

    return send_reply (context, 0, NULL, 0);
}

static int handle_rename_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct rename_operation operation = { 0 };

    operation.old_parent = request_header->nodeid;

    int rc = parse_rename_request (payload, payload_length, &operation);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (fuse->ops.rename == NULL)
        return send_reply (context, EROFS, NULL, 0);

    rc = build_child_path (fuse, operation.old_parent, operation.old_name, &operation.old_path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = build_child_path (fuse, operation.request.newparent, operation.new_name, &operation.new_path);
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
static int has_requested_setattr_callbacks (const struct fuse * fuse, const uint32_t valid)
{
    if ((valid & USFS_SET_MODE) != 0 && fuse->ops.chmod == NULL)
        return false;

    if ((valid & (USFS_SET_UID | USFS_SET_GID)) != 0 && fuse->ops.chown == NULL)
        return false;

    if ((valid & USFS_SET_SIZE) != 0 && fuse->ops.truncate == NULL)
        return false;

    if ((valid & (USFS_SET_ATIME | USFS_SET_MTIME)) != 0 && fuse->ops.utimens == NULL)
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
        times[ATTRIBUTE_ACCESS_TIME].tv_nsec = UTIME_NOW;
        times[ATTRIBUTE_MODIFICATION_TIME].tv_nsec = UTIME_NOW;

        return;
    }

    times[ATTRIBUTE_ACCESS_TIME].tv_sec = (time_t)request->atime;
    times[ATTRIBUTE_ACCESS_TIME].tv_nsec = (long)request->atimensec;
    times[ATTRIBUTE_MODIFICATION_TIME].tv_sec = (time_t)request->mtime;
    times[ATTRIBUTE_MODIFICATION_TIME].tv_nsec = (long)request->mtimensec;

    if ((request->valid & USFS_SET_ATIME) == 0)
        times[ATTRIBUTE_ACCESS_TIME].tv_nsec = UTIME_OMIT;
    if ((request->valid & USFS_SET_MTIME) == 0)
        times[ATTRIBUTE_MODIFICATION_TIME].tv_nsec = UTIME_OMIT;
}

static int apply_requested_owner (
    const struct fuse * fuse,
    const char * path,
    const struct usfs_setattr_in * request,
    struct fuse_file_info * file_info
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

    return fuse->ops.chown (path, uid, gid, file_info);
}

static int apply_requested_times (
    const struct fuse * fuse,
    const char * path,
    const struct usfs_setattr_in * request,
    struct fuse_file_info * file_info
)
{
    if ((request->valid & (USFS_SET_ATIME | USFS_SET_MTIME)) == 0)
        return 0;

    struct timespec requested_times[ATTRIBUTE_TIME_COUNT];

    build_setattr_times (request, requested_times);

    return fuse->ops.utimens (path, requested_times, file_info);
}

static int apply_requested_attributes (
    const struct fuse * fuse,
    const char * path,
    const struct usfs_setattr_in * request,
    struct fuse_file_info * file_info
)
{
    if (request->valid & USFS_SET_MODE)
    {
        const int rc = fuse->ops.chmod (path, (mode_t)request->mode, file_info);

        if (rc != 0)
            return rc;
    }

    const int rc = apply_requested_owner (fuse, path, request, file_info);

    if (rc != 0)
        return rc;

    if (request->valid & USFS_SET_SIZE)
    {
        const int truncate_error = fuse->ops.truncate (path, (off_t)request->size, file_info);

        if (truncate_error != 0)
            return truncate_error;
    }

    return apply_requested_times (fuse, path, request, file_info);
}

static int set_attributes_and_reply (const struct request_context * context, const char * path, const struct usfs_setattr_in * request)
{
    struct fuse * fuse = context->fuse;

    struct fuse_file_info file_info;
    struct fuse_file_info * file_info_pointer = NULL;

    if (request->fh != 0)
    {
        memset (&file_info, 0, sizeof (file_info));
        file_info.fh = request->fh;
        file_info_pointer = &file_info;
    }

    if (!has_requested_setattr_callbacks (fuse, request->valid))
        return send_reply (context, EROFS, NULL, 0);

    const int rc = apply_requested_attributes (fuse, path, request, file_info_pointer);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    return send_reply (context, 0, NULL, 0);
}

static int handle_setattr_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_setattr_in setattr_request;

    if (payload_length < sizeof (setattr_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&setattr_request, payload, sizeof (setattr_request));

    if (setattr_request.valid & USFS_SET_CTIME)
        return send_reply (context, EOPNOTSUPP, NULL, 0);

    struct resolved_handle_path resolved = { 0 };
    const int rc = resolve_node_handle_path (fuse, request_header->nodeid, setattr_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

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
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    const char * link_name;
    const char * target;

    int rc = parse_symlink_request (payload, payload_length, &link_name, &target);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    if (fuse->ops.symlink == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct path_buffer path = { 0 };

    rc = build_child_path (fuse, request_header->nodeid, link_name, &path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = fuse->ops.symlink (target, path.data);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    invalidate_node_directory_cache (fuse, request_header->nodeid);

    return send_reply (context, 0, NULL, 0);
}

static int validate_link_source (struct fuse * fuse, struct fuse_node * node, const char * path)
{
    struct stat metadata;
    int rc = read_path_attributes (fuse, path, &metadata);

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

    pthread_mutex_lock (&fuse->node_lock);
    rc = update_node_backend_identity (fuse, node, &metadata);
    pthread_mutex_unlock (&fuse->node_lock);

    return rc;
}

struct link_operation
{
    const struct usfs_link_in * request; // Borrowed decoded link request.
    const char * name;                   // Borrowed destination name in the request payload.
    struct fuse_node * node;             // Source node retained by the request dispatcher.
    struct path_buffer old_path;         // Resolved source backend path.
    struct path_buffer new_path;         // Resolved destination backend path.
};

static int link_prepared_name (const struct request_context * context, const struct link_operation * operation)
{
    struct fuse * fuse = context->fuse;

    struct fuse_alias * prepared = prepare_alias (operation->request->newparent, operation->name);

    if (prepared == NULL)
        return send_reply (context, ENOMEM, NULL, 0);

    const int rc = fuse->ops.link (operation->old_path.data, operation->new_path.data);
    if (rc != 0)
    {
        free (prepared->name);
        free (prepared);

        return send_reply (context, -rc, NULL, 0);
    }

    pthread_mutex_lock (&fuse->node_lock);
    attach_alias (fuse, operation->node, prepared);
    operation->node->backend_links++;
    pthread_mutex_unlock (&fuse->node_lock);

    invalidate_node_directory_cache (fuse, operation->request->newparent);

    return send_reply (context, 0, NULL, 0);
}

static int select_link_source_locked (const struct fuse * fuse, const uint64_t nodeid, struct link_operation * operation)
{
    operation->node = find_node_by_id (fuse, nodeid);

    if (operation->node == NULL)
        return ENOENT;

    if (operation->node->detached)
        return ENOENT;

    if (find_alias (fuse, operation->request->newparent, operation->name) != NULL)
        return EEXIST;

    return 0;
}

static int create_link_and_reply (const struct request_context * context, const struct usfs_link_in * request, const char * name)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct link_operation operation = { 0 };

    operation.request = request;
    operation.name = name;

    pthread_mutex_lock (&fuse->node_lock);
    int rc = select_link_source_locked (fuse, request_header->nodeid, &operation);

    pthread_mutex_unlock (&fuse->node_lock);

    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    rc = build_node_path (fuse, request_header->nodeid, operation.old_path.data, sizeof (operation.old_path.data));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = build_child_path (fuse, request->newparent, name, &operation.new_path);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = validate_link_source (fuse, operation.node, operation.old_path.data);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return link_prepared_name (context, &operation);
}

static int handle_link_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;

    struct usfs_link_in request;

    if (payload_length < sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&request, payload, sizeof (request));
    const char * name = payload + sizeof (request);

    if (!is_payload_null_terminated (name, payload_length - (uint32_t)sizeof (request)))
        return send_reply (context, EINVAL, NULL, 0);

    if (fuse->ops.link == NULL)
        return send_reply (context, EROFS, NULL, 0);

    return create_link_and_reply (context, &request, name);
}

static int open_backend_handle (const struct fuse * fuse, const char * path, const uint32_t is_directory, struct fuse_file_info * file_info)
{
    if (is_directory)
    {
        if (fuse->ops.opendir == NULL)
            return 0;

        return fuse->ops.opendir (path, file_info);
    }

    if (fuse->ops.open == NULL)
        return 0;

    return fuse->ops.open (path, file_info);
}

static int open_prepared_handle (
    const struct request_context * context,
    const char * path,
    struct open_handle_state * opened,
    struct fuse_handle * handle
)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    const int rc = open_backend_handle (fuse, path, opened->is_directory, &opened->file_info);
    if (rc != 0)
    {
        free (handle);

        return send_reply (context, -rc, NULL, 0);
    }

    opened->nodeid = request_header->nodeid;
    publish_client_handle (fuse, handle, opened);

    struct usfs_open_out reply = { 0 };

    reply.fh = opened->file_info.fh;

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int handle_open_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_open_in open_request;

    if (payload_length < sizeof (open_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&open_request, payload, sizeof (open_request));

    char path[USFS_PATH_MAX];
    const int rc = build_node_path (fuse, request_header->nodeid, path, sizeof (path));
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    struct open_handle_state opened = { 0 };

    opened.file_info.flags = translate_open_flags (open_request.flags);

    if (opened.file_info.flags < 0)
        return send_reply (context, -opened.file_info.flags, NULL, 0);

    struct fuse_handle * handle = calloc (1, sizeof (*handle));

    if (handle == NULL)
        return send_reply (context, ENOMEM, NULL, 0);

    opened.is_directory = open_request.isdir;

    return open_prepared_handle (context, path, &opened, handle);
}

static int read_file_and_reply (const struct request_context * context, const char * path, const struct usfs_read_in * request, char * data)
{
    struct fuse * fuse = context->fuse;

    struct fuse_file_info file_info = { 0 };

    file_info.flags = O_RDONLY;
    file_info.fh = request->fh;

    memset (data, 0, request->size);

    const int rc = fuse->ops.read (path, data, (size_t)request->size, (off_t)request->offset, &file_info);
    if (rc < 0)
        return send_reply (context, -rc, NULL, 0);

    if ((uint32_t)rc > request->size)
        return send_reply (context, EIO, NULL, 0);

    return send_reply (context, 0, data, (uint32_t)rc);
}

static int handle_read_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_read_in read_request;
    char * data = get_reply_buffer (fuse) + sizeof (struct usfs_reply_header);

    if (payload_length < sizeof (read_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&read_request, payload, sizeof (read_request));

    if (read_request.size > USFS_MAX_DATA)
        read_request.size = USFS_MAX_DATA;

    struct resolved_handle_path resolved = { 0 };
    const int rc = resolve_node_handle_path (fuse, request_header->nodeid, read_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (fuse->ops.read == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    return read_file_and_reply (context, resolved.path, &read_request, data);
}

static int resolve_append_position (const struct fuse * fuse, const char * path, struct usfs_write_in * request, struct fuse_file_info * file_info)
{
    if ((request->flags & USFS_WRITE_APPEND) == 0)
        return 0;

    file_info->flags |= O_APPEND;
    struct stat metadata;
    const int rc = read_backend_attributes (fuse, path, &metadata, file_info);

    if (rc != 0)
        return -rc;

    if (!is_backend_stat_valid (&metadata))
        return EIO;

    request->offset = (uint64_t)metadata.st_size;

    return 0;
}

static int prepare_write_position (const struct fuse * fuse, const char * path, struct usfs_write_in * request, struct fuse_file_info * file_info)
{
    const int rc = resolve_append_position (fuse, path, request, file_info);

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
    struct fuse * fuse = context->fuse;

    struct fuse_file_info file_info = { 0 };

    file_info.flags = O_WRONLY;
    file_info.fh = request->fh;

    int rc = prepare_write_position (fuse, path, request, &file_info);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    rc = fuse->ops.write (path, data, (size_t)request->size, (off_t)request->offset, &file_info);
    if (rc < 0)
        return send_reply (context, -rc, NULL, 0);

    if ((uint32_t)rc > request->size)
        return send_reply (context, EIO, NULL, 0);

    struct usfs_write_out reply = { 0 };

    reply.written = (uint32_t)rc;
    reply.offset = request->offset;

    return send_reply (context, 0, &reply, (uint32_t)sizeof (reply));
}

static int handle_write_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_write_in request;

    if (payload_length < sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&request, payload, sizeof (request));

    if (request.size > payload_length - (uint32_t)sizeof (request))
        return send_reply (context, EINVAL, NULL, 0);

    const char * data = payload + sizeof (request);

    if (fuse->ops.write == NULL)
        return send_reply (context, EROFS, NULL, 0);

    struct resolved_handle_path resolved = { 0 };
    const int rc = resolve_node_handle_path (fuse, request_header->nodeid, request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    return write_file_and_reply (context, resolved.path, &request, data);
}

static int client_handle_has_path (const struct fuse_handle * handle, const struct fuse_node * node)
{
    if (handle->nodeid == USFS_ROOT_ID)
        return true;

    if (node == NULL)
        return false;

    return !node->detached;
}

static void release_backend_handle (const struct fuse * fuse, const struct fuse_handle * handle, const char * path)
{
    struct fuse_file_info file_info = { 0 };

    file_info.flags = handle->flags;
    file_info.fh = handle->fh;

    if (handle->isdir)
    {
        if (fuse->ops.releasedir != NULL)
            fuse->ops.releasedir (path, &file_info);

        return;
    }

    if (fuse->ops.release != NULL)
        fuse->ops.release (path, &file_info);
}

static void dispose_client_handle (struct fuse * fuse, struct fuse_handle * handle)
{
    pthread_mutex_lock (&fuse->dircache_lock);

    if (handle->isdir)
        invalidate_handle_directory_cache_locked (fuse, handle->nodeid, handle->fh);

    pthread_mutex_unlock (&fuse->dircache_lock);

    char path_buffer[USFS_PATH_MAX];
    const char * path = NULL;

    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_node * node = find_node_by_id (fuse, handle->nodeid);

    if (client_handle_has_path (handle, node) && build_node_path_locked (fuse, handle->nodeid, path_buffer, sizeof (path_buffer)) == 0)
        path = path_buffer;

    pthread_mutex_unlock (&fuse->node_lock);
    release_backend_handle (fuse, handle, path);

    pthread_mutex_lock (&fuse->node_lock);

    if (node != NULL)
    {
        node->open_refs--;
        queue_node_for_collection (fuse, node);
    }

    pthread_mutex_unlock (&fuse->node_lock);
    free (handle);
}

static int client_handle_matches_release (
    const struct fuse_handle * handle,
    const uint64_t nodeid,
    const struct usfs_release_in * request,
    const int flags
)
{
    if (handle->nodeid != nodeid)
        return false;

    if (handle->fh != request->fh)
        return false;

    if (handle->isdir != (int)request->isdir)
        return false;

    return handle->flags == flags;
}

static struct fuse_handle * detach_released_handle (
    struct fuse * fuse,
    const uint64_t nodeid,
    const struct usfs_release_in * request,
    const int flags
)
{
    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_handle ** cursor = &fuse->handles;

    while (*cursor != NULL)
    {
        if (client_handle_matches_release (*cursor, nodeid, request, flags))
            break;

        cursor = &(*cursor)->next;
    }

    struct fuse_handle * handle = *cursor;

    if (handle != NULL)
        *cursor = handle->next;

    pthread_mutex_unlock (&fuse->node_lock);

    return handle;
}

static int handle_release_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
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

    struct fuse_handle * handle = detach_released_handle (fuse, request_header->nodeid, &request, flags);

    if (handle == NULL)
        return send_reply (context, EINVAL, NULL, 0);

    dispose_client_handle (fuse, handle);

    return send_reply (context, 0, NULL, 0);
}

static int handle_flush_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_flush_in flush_request;

    if (payload_length != sizeof (flush_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&flush_request, payload, sizeof (flush_request));

    if (flush_request.pad != 0)
        return send_reply (context, EINVAL, NULL, 0);

    struct resolved_handle_path resolved = { 0 };
    int rc = resolve_node_handle_path (fuse, request_header->nodeid, flush_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    if (fuse->ops.flush == NULL)
        return send_reply (context, 0, NULL, 0);

    struct fuse_file_info file_info = { 0 };

    file_info.flags = translate_open_flags (flush_request.flags);

    if (file_info.flags < 0)
        return send_reply (context, -file_info.flags, NULL, 0);

    file_info.fh = flush_request.fh;
    rc = fuse->ops.flush (resolved.path, &file_info);

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

static int sync_backend_handle (const struct fuse * fuse, const char * path, const struct usfs_fsync_in * request)
{
    struct fuse_file_info file_info = { 0 };

    file_info.fh = request->fh;

    const int datasync = (request->flags & USFS_FSYNC_DATASYNC) != 0;

    if ((request->flags & USFS_FSYNC_DIRECTORY) != 0)
    {
        if (fuse->ops.fsyncdir == NULL)
            return -ENOSYS;

        return fuse->ops.fsyncdir (path, datasync, &file_info);
    }

    if (fuse->ops.fsync == NULL)
        return -ENOSYS;

    return fuse->ops.fsync (path, datasync, &file_info);
}

static int handle_fsync_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_fsync_in fsync_request;
    int rc = parse_fsync_request (payload, payload_length, &fsync_request);
    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    struct resolved_handle_path resolved = { 0 };

    rc = resolve_node_handle_path (fuse, request_header->nodeid, fsync_request.fh, &resolved);
    if (rc != 0)
        return send_reply (context, -rc, NULL, 0);

    rc = sync_backend_handle (fuse, resolved.path, &fsync_request);

    return send_reply (context, normalize_callback_error (rc), NULL, 0);
}

static int handle_syncfs_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;

    struct usfs_syncfs_in syncfs_request;

    if (payload_length != sizeof (syncfs_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&syncfs_request, payload, sizeof (syncfs_request));

    if (syncfs_request.pad != 0 || syncfs_request.mode > USFS_SYNCFS_QUIESCE)
        return send_reply (context, EINVAL, NULL, 0);

    if (fuse->ops.syncfs == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    const int rc = fuse->ops.syncfs ("/");

    return send_reply (context, normalize_callback_error (rc), NULL, 0);
}

/* The callback constructs one snapshot before it becomes visible to readers. */
struct dirbuf
{
    struct fuse * fuse;      // Daemon that owns temporary-storage accounting.
    char * data;             // In-memory serialized entries until spilling.
    size_t len;              // Number of serialized bytes.
    size_t cap;              // Allocated memory capacity.
    uint64_t auto_ino;       // Next synthetic inode number.
    uint32_t count;          // Number of serialized entries.
    int spool_fd;            // Unlinked temporary file when spooled.
    size_t * checkpoints;    // Byte offset of every indexed entry.
    size_t checkpoint_count; // Number of populated checkpoint offsets.
    size_t checkpoint_cap;   // Allocated checkpoint capacity.
    size_t spool_reserved;   // Bytes charged to the daemon's spool quota.
    bool spooled;            // Whether spool_fd owns a temporary file.
    int error;               // First directory fill error.
};

static int directory_cache_keys_match (const struct fuse_dircache_key * left, const struct fuse_dircache_key * right)
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

static uint64_t get_directory_handle_generation (struct fuse * fuse, const uint64_t nodeid, const uint64_t fh)
{
    uint64_t generation = 0;
    unsigned matches = 0;

    if (fh == 0)
        return 0;

    pthread_mutex_lock (&fuse->node_lock);

    for (const struct fuse_handle * handle = fuse->handles; handle != NULL; handle = handle->next)
    {
        if (handle->isdir && handle->nodeid == nodeid && handle->fh == fh)
        {
            generation = handle->generation;
            ++matches;
        }
    }

    pthread_mutex_unlock (&fuse->node_lock);

    return matches == 1 ? generation : 0;
}

static struct fuse_dircache * find_directory_cache_entry (struct fuse * fuse, const struct fuse_dircache_key * key, const uint64_t snapshot_id)
{
    for (int slot_index = 0; slot_index < FUSE_DIRCACHE_SLOTS; slot_index++)
        if (fuse->dircache[slot_index].snapshot_id == snapshot_id && directory_cache_keys_match (&fuse->dircache[slot_index].key, key))
            return &fuse->dircache[slot_index];

    return NULL;
}

static void release_directory_cache_resources (struct fuse * fuse, struct fuse_dircache * slot)
{
    if (slot->owns_node_ref)
    {
        pthread_mutex_lock (&fuse->node_lock);

        struct fuse_node * node = find_node_by_id (fuse, slot->key.nodeid);
        if (node != NULL)
        {
            node->cache_refs--;
            queue_node_for_collection (fuse, node);
        }

        pthread_mutex_unlock (&fuse->node_lock);
    }

    free (slot->data);
    fuse->dircache_ram_bytes -= slot->data_capacity;

    if (slot->spooled)
        close (slot->spool_fd);

    free (slot->checkpoints);
    fuse->dircache_spool_bytes -= slot->spool_reserved;
    slot->data = NULL;
    slot->data_capacity = 0;
    slot->checkpoints = NULL;
    slot->checkpoint_count = 0;
    slot->spool_reserved = 0;
    slot->spooled = false;
    slot->owns_node_ref = false;
}

static void clear_directory_cache_slot (struct fuse * fuse, struct fuse_dircache * slot)
{
    if (slot->snapshot_id == 0)
        return;

    release_directory_cache_resources (fuse, slot);
    memset (slot, 0, sizeof (*slot));
}

static void invalidate_node_directory_cache (struct fuse * fuse, const uint64_t nodeid)
{
    pthread_mutex_lock (&fuse->dircache_lock);

    for (int slot_index = 0; slot_index < FUSE_DIRCACHE_SLOTS; ++slot_index)
        if (fuse->dircache[slot_index].snapshot_id != 0 && fuse->dircache[slot_index].key.nodeid == nodeid)
            clear_directory_cache_slot (fuse, &fuse->dircache[slot_index]);

    pthread_mutex_unlock (&fuse->dircache_lock);
}

static void invalidate_handle_directory_cache_locked (struct fuse * fuse, const uint64_t nodeid, const uint64_t fh)
{
    for (int slot_index = 0; slot_index < FUSE_DIRCACHE_SLOTS; ++slot_index)
    {
        struct fuse_dircache * slot = &fuse->dircache[slot_index];

        if (slot->snapshot_id == 0)
            continue;

        if (slot->key.nodeid != nodeid)
            continue;

        if (slot->key.fh != fh)
            continue;

        clear_directory_cache_slot (fuse, slot);
    }
}

static struct fuse_dircache * select_directory_cache_slot (struct fuse * fuse)
{
    struct fuse_dircache * slot = &fuse->dircache[0];

    for (int slot_index = 0; slot_index < FUSE_DIRCACHE_SLOTS; slot_index++)
    {
        struct fuse_dircache * candidate = &fuse->dircache[slot_index];

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

static int spill_directory_buffer (struct dirbuf * directory_buffer);

static int publish_directory_snapshot (
    struct fuse * fuse,
    const struct fuse_dircache_key * key,
    struct dirbuf * directory_buffer,
    uint64_t * snapshot_id
)
{
    for (;;)
    {
        pthread_mutex_lock (&fuse->dircache_lock);

        if (key->generation != 0 && get_directory_handle_generation (fuse, key->nodeid, key->fh) != key->generation)
        {
            pthread_mutex_unlock (&fuse->dircache_lock);
            return ESTALE;
        }

        if (fuse->next_snapshot_id >= USFS_DIRECTORY_CURSOR_ID_MAX)
        {
            pthread_mutex_unlock (&fuse->dircache_lock);
            return EOVERFLOW;
        }

        struct fuse_dircache * slot = select_directory_cache_slot (fuse);
        const size_t retained_ram_bytes = fuse->dircache_ram_bytes - slot->data_capacity;

        if (!directory_buffer->spooled && directory_buffer->cap > FUSE_DIRCACHE_RAM_MAX - retained_ram_bytes)
        {
            pthread_mutex_unlock (&fuse->dircache_lock);

            const int spill_rc = spill_directory_buffer (directory_buffer);
            if (spill_rc != 0)
                return spill_rc;

            continue;
        }

        clear_directory_cache_slot (fuse, slot);

        pthread_mutex_lock (&fuse->node_lock);
        struct fuse_node * node = find_node_by_id (fuse, key->nodeid);
        if (node != NULL)
            node->cache_refs++;
        pthread_mutex_unlock (&fuse->node_lock);

        slot->key = *key;
        slot->owns_node_ref = node != NULL;
        slot->snapshot_id = ++fuse->next_snapshot_id;
        slot->data = directory_buffer->data;
        slot->data_capacity = directory_buffer->cap;
        slot->len = directory_buffer->len;
        slot->count = directory_buffer->count;
        slot->spool_fd = directory_buffer->spool_fd;
        slot->checkpoints = directory_buffer->checkpoints;
        slot->checkpoint_count = directory_buffer->checkpoint_count;
        slot->spool_reserved = directory_buffer->spool_reserved;
        slot->spooled = directory_buffer->spooled;
        slot->seq = ++fuse->dircache_seq;
        fuse->dircache_ram_bytes += slot->data_capacity;

        *snapshot_id = slot->snapshot_id;
        directory_buffer->data = NULL;
        directory_buffer->cap = 0;
        directory_buffer->checkpoints = NULL;
        directory_buffer->spooled = false;
        directory_buffer->spool_reserved = 0;

        pthread_mutex_unlock (&fuse->dircache_lock);
        return 0;
    }
}

static void release_directory_buffer (struct dirbuf * directory_buffer)
{
    free (directory_buffer->data);
    free (directory_buffer->checkpoints);

    if (directory_buffer->spooled)
        close (directory_buffer->spool_fd);

    if (directory_buffer->spool_reserved != 0)
    {
        pthread_mutex_lock (&directory_buffer->fuse->dircache_lock);
        directory_buffer->fuse->dircache_spool_bytes -= directory_buffer->spool_reserved;
        pthread_mutex_unlock (&directory_buffer->fuse->dircache_lock);
    }
}

static int reserve_directory_spool (struct dirbuf * directory_buffer, const size_t needed)
{
    if (needed > FUSE_DIRSPOOL_MAX)
        return ENOSPC;

    const size_t rounded = (needed + FUSE_DIRSPOOL_RESERVATION - 1u) & ~(size_t)(FUSE_DIRSPOOL_RESERVATION - 1u);

    if (rounded <= directory_buffer->spool_reserved)
        return 0;

    struct fuse * fuse = directory_buffer->fuse;
    const size_t extra = rounded - directory_buffer->spool_reserved;

    pthread_mutex_lock (&fuse->dircache_lock);

    while (extra > FUSE_DIRSPOOL_MAX - fuse->dircache_spool_bytes)
    {
        struct fuse_dircache * oldest_handleless = NULL;

        for (int slot_index = 0; slot_index < FUSE_DIRCACHE_SLOTS; slot_index++)
        {
            struct fuse_dircache * candidate = &fuse->dircache[slot_index];

            if (candidate->snapshot_id == 0 || candidate->key.generation != 0 || candidate->spool_reserved == 0)
                continue;

            if (oldest_handleless == NULL || candidate->seq < oldest_handleless->seq)
                oldest_handleless = candidate;
        }

        if (oldest_handleless == NULL)
        {
            pthread_mutex_unlock (&fuse->dircache_lock);
            return ENOSPC;
        }

        clear_directory_cache_slot (fuse, oldest_handleless);
    }

    fuse->dircache_spool_bytes += extra;
    directory_buffer->spool_reserved = rounded;
    pthread_mutex_unlock (&fuse->dircache_lock);

    return 0;
}

static void clear_directory_cache (struct fuse * fuse)
{
    for (int slot_index = 0; slot_index < FUSE_DIRCACHE_SLOTS; slot_index++)
    {
        clear_directory_cache_slot (fuse, &fuse->dircache[slot_index]);
    }
}

static int create_directory_spool (const struct fuse * fuse, int * spool_fd)
{
    const char * temporary_directory = getenv ("TMPDIR");
    if (temporary_directory == NULL || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";

    char resolved_directory[USFS_PATH_MAX];
    if (realpath (temporary_directory, resolved_directory) == NULL)
        return EIO;

    if (fuse->mountpoint != NULL)
    {
        const size_t mountpoint_length = strlen (fuse->mountpoint);

        if (strncmp (resolved_directory, fuse->mountpoint, mountpoint_length) == 0 &&
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

static int append_directory_checkpoint (struct dirbuf * directory_buffer, const size_t offset)
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

static int spill_directory_buffer (struct dirbuf * directory_buffer)
{
    if (directory_buffer->fuse == NULL)
        return ENOSPC;

    int rc = reserve_directory_spool (directory_buffer, directory_buffer->len);
    if (rc != 0)
        return rc;

    rc = create_directory_spool (directory_buffer->fuse, &directory_buffer->spool_fd);
    if (rc != 0)
        return rc;

    directory_buffer->spooled = true;

    size_t position = 0;
    for (uint32_t entry_index = 0; entry_index < directory_buffer->count; entry_index++)
    {
        if (entry_index % FUSE_DIRECTORY_INDEX_STRIDE == 0)
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

static int reserve_directory_buffer (struct dirbuf * directory_buffer, const size_t needed)
{
    if (needed > FUSE_DIRCACHE_MAX)
        return 1;

    if (needed <= directory_buffer->cap)
        return 0;

    size_t new_capacity = directory_buffer->cap ? directory_buffer->cap : FUSE_DIRBUF_INITIAL_CAPACITY;

    while (new_capacity < needed)
    {
        if (new_capacity > SIZE_MAX / FUSE_DIRBUF_GROWTH_FACTOR)
        {
            new_capacity = needed;
            break;
        }
        new_capacity *= FUSE_DIRBUF_GROWTH_FACTOR;
    }

    if (new_capacity > FUSE_DIRCACHE_MAX)
        new_capacity = FUSE_DIRCACHE_MAX;

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

static int append_directory_entry (
    void * buffer,
    const char * name,
    const struct stat * entry_metadata,
    const off_t offset,
    enum fuse_fill_dir_flags flags
)
{
    struct dirbuf * directory_buffer = buffer;
    struct usfs_dirent directory_entry;

    (void)offset;
    (void)flags;

    if (directory_buffer->error != 0)
        return 1;

    const char * terminator = memchr (name, '\0', USFS_MAX_NAME);
    if (terminator == NULL)
    {
        directory_buffer->error = ENAMETOOLONG;
        return 1;
    }

    const size_t name_length = (size_t)(terminator - name);

    if (name_length >= USFS_MAX_NAME)
    {
        directory_buffer->error = ENAMETOOLONG;
        return 1;
    }

    const uint16_t record_length = USFS_DIRENT_SIZE (name_length);

    if (directory_buffer->count >= USFS_DIRECTORY_CURSOR_INDEX_MASK || directory_buffer->len > SIZE_MAX - record_length)
    {
        directory_buffer->error = EOVERFLOW;
        return 1;
    }

    const size_t needed = directory_buffer->len + record_length;

    if (!directory_buffer->spooled && needed > FUSE_DIRCACHE_MAX)
    {
        directory_buffer->error = spill_directory_buffer (directory_buffer);
        if (directory_buffer->error != 0)
            return 1;
    }

    if (directory_buffer->spooled)
    {
        directory_buffer->error = reserve_directory_spool (directory_buffer, needed);
        if (directory_buffer->error != 0)
            return 1;

        if (directory_buffer->count % FUSE_DIRECTORY_INDEX_STRIDE == 0)
        {
            directory_buffer->error = append_directory_checkpoint (directory_buffer, directory_buffer->len);
            if (directory_buffer->error != 0)
                return 1;
        }
    }
    else if (reserve_directory_buffer (directory_buffer, needed) != 0)
        return 1;

    memset (&directory_entry, 0, sizeof (directory_entry));
    directory_entry.namelen = (uint16_t)name_length;
    directory_entry.reclen = record_length;

    if (entry_metadata != NULL && entry_metadata->st_ino != 0)
    {
        directory_entry.ino = (uint64_t)entry_metadata->st_ino;
        directory_entry.type = ((uint32_t)entry_metadata->st_mode & FUSE_DIRENT_MODE_MASK) >> FUSE_DIRENT_TYPE_SHIFT;
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
            return 1;
    }

    directory_buffer->len += record_length;
    directory_buffer->count += 1;

    return 0;
}

static int load_readdir_snapshot (
    const struct request_context * context,
    const struct usfs_readdir_in * request,
    const char * path,
    const struct fuse_dircache_key * key,
    uint64_t * snapshot_id
)
{
    struct fuse * fuse = context->fuse;
    struct dirbuf directory_buffer = { 0 };
    struct fuse_file_info file_info = { 0 };

    directory_buffer.fuse = fuse;
    directory_buffer.auto_ino = FUSE_SYNTHETIC_INODE_BASE + context->header->nodeid;
    file_info.fh = request->fh;

    const int callback_result = fuse->ops.readdir (path, &directory_buffer, append_directory_entry, 0, &file_info, FUSE_READDIR_DEFAULTS);

    int rc = directory_buffer.error != 0 ? directory_buffer.error : normalize_callback_error (callback_result);
    if (rc == 0)
        rc = publish_directory_snapshot (fuse, key, &directory_buffer, snapshot_id);

    release_directory_buffer (&directory_buffer);
    return rc;
}

static int read_directory_entry (const struct fuse_dircache * snapshot, const size_t position, struct usfs_dirent * entry)
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

static int find_directory_entry_position (const struct fuse_dircache * snapshot, const uint32_t start_index, size_t * position)
{
    uint32_t entry_index = 0;
    *position = 0;

    if (snapshot->spooled && start_index != 0)
    {
        const size_t checkpoint_index = start_index / FUSE_DIRECTORY_INDEX_STRIDE;

        if (checkpoint_index >= snapshot->checkpoint_count)
            return EIO;

        *position = snapshot->checkpoints[checkpoint_index];
        entry_index = (uint32_t)(checkpoint_index * FUSE_DIRECTORY_INDEX_STRIDE);
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
    const struct fuse_dircache * snapshot,
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
    const struct fuse_dircache_key * key,
    const uint64_t snapshot_id,
    const uint32_t start_index,
    const uint32_t maximum_bytes
)
{
    struct fuse * fuse = context->fuse;
    struct usfs_readdir_out * reply = (struct usfs_readdir_out *)(get_reply_buffer (fuse) + sizeof (struct usfs_reply_header));
    uint32_t reply_length = 0;

    pthread_mutex_lock (&fuse->dircache_lock);

    struct fuse_dircache * snapshot = find_directory_cache_entry (fuse, key, snapshot_id);
    if (snapshot == NULL)
    {
        pthread_mutex_unlock (&fuse->dircache_lock);
        return send_reply (context, ESTALE, NULL, 0);
    }

    if (snapshot->completed && start_index < snapshot->count)
    {
        pthread_mutex_unlock (&fuse->dircache_lock);
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
        clear_directory_cache_slot (fuse, snapshot);
    else if (rc == 0 && key->generation == 0 && reply->count == 0 && start_index >= snapshot->count)
    {
        release_directory_cache_resources (fuse, snapshot);
        snapshot->completed = true;
    }

    if (rc != EIO)
        snapshot->seq = ++fuse->dircache_seq;

    pthread_mutex_unlock (&fuse->dircache_lock);

    if (rc != 0)
        return send_reply (context, rc, NULL, 0);

    return send_reply (context, 0, reply, reply_length);
}

static int handle_readdir_request (const struct request_context * context, const char * payload, const uint32_t payload_length)
{
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;
    struct usfs_readdir_in readdir_request;

    if (payload_length < sizeof (readdir_request))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&readdir_request, payload, sizeof (readdir_request));

    if (readdir_request.size == 0 || readdir_request.size > USFS_MAX_DATA)
        readdir_request.size = USFS_MAX_DATA;

    struct resolved_handle_path resolved = { 0 };
    const int path_rc = resolve_node_handle_path (fuse, request_header->nodeid, readdir_request.fh, &resolved);
    if (path_rc != 0)
        return send_reply (context, -path_rc, NULL, 0);

    if (fuse->ops.readdir == NULL)
        return send_reply (context, ENOSYS, NULL, 0);

    struct fuse_dircache_key key = { 0 };
    key.nodeid = request_header->nodeid;
    key.fh = readdir_request.fh;
    key.generation = get_directory_handle_generation (fuse, key.nodeid, key.fh);
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
    struct fuse * fuse = context->fuse;
    const struct usfs_in_hdr * request_header = context->header;

    struct usfs_forget_in body;
    uint64_t * lookup_references;
    int error = 0;

    if (payload_length != sizeof (body))
        return send_reply (context, EINVAL, NULL, 0);

    memcpy (&body, payload, sizeof (body));
    pthread_mutex_lock (&fuse->node_lock);

    struct fuse_node * node = find_node_by_id (fuse, request_header->nodeid);

    if (request_header->nodeid == USFS_ROOT_ID)
    {
        lookup_references = &fuse->root_lookup_refs;
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
        queue_node_for_collection (fuse, node);
    }

    pthread_mutex_unlock (&fuse->node_lock);

    return send_reply (context, error, NULL, 0);
}

static struct fuse_node * retain_request_node (struct fuse * fuse, const uint64_t nodeid)
{
    pthread_mutex_lock (&fuse->node_lock);
    struct fuse_node * node = find_node_by_id (fuse, nodeid);

    if (node != NULL)
        node->operation_refs++;

    pthread_mutex_unlock (&fuse->node_lock);

    return node;
}

static int lock_request_node_for_mutation (const struct usfs_in_hdr * request_header, struct fuse_node * retained, struct fuse_node ** object)
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

static void finish_request (struct fuse * fuse, struct fuse_node * object, struct fuse_node * retained, const int exclusive)
{
    if (object != NULL)
        pthread_mutex_unlock (&object->mutation_lock);

    pthread_mutex_lock (&fuse->node_lock);

    if (retained != NULL)
    {
        retained->operation_refs--;
        queue_node_for_collection (fuse, retained);
    }

    collect_unreferenced_nodes (fuse);
    pthread_mutex_unlock (&fuse->node_lock);
    release_namespace_access (fuse, exclusive);
}

static void handle_request (struct fuse * fuse, const char * message, const size_t message_length)
{
    struct fuse_context_scope scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };
    struct usfs_in_hdr request_header;
    struct fuse_node * object = NULL;
    struct fuse_node * retained = NULL;
    const char * payload = message + sizeof (request_header);

    if (message_length < sizeof (request_header))
    {
        fail_session (fuse, EIO);
        return;
    }

    memcpy (&request_header, message, sizeof (request_header));

    if (request_header.version != USFS_PROTOCOL_VERSION || request_header.len > message_length || request_header.len < sizeof (request_header))
    {
        fail_session (fuse, EIO);
        return;
    }

    struct request_context context = { 0 };

    context.fuse = fuse;
    context.header = &request_header;

    const uint32_t payload_length = request_header.len - (uint32_t)sizeof (request_header);

    int error = enter_callback_context (fuse, &scope);
    if (error != 0)
    {
        fail_session (fuse, error);
        return;
    }

    set_request_context (fuse, &request_header);
    const int exclusive = request_mutates_namespace (request_header.opcode);

    error = acquire_namespace_access (fuse, exclusive);
    if (error != 0)
    {
        send_reply (&context, error, NULL, 0);
        return;
    }

    retained = retain_request_node (fuse, request_header.nodeid);
    error = lock_request_node_for_mutation (&request_header, retained, &object);
    if (error != 0)
        send_reply (&context, error, NULL, 0);
    else
        dispatch_request (&context, payload, payload_length);

    finish_request (fuse, object, retained, exclusive);
}

/* ----------------------------------------------------------- *
 * Lifecycle                                                   *
 * ----------------------------------------------------------- */

static int parse_command_options (struct fuse_args const * args, struct fuse_cmdline_opts * options, int * mount_mode);

static void destroy_session_synchronization (struct fuse * fuse)
{
    pthread_cond_destroy (&fuse->namespace.changed);
    pthread_mutex_destroy (&fuse->namespace.lock);
    pthread_mutex_destroy (&fuse->reply_lock);
    pthread_mutex_destroy (&fuse->dircache_lock);
    pthread_mutex_destroy (&fuse->node_lock);
}

static int initialize_session_synchronization (struct fuse * fuse)
{
    if (pthread_mutex_init (&fuse->node_lock, NULL) != 0)
    {
        return -1;
    }

    if (pthread_mutex_init (&fuse->dircache_lock, NULL) != 0)
    {
        pthread_mutex_destroy (&fuse->node_lock);
        return -1;
    }

    if (pthread_mutex_init (&fuse->reply_lock, NULL) != 0)
    {
        pthread_mutex_destroy (&fuse->dircache_lock);
        pthread_mutex_destroy (&fuse->node_lock);
        return -1;
    }

    if (pthread_mutex_init (&fuse->namespace.lock, NULL) != 0)
    {
        pthread_mutex_destroy (&fuse->reply_lock);
        pthread_mutex_destroy (&fuse->dircache_lock);
        pthread_mutex_destroy (&fuse->node_lock);
        return -1;
    }

    if (pthread_cond_init (&fuse->namespace.changed, NULL) != 0)
    {
        pthread_mutex_destroy (&fuse->namespace.lock);
        pthread_mutex_destroy (&fuse->reply_lock);
        pthread_mutex_destroy (&fuse->dircache_lock);
        pthread_mutex_destroy (&fuse->node_lock);
        return -1;
    }

    return 0;
}

static void initialize_connection_info (struct fuse * fuse)
{
    fuse->conn.proto_major = FUSE_MAJOR_VERSION;
    fuse->conn.proto_minor = FUSE_MINOR_VERSION;
    fuse->conn.max_write = USFS_MAX_DATA;
    fuse->conn.max_read = USFS_MAX_DATA;
    fuse->conn.capable = (uint32_t)FUSE_CAP_ASYNC_READ;
    fuse->conn.capable_ext = FUSE_CAP_ASYNC_READ;
}

static int initialize_message_io (struct fuse * fuse)
{
    fuse->readbuf = malloc (USFS_MSG_MAX);
    fuse->writebuf = malloc (USFS_MSG_MAX);

    if (fuse->readbuf != NULL && fuse->writebuf != NULL && create_session_wake_pipe (&fuse->session) == 0)
        return 0;

    free (fuse->readbuf);
    free (fuse->writebuf);

    return -1;
}

struct fuse * _fuse_new_31 (
    struct fuse_args * args,
    const struct fuse_operations * op,
    size_t op_size,
    struct libfuse_version * version,
    void * user_data
)
{
    struct fuse_cmdline_opts options;
    int mount_mode = FUSE_MOUNT_MODE_AUTOMATIC;

    (void)version;

    if (args != NULL)
    {
        const int rc = parse_command_options (args, &options, &mount_mode);

        free (options.mountpoint);

        if (rc != 0)
            return NULL;
    }

    struct fuse * fuse = calloc (1, sizeof (*fuse));

    if (fuse == NULL)
        return NULL;

    if (op_size > sizeof (fuse->ops))
        op_size = sizeof (fuse->ops);

    memcpy (&fuse->ops, op, op_size);

    if (op_size < offsetof (struct fuse_operations, export_id) + sizeof (fuse->ops.export_id))
        fuse->ops.export_id = NULL;

    if (op_size < offsetof (struct fuse_operations, resolve_id) + sizeof (fuse->ops.resolve_id))
        fuse->ops.resolve_id = NULL;

    fuse->fd = INVALID_FILE_DESCRIPTOR;
    fuse->mount_mode = mount_mode;
    fuse->user_data = user_data;
    fuse->next_id = USFS_ROOT_ID + 1;

    if (initialize_session_synchronization (fuse) != 0)
    {
        free (fuse);
        return NULL;
    }

    initialize_connection_info (fuse);

    if (initialize_message_io (fuse) != 0)
    {
        destroy_session_synchronization (fuse);
        free (fuse);
        return NULL;
    }

    return fuse;
}

static int enter_destroy_callback_context (struct fuse * fuse, struct fuse_context_scope * scope)
{
    if (fuse->handles == NULL && (!fuse->init_done || fuse->ops.destroy == NULL))
        return 0;

    return enter_callback_context (fuse, scope);
}

static void destroy_user_state (struct fuse * fuse)
{
    while (fuse->handles != NULL)
    {
        struct fuse_handle * handle = fuse->handles;

        fuse->handles = handle->next;
        dispose_client_handle (fuse, handle);
    }

    if (fuse->init_done && fuse->ops.destroy != NULL)
        fuse->ops.destroy (fuse->user_data);
}

static void close_transport (const struct fuse * fuse)
{
    if (fuse->fd >= 0)
        close (fuse->fd); /* triggers ddclose -> connection teardown */

    fuse_remove_signal_handlers (&fuse->session);

    close (fuse->session.wake_read);
    close (fuse->session.wake_write);
}

static void destroy_namespace (struct fuse * fuse)
{
    clear_directory_cache (fuse);

    struct fuse_node * node = fuse->nodes;

    while (node != NULL)
    {
        struct fuse_node * next = node->next;

        destroy_node (node);
        node = next;
    }
}

static void free_session_storage (struct fuse * fuse)
{
    free (fuse->mountpoint);
    free (fuse->readbuf);
    free (fuse->writebuf);
    destroy_session_synchronization (fuse);
    free (fuse);
}

void fuse_destroy (struct fuse * fuse)
{
    struct fuse_context_scope scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };

    errno = 0;

    if (fuse == NULL)
        return;

    if (fuse->mounted)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to destroy filesystem: mount cleanup is unresolved\n");
        errno = EBUSY;
        return;
    }

    const int error = enter_destroy_callback_context (fuse, &scope);

    if (error != 0)
    {
        errno = error;
        return;
    }

    destroy_user_state (fuse);
    close_transport (fuse);
    destroy_namespace (fuse);
    free_session_storage (fuse);
    errno = 0;
}

/*
 * Reports whether the file system can be modified, which is simply whether it
 * implements any operation that would modify it. The kernel extension is told
 * at mount time so that a read-only file system can refuse writes outright
 * instead of asking a daemon that would only refuse them too.
 */
static int is_filesystem_writable (const struct fuse * fuse)
{
    if (fuse->mount_mode != FUSE_MOUNT_MODE_AUTOMATIC)
        return fuse->mount_mode == FUSE_MOUNT_MODE_READ_WRITE;

    if (fuse->ops.create != NULL)
        return true;

    if (fuse->ops.create_attr != NULL)
        return true;

    if (fuse->ops.mkdir != NULL)
        return true;

    if (fuse->ops.write != NULL || fuse->ops.unlink != NULL)
        return true;

    if (fuse->ops.rmdir != NULL)
        return true;

    if (fuse->ops.rename != NULL)
        return true;

    if (fuse->ops.symlink != NULL || fuse->ops.link != NULL)
        return true;

    if (fuse->ops.chmod != NULL)
        return true;

    if (fuse->ops.chown != NULL)
        return true;

    if (fuse->ops.truncate != NULL || fuse->ops.utimens != NULL)
        return true;

    return false;
}

#define FUSE_VMOUNT_ALIGNMENT_BYTES       4
#define FUSE_VMOUNT_BUFFER_SIZE           4096
#define FUSE_MOUNT_INFO_BUFFER_SIZE       128
#define FUSE_MOUNT_INVENTORY_INITIAL_SIZE 8192
#define FUSE_UNMOUNT_MAX_ATTEMPTS         20
#define FUSE_UNMOUNT_RETRY_DELAY_US       250000

static void align_mount_data_cursor (char ** cursor)
{
    while ((uintptr_t)(*cursor) % FUSE_VMOUNT_ALIGNMENT_BYTES != 0)
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

static int request_connection (struct fuse * fuse)
{
    if (ioctl (fuse->fd, USFS_IOC_CONNECTION_REQUEST, &fuse->info) != 0)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to establish USFS connection: %s\n", strerror (errno));
        return -1;
    }

    if (fuse->info.protocol_version != USFS_PROTOCOL_VERSION)
    {
        fuse_log (
            FUSE_LOG_ERR,
            "Failed to establish USFS connection: protocol version mismatch (kernel %u, library %u)\n",
            (unsigned)fuse->info.protocol_version,
            (unsigned)USFS_PROTOCOL_VERSION
        );
        return -1;
    }

    return 0;
}

static int submit_mount (struct fuse const * fuse, const char * mountpoint, const char * mount_info)
{
    const size_t buffer_size = FUSE_VMOUNT_BUFFER_SIZE;
    char * buffer = calloc (1, buffer_size);
    char * data;

    if (buffer == NULL)
        return -1;

    struct vmount * vm = (struct vmount *)buffer;
    data = buffer + sizeof (struct vmount);

    vm->vmt_revision = VMT_REVISION;
    vm->vmt_flags = is_filesystem_writable (fuse) ? 0 : MNT_READONLY;
    vm->vmt_gfstype = fuse->info.fs_type;

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
        fuse_log (FUSE_LOG_ERR, "Failed to mount %s: %s\n", mountpoint, strerror (errno));

    return result;
}

static int connect_and_mount_filesystem (struct fuse * fuse, const char * resolved_mountpoint, char ** cleanup_mountpoint)
{
    char mount_info[FUSE_MOUNT_INFO_BUFFER_SIZE];

    if (request_connection (fuse) != 0)
        return -1;

    snprintf (
        mount_info,
        sizeof (mount_info),
        "fd=%d,chan=%d,cookie=%016llx,rw=%d",
        fuse->fd,
        (int)fuse->info.channel,
        (unsigned long long)fuse->info.cookie,
        is_filesystem_writable (fuse)
    );
    *cleanup_mountpoint = strdup (resolved_mountpoint);

    if (*cleanup_mountpoint == NULL)
        return -1;

    if (submit_mount (fuse, resolved_mountpoint, mount_info) != 0)
        return -1;

    return 0;
}

int fuse_mount (struct fuse * fuse, const char * mountpoint)
{
    char resolved_mountpoint[PATH_MAX];
    char * cleanup_mountpoint = NULL;

    if (realpath (mountpoint, resolved_mountpoint) == NULL)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to resolve mountpoint `%s': %s\n", mountpoint, strerror (errno));
        return -1;
    }

    fuse->fd = open (USFS_DEVICE_PATH, O_RDWR | O_NONBLOCK | _FCLOEXEC);

    if (fuse->fd < 0)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to open %s: %s\n", USFS_DEVICE_PATH, strerror (errno));
        return -1;
    }

    if (connect_and_mount_filesystem (fuse, resolved_mountpoint, &cleanup_mountpoint) != 0)
    {
        free (cleanup_mountpoint);
        close (fuse->fd);
        fuse->fd = INVALID_FILE_DESCRIPTOR;
        return -1;
    }

    fuse->mountpoint = cleanup_mountpoint;
    fuse->mounted = 1;

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
    int buffer_size = FUSE_MOUNT_INVENTORY_INITIAL_SIZE;
    char * buffer = NULL;
    const int mount_count = read_complete_mount_inventory (&buffer, &buffer_size);

    if (mount_count < 0)
        return -1;

    inventory->buffer = buffer;
    inventory->record_count = mount_count;
    inventory->buffer_size = buffer_size;

    return 0;
}

static int check_mount_record_ownership (const struct fuse * fuse, const char * record, const struct vmount * mount, int * matches)
{
    *matches = 0;

    if (mount->vmt_gfstype != fuse->info.fs_type)
        return 0;

    const struct vmt_data * field = &mount->vmt_data[VMT_INFO];

    if (!usfs_bounded_region_valid (mount->vmt_length, field->vmt_off, field->vmt_size, sizeof (*mount)))
        return -1;

    const char * info = record + field->vmt_off;
    uint64_t cookie;
    const int cookie_parse_result = usfs_info_field_u64 (info, field->vmt_size, "cookie", sizeof ("cookie") - 1, FUSE_HEXADECIMAL_RADIX, &cookie);
    if (cookie_parse_result != 1)
        return -1;

    uint64_t channel;
    const int channel_parse_result = usfs_info_field_u64 (info, field->vmt_size, "chan", sizeof ("chan") - 1, FUSE_DECIMAL_RADIX, &channel);
    if (channel_parse_result != 1)
        return -1;

    *matches = cookie == fuse->info.cookie && channel == (uint64_t)fuse->info.channel;

    return 0;
}

static int find_owned_mount (const struct fuse * fuse, const struct mount_inventory * inventory, int * number)
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

        if (check_mount_record_ownership (fuse, cursor, mount_record, &matches) != 0)
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

static int query_owned_mount_number (const struct fuse * fuse, int * number)
{
    struct mount_inventory inventory = { 0 };

    if (query_mount_inventory (&inventory) != 0)
        return -1;

    const int found = find_owned_mount (fuse, &inventory, number);

    free (inventory.buffer);

    if (found < 0)
        errno = EIO;

    return found;
}

static void close_exited_channel (struct fuse * fuse)
{
    if (is_session_exited (&fuse->session) && fuse->fd >= 0)
    {
        close (fuse->fd);
        fuse->fd = INVALID_FILE_DESCRIPTOR;
    }
}

static int unmount_owned_mount (const struct fuse * fuse, int number)
{
    /* AIX may briefly retain the mount after its last access. Every retry uses
     * the verified unique mount number, never its pathname. */
    for (int attempt = 0; attempt < FUSE_UNMOUNT_MAX_ATTEMPTS; ++attempt)
    {
        if (uvmount (number, 0) == 0)
            return 0;

        if (errno != EBUSY)
        {
            const int error = errno;

            /* External unmount can finish between the inventory read and this
             * call. Only verified absence releases ownership. */
            if (error == EINVAL && query_owned_mount_number (fuse, &number) == 0)
                return 0;

            fuse_log (FUSE_LOG_ERR, "Failed to unmount %s: %s\n", fuse->mountpoint, strerror (error));
            errno = error;

            return -1;
        }

        if (attempt + 1 < FUSE_UNMOUNT_MAX_ATTEMPTS)
            usleep (FUSE_UNMOUNT_RETRY_DELAY_US);
    }

    fuse_log (FUSE_LOG_ERR, "Failed to unmount %s: still busy\n", fuse->mountpoint);
    errno = EBUSY;

    return -1;
}

void fuse_unmount (struct fuse * fuse)
{
    int number = -1;
    errno = 0;

    if (fuse == NULL || !fuse->mounted)
        return;

    if (fuse->mountpoint == NULL)
    {
        errno = EINVAL;
        return;
    }

    const int found = query_owned_mount_number (fuse, &number);
    if (found < 0)
        return;

    close_exited_channel (fuse);

    if (found && unmount_owned_mount (fuse, number) != 0)
        return;

    fuse->mounted = 0;
    errno = 0;
}

static int initialize_filesystem_once (struct fuse * fuse)
{
    struct fuse_context_scope scope __attribute__ ((cleanup (restore_callback_context))) = { 0 };
    const int error = enter_callback_context (fuse, &scope);

    if (error != 0)
        return -error;

    if (!fuse->init_done)
    {
        if (fuse->ops.init != NULL)
            fuse->user_data = fuse->ops.init (&fuse->conn, &fuse->config);

        fuse->init_done = 1;
    }

    return 0;
}

static int fail_session_on_device_error (struct fuse * fuse, const int error)
{
    fail_session (fuse, error);

    return resolve_session_result (fuse, -error);
}

static int read_and_handle_request (struct fuse * fuse)
{
    const ssize_t request_length = read (fuse->fd, fuse->readbuf, USFS_MSG_MAX);

    if (request_length < 0)
    {
        if (errno == EINTR || errno == EAGAIN)
            return 0; /* The request loop re-checks the session exit state. */

        return fail_session_on_device_error (fuse, errno);
    }

    if (request_length == 0)
    {
        fuse_exit (fuse);
        return 0;
    }

    if (!is_session_exited (&fuse->session))
        handle_request (fuse, fuse->readbuf, (size_t)request_length);

    return 0;
}

static int run_request_loop (struct fuse * fuse)
{
    while (!is_session_exited (&fuse->session))
    {
        const int ready = wait_for_session_event (&fuse->session, fuse->fd);

        if (ready != 0)
        {
            fail_session (fuse, -ready);
            return resolve_session_result (fuse, ready);
        }

        if (is_session_exited (&fuse->session))
            break;

        const int rc = read_and_handle_request (fuse);
        if (rc != 0)
            return rc;
    }

    return resolve_session_result (fuse, 0);
}

int fuse_loop (struct fuse * fuse)
{
    if (fuse == NULL)
        return -EINVAL;

    const int init_error = initialize_filesystem_once (fuse);

    if (init_error != 0)
    {
        fail_session (fuse, -init_error);
        return resolve_session_result (fuse, init_error);
    }

    return run_request_loop (fuse);
}

#define FUSE_MT_DEFAULT_THREADS 10u
#define FUSE_MT_MAX_THREADS     64u
#define FUSE_MT_QUEUE_FACTOR    2u

struct fuse_mt_slot
{
    size_t len;              // Number of request bytes in this slot.
    char data[USFS_MSG_MAX]; // Owned fixed-capacity request storage.
};

struct fuse_mt_pool
{
    struct fuse * fuse;          // Session served by the workers.
    pthread_mutex_t lock;        // Protects queue and worker-start state.
    pthread_cond_t started;      // Notifies the reader of worker initialization.
    pthread_cond_t readable;     // Notifies workers when requests arrive or stop is requested.
    pthread_cond_t writable;     // Notifies queue capacity changes.
    struct fuse_mt_slot * slots; // Owned bounded request queue.
    size_t capacity;             // Number of available queue slots.
    size_t head;                 // Next queue slot to consume.
    size_t tail;                 // Next queue slot to publish.
    size_t count;                // Number of queued requests.
    size_t ready;                // Number of workers that completed initialization.
    int worker_error;            // Worker context initialization error.
    int stopping;                // Whether workers should stop after draining the queue.
};

struct fuse_mt_worker
{
    struct fuse_mt_pool * pool;        // Borrowed shared queue.
    struct fuse_thread_context thread; // Worker-owned reply buffer holder.
    char * readbuf;                    // Owned request buffer of USFS_MSG_MAX bytes.
};

struct fuse_mt_runtime
{
    struct fuse_mt_pool pool;        // Shared worker queue.
    struct fuse_mt_worker * workers; // Owned worker storage.
    pthread_t * threads;             // Owned thread identifiers.
    unsigned int worker_count;       // Number of configured workers.
    unsigned int started;            // Number of successfully created threads.
};

struct fuse_mt_signal_mask
{
    sigset_t previous; // Reader signal mask saved before worker creation.
    int blocked;       // Whether the original mask still needs restoration.
};

static int initialize_request_worker (const struct fuse_mt_worker * worker, struct fuse_thread_context ** thread)
{
    struct fuse_mt_pool * pool = worker->pool;
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

static int dequeue_worker_request (const struct fuse_mt_worker * worker, size_t * request_length)
{
    struct fuse_mt_pool * pool = worker->pool;

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

    notify_session (&pool->fuse->session);

    return true;
}

static void * run_request_worker (void * opaque)
{
    const struct fuse_mt_worker * worker = opaque;
    const struct fuse_mt_pool * pool = worker->pool;
    struct fuse_thread_context * thread;

    if (initialize_request_worker (worker, &thread) != 0)
        return NULL;

    size_t request_length;

    while (dequeue_worker_request (worker, &request_length))
    {
        if (!is_session_exited (&pool->fuse->session))
            handle_request (pool->fuse, worker->readbuf, request_length);
    }

    thread->writebuf = NULL; /* The worker pool owns the buffer, TLS owns only context. */

    return NULL;
}

static int enqueue_worker_request (struct fuse_mt_pool * pool, const char * request, const size_t request_length)
{
    int result = 0;

    pthread_mutex_lock (&pool->lock);

    while (pool->count == pool->capacity && !is_session_exited (&pool->fuse->session))
    {
        pthread_mutex_unlock (&pool->lock);
        result = wait_for_session_event (&pool->fuse->session, -1);
        pthread_mutex_lock (&pool->lock);

        if (result != 0)
            break;
    }

    if (!is_session_exited (&pool->fuse->session) && result == 0)
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

static int read_and_enqueue_requests (struct fuse * fuse, struct fuse_mt_pool * pool)
{
    while (!is_session_exited (&fuse->session))
    {
        int result = wait_for_session_event (&fuse->session, fuse->fd);
        if (result != 0 || is_session_exited (&fuse->session))
            return result;

        const ssize_t request_length = read (fuse->fd, fuse->readbuf, USFS_MSG_MAX);

        if (request_length < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return fail_session_on_device_error (fuse, errno);
        }

        if (request_length == 0)
        {
            fuse_exit (fuse);
            return 0;
        }

        result = enqueue_worker_request (pool, fuse->readbuf, (size_t)request_length);
        if (result != 0)
            return result;
    }

    return 0;
}

static void free_worker_storage (struct fuse_mt_runtime * runtime)
{
    free (runtime->threads);
    free (runtime->workers);
    free (runtime->pool.slots);
}

static int initialize_worker_synchronization (struct fuse_mt_pool * pool)
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

static int initialize_worker_runtime (struct fuse_mt_runtime * runtime, struct fuse * fuse, unsigned int worker_count)
{
    memset (runtime, 0, sizeof (*runtime));

    struct fuse_mt_pool * pool = &runtime->pool;
    pool->fuse = fuse;
    pool->capacity = (size_t)worker_count * FUSE_MT_QUEUE_FACTOR;
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

static void destroy_worker_runtime (struct fuse_mt_runtime * runtime)
{
    pthread_cond_destroy (&runtime->pool.started);
    pthread_cond_destroy (&runtime->pool.writable);
    pthread_cond_destroy (&runtime->pool.readable);
    pthread_mutex_destroy (&runtime->pool.lock);
    free_worker_storage (runtime);
}

static int block_worker_exit_signals (struct fuse_mt_signal_mask * mask)
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

static int restore_thread_signal_mask (struct fuse_mt_signal_mask * mask)
{
    if (!mask->blocked)
        return 0;

    if (pthread_sigmask (SIG_SETMASK, &mask->previous, NULL) != 0)
        return -EAGAIN;

    mask->blocked = 0;

    return 0;
}

static int start_request_workers (struct fuse_mt_runtime * runtime)
{
    for (unsigned int worker_index = 0; worker_index < runtime->worker_count; worker_index++)
    {
        struct fuse_mt_worker * worker = &runtime->workers[worker_index];

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

static int wait_for_worker_startup (struct fuse_mt_runtime * runtime)
{
    struct fuse_mt_pool * pool = &runtime->pool;

    pthread_mutex_lock (&pool->lock);
    while (pool->ready < runtime->started)
        pthread_cond_wait (&pool->started, &pool->lock);

    const int result = pool->worker_error == 0 ? 0 : -pool->worker_error;
    pthread_mutex_unlock (&pool->lock);

    return result;
}

static void stop_request_workers (struct fuse_mt_runtime * runtime)
{
    struct fuse_mt_pool * pool = &runtime->pool;
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

static int run_worker_runtime (struct fuse * fuse, struct fuse_mt_runtime * runtime)
{
    struct fuse_mt_signal_mask signal_mask = { 0 };
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
            result = read_and_enqueue_requests (fuse, &runtime->pool);

        stop_request_workers (runtime);
        (void)restore_thread_signal_mask (&signal_mask);
    }

    return result;
}

static int run_multithreaded_session (struct fuse * fuse, const unsigned int thread_count)
{
    struct fuse_mt_runtime runtime;
    int result = 0;

    if (fuse == NULL || thread_count == 0 || thread_count > FUSE_MT_MAX_THREADS)
        return -EINVAL;

    const int init_error = initialize_filesystem_once (fuse);

    if (init_error != 0)
    {
        fail_session (fuse, -init_error);
        return resolve_session_result (fuse, init_error);
    }

    result = initialize_worker_runtime (&runtime, fuse, thread_count);

    if (result != 0)
    {
        fail_session (fuse, -result);
        return resolve_session_result (fuse, result);
    }

    result = run_worker_runtime (fuse, &runtime);
    destroy_worker_runtime (&runtime);
    if (result != 0)
        fail_session (fuse, -result);

    return resolve_session_result (fuse, result);
}

int fuse_loop_mt_31 (struct fuse * fuse, const int clone_fd)
{
    /* One reader preserves device request ordering; bounded workers execute
       callbacks concurrently. A cloned descriptor is therefore unnecessary. */
    (void)clone_fd;

    return run_multithreaded_session (fuse, FUSE_MT_DEFAULT_THREADS);
}

#undef fuse_loop_mt

int fuse_loop_mt (struct fuse * fuse, struct fuse_loop_config * config)
{
    (void)config;

    return run_multithreaded_session (fuse, FUSE_MT_DEFAULT_THREADS);
}

/* ----------------------------------------------------------- *
 * Command line handling and fuse_main                         *
 * ----------------------------------------------------------- */

/* Preserve the established fuse_main exit-status values. */
enum fuse_main_result
{
    FUSE_MAIN_SUCCESS = 0,
    FUSE_MAIN_INVALID_ARGUMENTS = 1,
    FUSE_MAIN_MISSING_MOUNTPOINT = 2,
    FUSE_MAIN_CREATE_FAILED = 3,
    FUSE_MAIN_MOUNT_FAILED = 4,
    FUSE_MAIN_SIGNAL_HANDLERS_FAILED = 6,
    FUSE_MAIN_LOOP_FAILED = 8,
    FUSE_MAIN_UNMOUNT_FAILED = 9
};

void fuse_cmdline_help (void)
{
    printf (
        "    -h   --help            print help\n"
        "    -V   --version         print version\n"
        "    -d   -o debug          enable debug output (implies -f)\n"
        "    -f                     foreground operation (always on)\n"
        "    -s                     disable multi-threaded operation\n"
        "    -o max_threads=N       bound worker threads (1..64, default 10)\n"
        "    -o ro | -o rw          request read-only or read/write access\n"
    );
}

void fuse_lib_help (struct fuse_args * args)
{
    (void)args;
    fuse_cmdline_help ();
}

static int set_mount_access_mode (int * mount_mode, const int requested_mode)
{
    if (*mount_mode != FUSE_MOUNT_MODE_AUTOMATIC && *mount_mode != requested_mode)
    {
        errno = EINVAL;
        return -1;
    }

    *mount_mode = requested_mode;

    return 0;
}

static int parse_max_threads_option (struct fuse_cmdline_opts * options, const char * thread_count_text)
{
    char * thread_count_end;

    errno = 0;

    const unsigned long thread_count = strtoul (thread_count_text, &thread_count_end, FUSE_DECIMAL_RADIX);

    if (errno != 0)
        return -1;

    if (*thread_count_end != '\0')
        return -1;

    if (thread_count == 0)
        return -1;

    if (thread_count > FUSE_MT_MAX_THREADS)
        return -1;

    options->max_threads = (unsigned int)thread_count;

    return 0;
}

static int parse_mount_option (struct fuse_cmdline_opts * options, const char * option, int * mount_mode)
{
    static const char max_threads_prefix[] = "max_threads=";

    if (strcmp (option, "ro") == 0)
        return set_mount_access_mode (mount_mode, FUSE_MOUNT_MODE_READ_ONLY);

    if (strcmp (option, "rw") == 0)
        return set_mount_access_mode (mount_mode, FUSE_MOUNT_MODE_READ_WRITE);

    if (strcmp (option, "debug") == 0)
    {
        options->foreground = 1;
        options->debug = 1;
        return 0;
    }

    if (strncmp (option, max_threads_prefix, sizeof (max_threads_prefix) - 1) == 0)
        return parse_max_threads_option (options, option + sizeof (max_threads_prefix) - 1);

    fuse_log (FUSE_LOG_ERR, "Failed to parse mount options: unsupported mount option `%s'\n", option);
    errno = EINVAL;

    return -1;
}

static int is_mount_option_list_valid (const char * text)
{
    if (text == NULL)
        return false;

    if (text[0] == '\0')
        return false;

    if (text[0] == ',')
        return false;

    if (text[strlen (text) - 1] == ',')
        return false;

    if (strstr (text, ",,") != NULL)
        return false;

    return true;
}

static int parse_mount_option_list (struct fuse_cmdline_opts * options, const char * text, int * mount_mode)
{
    char * next_option = NULL;
    int result = 0;

    if (!is_mount_option_list_valid (text))
    {
        errno = EINVAL;
        return -1;
    }

    char * option_list = strdup (text);
    if (option_list == NULL)
        return -1;

    for (const char * option = strtok_r (option_list, ",", &next_option); option != NULL; option = strtok_r (NULL, ",", &next_option))
    {
        result = parse_mount_option (options, option, mount_mode);

        if (result != 0)
            break;
    }

    free (option_list);

    return result;
}

static int apply_command_flag (struct fuse_cmdline_opts * options, const char * argument)
{
    if (strcmp (argument, "-h") == 0 || strcmp (argument, "--help") == 0)
    {
        options->show_help = 1;
    }
    else if (strcmp (argument, "-V") == 0 || strcmp (argument, "--version") == 0)
    {
        options->show_version = 1;
    }
    else if (strcmp (argument, "-d") == 0)
    {
        options->debug = 1;
        options->foreground = 1;
    }
    else if (strcmp (argument, "-f") == 0)
    {
        options->foreground = 1;
    }
    else if (strcmp (argument, "-s") == 0)
    {
        options->singlethread = 1;
    }
    else
    {
        return false;
    }

    return true;
}

struct command_parser
{
    const struct fuse_args * args;      // Borrowed command-line arguments.
    struct fuse_cmdline_opts * options; // Caller-owned parsed command options.
    int * mount_mode;                   // Caller-owned mount access selection.
};

static int parse_mount_option_argument (const struct command_parser * parser, int * argument_index, const char * option_text)
{
    if (option_text[0] == '\0')
    {
        ++*argument_index;

        if (*argument_index >= parser->args->argc)
            return -1;

        option_text = parser->args->argv[*argument_index];
    }

    return parse_mount_option_list (parser->options, option_text, parser->mount_mode);
}

static int parse_mountpoint_argument (struct fuse_cmdline_opts * options, const char * argument)
{
    if (options->mountpoint != NULL)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to parse command line: invalid argument `%s'\n", argument);
        return -1;
    }

    options->mountpoint = strdup (argument);
    if (options->mountpoint == NULL)
        return -1;

    return 0;
}

static int parse_command_argument (const struct command_parser * parser, int * argument_index)
{
    static const char mount_option_prefix[] = "-o";
    const char * argument = parser->args->argv[*argument_index];

    if (argument == NULL)
    {
        errno = EINVAL;
        return -1;
    }

    if (apply_command_flag (parser->options, argument))
        return 0;

    if (strncmp (argument, mount_option_prefix, sizeof (mount_option_prefix) - 1) == 0)
        return parse_mount_option_argument (parser, argument_index, argument + sizeof (mount_option_prefix) - 1);

    if (argument[0] == '-')
    {
        fuse_log (FUSE_LOG_ERR, "Failed to parse command line: unsupported argument `%s'\n", argument);
        errno = EINVAL;
        return -1;
    }

    return parse_mountpoint_argument (parser->options, argument);
}

static int parse_command_options (struct fuse_args const * args, struct fuse_cmdline_opts * options, int * mount_mode)
{
    memset (options, 0, sizeof (*options));
    options->max_idle_threads = (unsigned int)-1;
    options->max_threads = FUSE_MT_DEFAULT_THREADS;
    *mount_mode = FUSE_MOUNT_MODE_AUTOMATIC;

    if (args == NULL || args->argc < 0 || (args->argc != 0 && args->argv == NULL))
    {
        errno = EINVAL;
        return -1;
    }

    struct command_parser parser = { 0 };

    parser.args = args;
    parser.options = options;
    parser.mount_mode = mount_mode;

    for (int argument_index = 1; argument_index < args->argc; argument_index++)
    {
        if (parse_command_argument (&parser, &argument_index) != 0)
            return -1;
    }

    return 0;
}

int fuse_parse_cmdline (struct fuse_args * args, struct fuse_cmdline_opts * options)
{
    int mount_mode;

    return parse_command_options (args, options, &mount_mode);
}

static int run_mounted_request_loop (struct fuse * fuse, const struct fuse_cmdline_opts * options)
{
    struct fuse_session * session = fuse_get_session (fuse);

    if (fuse_set_signal_handlers (session) != 0)
        return FUSE_MAIN_SIGNAL_HANDLERS_FAILED;

    int rc = options->singlethread ? fuse_loop (fuse) : run_multithreaded_session (fuse, options->max_threads);
    if (rc != 0)
        rc = FUSE_MAIN_LOOP_FAILED;

    fuse_remove_signal_handlers (session);

    return rc;
}

static int mount_and_run_session (struct fuse * fuse, const struct fuse_cmdline_opts * options)
{
    if (fuse_mount (fuse, options->mountpoint) != 0)
        return FUSE_MAIN_MOUNT_FAILED;

    int rc = run_mounted_request_loop (fuse, options);

    fuse_unmount (fuse);

    if (fuse->mounted)
        rc = FUSE_MAIN_UNMOUNT_FAILED;

    return rc;
}

struct filesystem_implementation
{
    const struct fuse_operations * operations; // Borrowed callback table supplied by the filesystem.
    size_t operations_size;                    // Callback table size used for ABI compatibility.
    struct libfuse_version * version;          // Borrowed version supplied to session creation.
    void * user_data;                          // Initial callback private data.
};

static int process_main_options (
    struct fuse_args * args,
    const struct fuse_cmdline_opts * options,
    const struct filesystem_implementation * implementation
)
{
    if (options->show_version)
    {
        printf ("FUSE library version %s (AIX USFS)\n", fuse_pkgversion ());
        return FUSE_MAIN_SUCCESS;
    }

    if (options->show_help)
    {
        if (args->argv[0][0] != '\0')
            printf ("usage: %s [options] <mountpoint>\n\n", args->argv[0]);

        fuse_cmdline_help ();
        return FUSE_MAIN_SUCCESS;
    }

    if (options->mountpoint == NULL)
    {
        fuse_log (FUSE_LOG_ERR, "Failed to start filesystem: no mountpoint specified\n");
        return FUSE_MAIN_MISSING_MOUNTPOINT;
    }

    struct fuse * fuse =
        _fuse_new_31 (args, implementation->operations, implementation->operations_size, implementation->version, implementation->user_data);

    if (fuse == NULL)
        return FUSE_MAIN_CREATE_FAILED;

    const int rc = mount_and_run_session (fuse, options);

    fuse_destroy (fuse);

    return rc;
}

int fuse_main_real_versioned (
    const int argc,
    char * argv[],
    const struct fuse_operations * op,
    const size_t op_size,
    struct libfuse_version * version,
    void * user_data
)
{
    struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
    struct fuse_cmdline_opts options;
    int rc = FUSE_MAIN_INVALID_ARGUMENTS;

    if (fuse_parse_cmdline (&args, &options) == 0)
    {
        struct filesystem_implementation implementation = { 0 };

        implementation.operations = op;
        implementation.operations_size = op_size;
        implementation.version = version;
        implementation.user_data = user_data;

        rc = process_main_options (&args, &options, &implementation);
    }

    free (options.mountpoint);
    fuse_opt_free_args (&args);

    return rc;
}
