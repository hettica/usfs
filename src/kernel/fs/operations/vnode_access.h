// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_OPERATIONS_VNODE_ACCESS_H
#define USFS_FS_OPERATIONS_VNODE_ACCESS_H

/* Vnode attribute operations. */

static int access_class_permissions (const mode_t object_mode, const int shift)
{
    return (int)((object_mode >> shift) & 07);
}

static int access_class_allows (const mode_t object_mode, const int shift, const int requested)
{
    const int permissions = access_class_permissions (object_mode, shift);
    return (permissions & requested) == requested;
}

static int access_privilege_allows (const int requested, const int object_type, struct ucred * credentials)
{
    if (privcheck_cr (BYPASS_DAC, credentials) == 0)
        return 1;

    if ((requested & R_ACC) != 0 && privcheck_cr (BYPASS_DAC_READ, credentials) != 0)
    {
        return 0;
    }

    if ((requested & W_ACC) != 0 && privcheck_cr (BYPASS_DAC_WRITE, credentials) != 0)
    {
        return 0;
    }

    if ((requested & X_ACC) != 0)
    {
        const int privilege = object_type == VDIR ? BYPASS_DAC_READ : BYPASS_DAC_EXEC;
        if (privcheck_cr (privilege, credentials) != 0)
            return 0;
    }

    return 1;
}

static int access_self (const struct vattr * attributes, int requested, struct ucred * credentials)
{
    int permissions;

    if (credentials->cr_uid == attributes->va_uid)
    {
        permissions = access_class_permissions (attributes->va_mode, 6);
    }
    else if (groupmember_cr (attributes->va_gid, credentials))
    {
        permissions = access_class_permissions (attributes->va_mode, 3);
    }
    else
    {
        permissions = access_class_permissions (attributes->va_mode, 0);
    }

    requested &= ~permissions;

    if (requested == 0 || access_privilege_allows (requested, attributes->va_type, credentials))
    {
        return 0;
    }

    return EACCES;
}

static int access_for_subject (const struct vattr * attributes, const int requested, const int32long64_t subject_selector, struct ucred * credentials)
{
    const int owner = access_class_allows (attributes->va_mode, 6, requested);
    const int group = access_class_allows (attributes->va_mode, 3, requested);
    const int other = access_class_allows (attributes->va_mode, 0, requested);

    switch (subject_selector)
    {
        case ACC_SELF:
            return access_self (attributes, requested, credentials);
        case ACC_ANY:
            return owner || group || other ? 0 : EACCES;
        case ACC_OTHERS:
            return group || other ? 0 : EACCES;
        case ACC_ALL:
            return owner && group && other ? 0 : EACCES;
        default:
            return EINVAL;
    }
}

int gn_access (struct vnode * file_vnode, int32long64_t access_mode, int32long64_t subject_selector, struct ucred * credentials)
{
    struct usfs_mount_data * mount_data = _mount_of (file_vnode);

    if (mount_data == NULL || credentials == NULL)
        return EIO;

    if ((access_mode & ~(int32long64_t)(R_ACC | W_ACC | X_ACC)) != 0)
        return EINVAL;

    if ((access_mode & W_ACC) != 0 && !mount_data->writable)
        return EROFS;

    if (access_mode == E_ACC)
        return 0;

    if (subject_selector != ACC_SELF && access_mode != R_ACC && access_mode != W_ACC && access_mode != X_ACC)
        return EINVAL;

    struct vattr attributes;
    const int getattr_rc = gn_getattr (file_vnode, &attributes, credentials);
    if (getattr_rc != 0)
        return getattr_rc;

    return access_for_subject (&attributes, (int)access_mode, subject_selector, credentials);
}

#endif
