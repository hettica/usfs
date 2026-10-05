// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

/**
 * USFS wire protocol.
 *
 * Shared between the kernel extension (src/kernel/) and the userspace library
 * (src/client/). This is USFS's own protocol: it is modeled on
 * the FUSE kernel protocol (nodeid-based requests, unique-id correlation) but
 * makes no attempt at binary compatibility with Linux /dev/fuse.
 *
 * Framing:
 *   kernel -> daemon (read from /dev/usfs0):
 *       struct usfs_in_hdr | per-op body | optional name | optional payload
 *   daemon -> kernel (write to /dev/usfs0):
 *       struct usfs_out_hdr | per-op body / data (absent when error != 0)
 *
 * The name is NUL-terminated; operations naming two objects (RENAME, SYMLINK)
 * put the second one in the payload so the two sit back to back, with a length
 * in the body separating them. WRITE uses the payload for raw file data.
 *
 * Every message is self-describing: hdr.len is the total message length in
 * bytes including the header. A read() must supply a buffer of at least
 * USFS_MSG_MAX bytes or the kernel fails the read with EMSGSIZE.
 *
 * All structures use only fixed-width types with explicit padding so that the
 * kernel (gcc -maix64 -D_KERNEL) and userspace (gcc -maix64) compilations
 * produce identical layouts. Every struct size is a multiple of 8 bytes.
 */

#ifndef USFS_PROTO_H
#define USFS_PROTO_H

#ifdef _KERNEL
    #include <sys/inttypes.h>
#else
    #include <stdint.h>
#endif

/* Version 2 added READLINK and STATFS, and the parent field of usfs_attr_out.
 * Version 3 added the mutating operations (CREATE, MKDIR, UNLINK, RMDIR,
 * RENAME, SYMLINK, LINK, SETATTR, WRITE) and gave GETATTR a body carrying the
 * file handle. Version 4 added truthful durability operations (FLUSH, FSYNC,
 * and SYNCFS).
 * Both sides refuse a mismatched version rather than misparsing a reply. */
/* Version 5 adds atomic CREATE_ATTR and explicit activation intent. */
/* Version 6 transfers explicit lookup ownership and acknowledges FORGET. */
/* Version 7 adds terminal device EOF after successful ordinary unmount.
 * Versions through 6 can spin on EOF and must fail the version handshake. */
/* Version 8 makes READDIR positions opaque positive 63-bit snapshot cursors:
 * the high 39 bits identify a snapshot, and the low 24 bits give its next
 * entry index. Zero starts a new snapshot. */
/* Version 9 adds mount-scoped file identifiers and vnode reconstruction. */
#define USFS_PROTOCOL_VERSION 9u

#define USFS_DIRECTORY_CURSOR_INDEX_BITS 24u
#define USFS_DIRECTORY_CURSOR_INDEX_MASK ((1ull << USFS_DIRECTORY_CURSOR_INDEX_BITS) - 1ull)
#define USFS_DIRECTORY_CURSOR_ID_MAX     ((1ull << 39u) - 1ull)

/* The nodeid of the filesystem root. Known implicitly by both sides at mount
 * time; the kernel never sends a LOOKUP for the root itself. */
#define USFS_ROOT_ID 1ull

/* Maximum length of one directory entry name, including the NUL. */
#define USFS_MAX_NAME 256u

/* Maximum symbolic link target length carried in a READLINK reply. */
#define USFS_MAX_LINK 1024u

/* Cap on a single READ reply payload and on a READDIR reply blob. Larger
 * transfers are split into multiple requests by the kernel. */
#define USFS_MAX_DATA (64u * 1024u)

/* Upper bound for any single wire message in either direction. Daemons must
 * read() with a buffer at least this large. */
#define USFS_MSG_MAX (sizeof (struct usfs_in_hdr) + 64u + USFS_MAX_NAME + USFS_MAX_DATA)

enum usfs_opcode
{
    USFS_OP_LOOKUP = 1,   /* in: hdr(nodeid=parent) + name; out: usfs_entry_out    */
    USFS_OP_GETATTR = 2,  /* in: hdr + usfs_getattr_in;     out: usfs_attr_out     */
    USFS_OP_OPEN = 3,     /* in: hdr + usfs_open_in;        out: usfs_open_out     */
    USFS_OP_READ = 4,     /* in: hdr + usfs_read_in;        out: raw data          */
    USFS_OP_RELEASE = 5,  /* in: hdr + usfs_release_in;     out: hdr only          */
    USFS_OP_READDIR = 6,  /* in: hdr + usfs_readdir_in;     out: usfs_readdir_out  */
    USFS_OP_READLINK = 7, /* in: hdr only;                  out: raw link target   */
    USFS_OP_STATFS = 8,   /* in: hdr only;                  out: usfs_statfs_out   */

    /* Mutating operations. A file system that implements none of them is
     * mounted read only and the kernel refuses these without asking. */
    USFS_OP_CREATE = 9,       /* hdr(parent) + usfs_create_in + name;   out: usfs_create_out */
    USFS_OP_MKDIR = 10,       /* hdr(parent) + usfs_mkdir_in + name;    out: hdr only        */
    USFS_OP_UNLINK = 11,      /* hdr(parent) + name;                    out: hdr only        */
    USFS_OP_RMDIR = 12,       /* hdr(parent) + name;                    out: hdr only        */
    USFS_OP_RENAME = 13,      /* hdr(srcdir) + usfs_rename_in + 2 names;  out: hdr only      */
    USFS_OP_SYMLINK = 14,     /* hdr(parent) + usfs_symlink_in + 2 names; out: hdr only       */
    USFS_OP_LINK = 15,        /* hdr(nodeid) + usfs_link_in + name;     out: hdr only        */
    USFS_OP_SETATTR = 16,     /* hdr + usfs_setattr_in;                 out: usfs_attr_out   */
    USFS_OP_WRITE = 17,       /* hdr + usfs_write_in + data;            out: usfs_write_out  */
    USFS_OP_FLUSH = 18,       /* hdr + usfs_flush_in;                   out: hdr only        */
    USFS_OP_FSYNC = 19,       /* hdr + usfs_fsync_in;                   out: hdr only        */
    USFS_OP_SYNCFS = 20,      /* hdr(root) + usfs_syncfs_in;            out: hdr only        */
    USFS_OP_CREATE_ATTR = 21, /* hdr(parent) + usfs_create_attr_in + name; out: usfs_create_out */
    USFS_OP_FORGET = 22,      /* hdr(object) + usfs_forget_in; out: acknowledged header only */
    USFS_OP_FID = 23,         /* hdr(object); out: usfs_fid_out */
    USFS_OP_VGET = 24         /* hdr(root) + usfs_vget_in; out: usfs_vget_out */
    /* access is answered kernel-side; no opcode for it. */
};

/* Kernel -> daemon request header. 40 bytes. */
struct usfs_in_hdr
{
    uint32_t len;     /* total message bytes incl. this header */
    uint16_t version; /* USFS_PROTOCOL_VERSION */
    uint16_t opcode;  /* enum usfs_opcode */
    uint64_t unique;  /* kernel-assigned correlation id, echoed in the reply */
    uint64_t nodeid;  /* target node (parent node for LOOKUP) */
    uint32_t uid;     /* credentials of the calling process */
    uint32_t gid;
    uint32_t pid;
    uint32_t pad;
};

/* LOOKUP: no body struct; the NUL-terminated entry name directly follows the
 * header. hdr.len = sizeof(hdr) + strlen(name) + 1. */

/* GETATTR: header only. */

struct usfs_open_in
{
    uint32_t flags; /* open flags as seen by the kernel (FREAD etc.) */
    uint32_t isdir; /* nonzero when the target is a directory */
};

/* A file that is open carries its handle here, and a stat of a file that has
 * been unlinked while open has nothing else to go on: its name is gone, so a
 * daemon that resolves by path alone could only answer ENOENT. Zero when the
 * object is not open. */
struct usfs_getattr_in
{
    uint64_t fh;
};

struct usfs_read_in
{
    uint64_t fh; /* file handle from usfs_open_out */
    uint64_t offset;
    uint32_t size; /* requested bytes, <= USFS_MAX_DATA */
    uint32_t pad;
};

struct usfs_release_in
{
    uint64_t fh;
    uint32_t flags;
    uint32_t isdir; /* nonzero when the target is a directory */
};

struct usfs_readdir_in
{
    uint64_t fh;     /* daemon directory handle, or zero for handleless walks */
    uint64_t cookie; /* 0 starts a snapshot; otherwise snapshot ID and entry index */
    uint32_t size;   /* max reply payload bytes, <= USFS_MAX_DATA */
    uint32_t pad;
};

/* VGET identifies an object independently of its short-lived wire node ID. */
struct usfs_vget_in
{
    uint64_t token; /* Backend-owned identity, never reused within a mount. */
};

/* ---------------------------------------------------------------------------
 * Mutating request bodies.
 *
 * Operations naming two objects (RENAME, SYMLINK) carry a length in the body
 * and then both NUL-terminated strings back to back, so the receiver can find
 * the second one without rescanning.
 * ------------------------------------------------------------------------- */

struct usfs_create_in
{
    uint32_t flags; /* open flags; the created file is also opened */
    uint32_t mode;  /* permission bits for the new file */
};

struct usfs_mkdir_in
{
    uint32_t mode;
    uint32_t pad;
};

/* UNLINK, RMDIR: no body; the NUL-terminated entry name follows the header,
 * and hdr.nodeid is the containing directory. */

struct usfs_rename_in
{
    uint64_t newparent;  /* destination directory */
    uint32_t oldnamelen; /* bytes of the first name incl. its NUL */
    uint32_t pad;
    /* then: oldname NUL, newname NUL */
};

struct usfs_symlink_in
{
    uint32_t namelen; /* bytes of the link name incl. its NUL */
    uint32_t pad;
    /* then: link name NUL, target NUL (target may be up to USFS_MAX_LINK) */
};

struct usfs_link_in
{
    uint64_t newparent; /* directory to create the new name in */
    /* then: the new name, NUL-terminated. hdr.nodeid is the existing file. */
};

/* Which fields of usfs_setattr_in are meaningful. */
#define USFS_SET_MODE      (1u << 0)
#define USFS_SET_UID       (1u << 1)
#define USFS_SET_GID       (1u << 2)
#define USFS_SET_SIZE      (1u << 3)
#define USFS_SET_ATIME     (1u << 4)
#define USFS_SET_MTIME     (1u << 5)
#define USFS_SET_CTIME     (1u << 6)
#define USFS_SET_TIMES_NOW (1u << 7) /* use current time, ignore the values */

struct usfs_setattr_in
{
    uint64_t fh;    /* handle when the object is open, else 0; see usfs_getattr_in */
    uint32_t valid; /* OR of USFS_SET_* */
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t pad;
};

#define USFS_CREATE_DEFAULT 0u
#define USFS_CREATE_LOOKUP  1u
#define USFS_CREATE_OPEN    2u

struct usfs_create_attr_in
{
    uint32_t flags;
    uint32_t activation;
    struct usfs_setattr_in attr; /* fh/pad zero; MODE required, TIMES_NOW forbidden */
};

/* Successful LOOKUP and activated CREATE/CREATE_ATTR transfer one lookup
 * reference, even when the kernel reuses an existing vnode. FORGET releases
 * accumulated references and must receive a successful acknowledgement. */
struct usfs_forget_in
{
    uint64_t count;
};

/* usfs_write_in.flags */
#define USFS_WRITE_APPEND (1u << 0) /* ignore offset, write at end of file */

struct usfs_write_in
{
    uint64_t fh;
    uint64_t offset;
    uint32_t size;  /* payload bytes following this body, <= USFS_MAX_DATA */
    uint32_t flags; /* OR of USFS_WRITE_* */
};

/* Called for every file close before the final RELEASE. Unlike RELEASE, a
 * FLUSH error is returned to the application. */
struct usfs_flush_in
{
    uint64_t fh;
    uint32_t flags; /* open flags as seen by the kernel */
    uint32_t pad;
};

/* usfs_fsync_in.flags */
#define USFS_FSYNC_DATASYNC  (1u << 0)
#define USFS_FSYNC_RANGE     (1u << 1)
#define USFS_FSYNC_DIRECTORY (1u << 2)

struct usfs_fsync_in
{
    uint64_t fh;
    uint64_t offset; /* meaningful only with USFS_FSYNC_RANGE */
    uint64_t length; /* zero means through end of file */
    uint32_t flags;  /* OR of USFS_FSYNC_* */
    uint32_t pad;
};

/* Normalized AIX syncvfs command levels. The filesystem granularity has
 * already been resolved by AIX before the per-mount callback is entered. */
#define USFS_SYNCFS_TRY     0u
#define USFS_SYNCFS_FORCE   1u
#define USFS_SYNCFS_QUIESCE 2u

struct usfs_syncfs_in
{
    uint32_t mode; /* one of USFS_SYNCFS_* */
    uint32_t pad;
};

/* Daemon -> kernel reply header. 24 bytes. */
struct usfs_reply_header
{
    uint32_t len;     /* total message bytes incl. this header */
    uint16_t version; /* USFS_PROTOCOL_VERSION */
    uint16_t opcode;  /* echo of the request opcode */
    uint64_t id;      /* echo of the request unique id */
    int32_t error;    /* 0 on success, positive errno on failure (no body then) */
    uint32_t pad;
};

/* File attributes, everything the kernel needs to fill an AIX struct vattr.
 * 88 bytes. */
struct usfs_attr
{
    uint64_t ino;
    uint64_t size;
    uint64_t blocks; /* 512-byte units */
    int64_t atime;   /* seconds since the epoch */
    int64_t mtime;
    int64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode; /* S_IFMT file type bits + permission bits */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
    uint32_t blksize;
    uint32_t pad;
};

struct usfs_entry_out /* LOOKUP reply body */
{
    uint64_t nodeid; /* daemon-assigned id for subsequent requests */
    struct usfs_attr attr;
};

struct usfs_fid_out
{
    uint64_t token; /* Backend-owned identity for the requested object. */
};

/* Successful VGET transfers one lookup reference for entry.nodeid. */
struct usfs_vget_out
{
    uint64_t token;              /* Echo of the requested backend identity. */
    struct usfs_entry_out entry; /* Canonical wire node and validated attributes. */
};

struct usfs_attr_out /* GETATTR reply body */
{
    struct usfs_attr attr;
    /* Informational parent snapshot; the root's parent is itself. LOOKUP of
     * ".." resolves the authoritative current parent after namespace moves. */
    uint64_t parent;
};

struct usfs_open_out
{
    uint64_t fh; /* opaque daemon file handle, echoed in READ/RELEASE */
    uint32_t open_flags;
    uint32_t pad;
};

/* CREATE reply: the new object plus the handle it was opened with, since
 * vnop_create is required to leave the file open. */
struct usfs_create_out
{
    uint64_t nodeid;
    struct usfs_attr attr;
    uint64_t fh;
};

/* WRITE reply. The count is reported so a short write is detectable: unlike a
 * short read, it is not an ordinary end-of-file condition. The offset is where
 * the bytes actually landed, which the requester only knows in advance when
 * USFS_WRITE_APPEND is clear; the kernel needs it to advance the file pointer. */
struct usfs_write_out
{
    uint32_t written;
    uint32_t pad;
    uint64_t offset;
};

/* READ reply: raw file data directly follows the header;
 * data length = hdr.len - sizeof(hdr). A short reply signals EOF. */

/* READLINK reply: the link target directly follows the header, without a
 * terminating NUL; length = hdr.len - sizeof(hdr), at most USFS_MAX_LINK. */

struct usfs_statfs_out /* STATFS reply body */
{
    uint64_t blocks; /* total blocks, in units of bsize */
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files; /* total file slots */
    uint64_t ffree;
    uint32_t bsize;
    uint32_t namemax; /* longest permitted component name */
};

/* READDIR reply: usfs_readdir_out followed by `count` variable-length
 * usfs_dirent records. A count of 0 signals end of directory. */
struct usfs_readdir_out
{
    uint64_t snapshot_id; /* positive cursor identity, valid through the last entry */
    uint32_t count;
    uint32_t pad;
};

struct usfs_dirent
{
    uint64_t ino;
    uint32_t type;    /* file type bits: (mode & S_IFMT) >> 12 */
    uint16_t namelen; /* name length excluding the NUL */
    uint16_t reclen;  /* total record size incl. name, padded to 8 bytes */
    /* char name[]; NUL-terminated, padded with zeros up to reclen */
};

#define USFS_DIRENT_SIZE(namelen) \
    ((uint16_t)((sizeof (struct usfs_dirent) + (namelen) + 1u + 7u) & ~7u))

/* ---------------------------------------------------------------------------
 * Device ioctl: daemon setup handshake.
 *
 * After open("/dev/usfs0"), the daemon issues USFS_IOC_CONNECTION_REQUEST to learn the
 * registered gfs type number (assigned dynamically at kext config time), its
 * own channel id, and a per-connection random cookie. chan+cookie are passed
 * back to the kernel in the vmount data so that vfs_mount can bind the mount
 * to this connection. A plain integer constant is used instead of the _IOR
 * macro family to avoid <sys/ioctl.h> encoding differences between the kernel
 * and userspace compilations.
 * ------------------------------------------------------------------------- */

#define USFS_IOC_CONNECTION_REQUEST 0x55530001 /* 'U' 'S' 0x0001 */

struct usfs_dev_info
{
    uint32_t protocol_version; /* USFS_PROTOCOL_VERSION */
    int32_t fs_type;           /* value for vmount.vmt_gfstype */
    int32_t channel;           /* this connection's channel id */
    uint32_t padding;
    uint64_t cookie; /* per-connection random cookie */
};

/* Runtime availability-policy control used by chusfs. */
#define USFS_IOC_GET_RUNTIME_CONFIG 0x55530003
#define USFS_IOC_SET_RUNTIME_CONFIG 0x55530004
#define USFS_IOC_GET_KEXT_STATE     0x55530005

/* Mount data: the daemon puts the string
 *     "fd=%d,chan=%d,cookie=%016llx,rw=%d"
 * into the VMT_INFO field of the vmount structure passed to vmount(). */

/* The kernel and the library compile this header separately, so the padding of
 * every message body has to be written out rather than left to the compiler to
 * invent. A size that is not a multiple of 8 means a member needed alignment
 * the explicit padding did not provide, so the compiler inserted its own --
 * exactly the thing these declarations exist to prevent. Each of these fails
 * the build in that case, in both compilations. */
#define USFS_CHECK_SIZE(name) \
    typedef char usfs_size_check_##name[((sizeof (struct name) % 8u) == 0) ? 1 : -1]

USFS_CHECK_SIZE (usfs_in_hdr);
USFS_CHECK_SIZE (usfs_open_in);
USFS_CHECK_SIZE (usfs_getattr_in);
USFS_CHECK_SIZE (usfs_read_in);
USFS_CHECK_SIZE (usfs_release_in);
USFS_CHECK_SIZE (usfs_readdir_in);
USFS_CHECK_SIZE (usfs_vget_in);
USFS_CHECK_SIZE (usfs_create_in);
USFS_CHECK_SIZE (usfs_create_attr_in);
USFS_CHECK_SIZE (usfs_forget_in);
USFS_CHECK_SIZE (usfs_mkdir_in);
USFS_CHECK_SIZE (usfs_rename_in);
USFS_CHECK_SIZE (usfs_symlink_in);
USFS_CHECK_SIZE (usfs_link_in);
USFS_CHECK_SIZE (usfs_setattr_in);
USFS_CHECK_SIZE (usfs_write_in);
USFS_CHECK_SIZE (usfs_reply_header);
USFS_CHECK_SIZE (usfs_attr);
USFS_CHECK_SIZE (usfs_entry_out);
USFS_CHECK_SIZE (usfs_fid_out);
USFS_CHECK_SIZE (usfs_vget_out);
USFS_CHECK_SIZE (usfs_attr_out);
USFS_CHECK_SIZE (usfs_open_out);
USFS_CHECK_SIZE (usfs_create_out);
USFS_CHECK_SIZE (usfs_write_out);
USFS_CHECK_SIZE (usfs_statfs_out);
USFS_CHECK_SIZE (usfs_readdir_out);

#endif /* USFS_PROTO_H */
