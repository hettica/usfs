// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_NAMESPACE_SECURITY_H
#define USFS_NAMESPACE_SECURITY_H

static int usfs_check_sticky_entry (struct vnode * directory_vnode, const char * entry_name, struct ucred * credentials, const int may_be_absent)
{
    if (credentials == NULL)
        return EINVAL;

    if (entry_name == NULL)
        return EINVAL;

    if (privcheck_cr (PV_DAC_O, credentials) == 0)
        return 0;

    struct vattr attributes;

    int rc = gn_getattr (directory_vnode, &attributes, credentials);
    if (rc != 0)
        return rc;

    if ((attributes.va_mode & S_ISVTX) == 0)
        return 0;

    if (attributes.va_uid == credentials->cr_uid)
        return 0;

    struct vnode * target_vnode = NULL;

    rc = gn_lookup (directory_vnode, &target_vnode, (char *)entry_name, 0, &attributes, credentials);
    if (rc == ENOENT && may_be_absent)
        return 0;

    if (rc != 0)
        return rc;

    rc = attributes.va_uid == credentials->cr_uid ? 0 : EPERM;
    gn_rele (target_vnode);

    return rc;
}

#endif
