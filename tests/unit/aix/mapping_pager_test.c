/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"

#include <string.h>

int main(void)
{
    struct tap_state tap;
    struct vnode vnode;
    struct gnode gnode;
    struct ucred credential;
    struct buf buffer;
    uint64_t offset = 0;
    int access = 0;
    char byte = 0;

    tap_plan(&tap, 13);
    memset(&vnode, 0, sizeof(vnode));
    memset(&gnode, 0, sizeof(gnode));
    memset(&credential, 0, sizeof(credential));
    memset(&buffer, 0, sizeof(buffer));
    tap_ok(&tap, usfs_validate_mapping_arguments(NULL, 1, 0, 0,
                                                  &credential, &access) ==
                 EINVAL,
           "mapping rejects null vnode");
    vnode.v_gnode = &gnode;
    gnode.gn_type = VDIR;
    tap_ok(&tap, usfs_validate_mapping_arguments(&vnode, 1, 0, 0,
                                                  &credential, &access) ==
                 ENODEV,
           "mapping rejects nonregular vnode");
    gnode.gn_type = VREG;
    tap_ok(&tap, usfs_validate_mapping_arguments(&vnode, 0, 0, 0,
                                                  &credential, &access) ==
                 EINVAL,
           "mapping rejects zero length");
    tap_ok(&tap, usfs_validate_mapping_arguments(
                     &vnode, 1, (uint64_t)INT64_MAX + 1u, 0,
                     &credential, &access) == EINVAL,
           "mapping rejects offset beyond signed file range");
    tap_ok(&tap, usfs_validate_mapping_arguments(
                     &vnode, 2, (uint64_t)INT64_MAX - 1u, 0,
                     &credential, &access) == EINVAL,
           "mapping rejects offset plus length overflow");
    tap_ok(&tap, usfs_validate_mapping_arguments(&vnode, 1, 0, 0,
                                                  &credential, &access) ==
                     0 &&
                 access == (R_ACC | W_ACC),
           "writable mapping requests read and write access");
    tap_ok(&tap, usfs_validate_mapping_arguments(&vnode, 1, 0, SHM_RDONLY,
                                                  &credential, &access) ==
                     0 &&
                 access == R_ACC,
           "readonly mapping requests read access only");

    tap_ok(&tap, usfs_pager_offset(NULL, &offset) == EINVAL,
           "pager rejects null buffer");
    tap_ok(&tap, usfs_pager_offset(&buffer, NULL) == EINVAL,
           "pager rejects null offset output");
    buffer.b_blkno = -1;
    tap_ok(&tap, usfs_pager_offset(&buffer, &offset) == EINVAL,
           "pager rejects negative block number");
    buffer.b_blkno = 2;
    tap_ok(&tap, usfs_pager_offset(&buffer, &offset) == 0 &&
                 offset == 2u * (uint64_t)UBSIZE,
           "pager converts block number with checked arithmetic");
    tap_ok(&tap, !usfs_pager_count_valid(NULL, 1) &&
                 !usfs_pager_count_valid(&byte, 0) &&
                 !usfs_pager_count_valid(&byte, USFS_MAX_DATA + 1u),
           "pager rejects invalid data ranges");
    tap_ok(&tap, usfs_pager_count_valid(&byte, USFS_MAX_DATA),
           "pager accepts its maximum transfer size");
    return tap_finish(&tap);
}
