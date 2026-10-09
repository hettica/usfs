/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "definitions.h"
#include "instrumentation/testing/runtime.h"

#include <sys/lkup.h>

/* The probe uses the native AIX lookup, fid and fidtovp entry points. */
typedef char usfs_test_fileid_size_check[(sizeof (struct fileid) == USFS_TEST_FID_RAW_BYTES) ? 1 : -1];
typedef char usfs_test_fsid_size_check[(sizeof (fsid_t) == USFS_TEST_FSID_RAW_BYTES) ? 1 : -1];

enum
{
    FID_PROBE_ALLOCATION_ALIGNMENT = 4
};

static int valid_probe_path (const char * path)
{
    if (path[0] != '/')
        return false;

    return memchr (path, '\0', USFS_TEST_FID_PATH_MAX) != NULL;
}

static int is_usfs_vnode (const struct vnode * file_vnode)
{
    if (file_vnode == NULL)
        return false;

    if (file_vnode->v_vfsp == NULL)
        return false;

    if (file_vnode->v_vfsp->vfs_gfs == NULL)
        return false;

    return file_vnode->v_vfsp->vfs_type == gfs.gfs_type;
}

static int lookup_usfs_vnode (char * path, struct ucred * credentials, struct vnode ** result_vnode)
{
    *result_vnode = NULL;

    int rc = lookupvp (path, L_NOFOLLOW, result_vnode, credentials);
    if (rc != 0)
        return rc;

    if (*result_vnode == NULL)
        return EIO;

    if (!is_usfs_vnode (*result_vnode))
    {
        VNOP_RELE (*result_vnode);
        *result_vnode = NULL;
        return EXDEV;
    }

    return 0;
}

static int read_vnode_identity (struct vnode * file_vnode, struct ucred * credentials, uint32_t * vnode_type, uint64_t * inode)
{
    struct vattr attributes = { 0 };

    const int rc = VNOP_GETATTR (file_vnode, &attributes, credentials);
    if (rc != 0)
        return rc;

    *vnode_type = (uint32_t)file_vnode->v_vntype;
    *inode = (uint64_t)attributes.va_serialno;
    return 0;
}

static int capture_identifier (struct usfs_test_fid_probe * request, struct ucred * credentials)
{
    struct vnode * file_vnode = NULL;
    int rc = lookup_usfs_vnode (request->path, credentials, &file_vnode);
    if (rc != 0)
        return rc;

    struct fileid file_id = { 0 };
    rc = VNOP_FID (file_vnode, &file_id, credentials);
    if (rc == 0)
    {
        struct vnode * resolved_vnode = NULL;

        rc = fidtovp (&file_vnode->v_vfsp->vfs_fsid, &file_id, &resolved_vnode, credentials);
        if (rc == 0)
        {
            if (resolved_vnode == NULL || resolved_vnode != file_vnode)
                rc = EIO;
        }

        if (resolved_vnode != NULL)
            VNOP_RELE (resolved_vnode);
    }

    if (rc == 0)
        rc = read_vnode_identity (file_vnode, credentials, &request->vnode_type, &request->inode);

    if (rc == 0)
        memcpy (request->file_id, &file_id, sizeof (file_id));

    if (rc == 0)
        memcpy (request->file_system_id, &file_vnode->v_vfsp->vfs_fsid, sizeof (fsid_t));

    VNOP_RELE (file_vnode);
    return rc;
}

static int compare_expected_vnode (struct usfs_test_fid_probe * request, struct vnode * resolved_vnode, struct ucred * credentials)
{
    if (request->expected_path[0] == '\0')
        return 0;

    struct vnode * expected_vnode = NULL;
    int rc = lookup_usfs_vnode (request->expected_path, credentials, &expected_vnode);
    if (rc != 0)
        return rc;

    if (expected_vnode != resolved_vnode)
        rc = EIO;

    VNOP_RELE (expected_vnode);
    return rc;
}

static int verify_resolved_identifier (struct usfs_test_fid_probe * request, struct vnode * resolved_vnode, struct ucred * credentials)
{
    struct fileid reencoded_file_id = { 0 };

    int rc = VNOP_FID (resolved_vnode, &reencoded_file_id, credentials);
    if (rc != 0)
        return rc;

    if (memcmp (&reencoded_file_id, request->file_id, sizeof (reencoded_file_id)) != 0)
        return EIO;

    uint32_t vnode_type = 0;
    uint64_t inode = 0;
    rc = read_vnode_identity (resolved_vnode, credentials, &vnode_type, &inode);
    if (rc != 0)
        return rc;

    if (vnode_type != request->vnode_type || inode != request->inode)
        return EIO;

    return compare_expected_vnode (request, resolved_vnode, credentials);
}

static int resolve_identifier (struct usfs_test_fid_probe * request, struct ucred * credentials)
{
    struct fileid file_id = { 0 };
    memcpy (&file_id, request->file_id, sizeof (file_id));

    fsid_t file_system_id = { 0 };
    memcpy (&file_system_id, request->file_system_id, sizeof (file_system_id));

    struct vnode * resolved_vnode = NULL;
    int rc = fidtovp (&file_system_id, &file_id, &resolved_vnode, credentials);
    if (rc == 0)
    {
        if (resolved_vnode == NULL)
            rc = EIO;
        else if (!is_usfs_vnode (resolved_vnode))
            rc = EXDEV;
        else
            rc = verify_resolved_identifier (request, resolved_vnode, credentials);
    }

    if (resolved_vnode != NULL)
        VNOP_RELE (resolved_vnode);

    return rc;
}

static int validate_probe_request (const struct usfs_test_fid_probe * request)
{
    if (request->abi_version != USFS_TEST_ABI_VERSION)
        return EINVAL;

    if (request->action != USFS_TEST_FID_CAPTURE && request->action != USFS_TEST_FID_RESOLVE)
        return EINVAL;

    if (request->reserved != 0)
        return EINVAL;

    if (!valid_probe_path (request->path))
        return EINVAL;

    if (request->expected_path[0] != '\0' && !valid_probe_path (request->expected_path))
        return EINVAL;

    if (request->action == USFS_TEST_FID_CAPTURE && request->expected_path[0] != '\0')
        return EINVAL;

    return 0;
}

int usfs_test_fid_probe (void * user_buffer)
{
    if (privcheck (DEV_CONFIG) != 0)
        return EPERM;

    struct usfs_test_fid_probe * request = xmalloc (sizeof (*request), FID_PROBE_ALLOCATION_ALIGNMENT, kernel_heap);
    if (request == NULL)
        return ENOMEM;

    int rc = copyin ((caddr_t)user_buffer, (caddr_t)request, sizeof (*request)) == 0 ? 0 : EFAULT;
    if (rc == 0)
        rc = validate_probe_request (request);

    if (rc == 0)
    {
        struct ucred * credentials = crref ();
        if (credentials == NULL)
        {
            rc = ENOMEM;
        }
        else
        {
            if (request->action == USFS_TEST_FID_CAPTURE)
                rc = capture_identifier (request, credentials);
            else
                rc = resolve_identifier (request, credentials);

            crfree (credentials);
        }
    }

    if (rc == 0)
        rc = copyout ((caddr_t)request, (caddr_t)user_buffer, sizeof (*request)) == 0 ? 0 : EFAULT;

    xmfree (request, kernel_heap);
    return rc;
}
