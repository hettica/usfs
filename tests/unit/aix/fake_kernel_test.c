/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "support/fake_kernel.h"

int main(void)
{
    struct tap_state tap;
    struct ucred credential;

    memset(&credential, 0, sizeof(credential));
    tap_plan(&tap, 7);
    usfs_fake_kernel_reset();
    tap_ok(&tap, usfs_fake_kernel_clean(), "fresh fake state is clean");
    tap_ok(&tap, privcheck_cr(BYPASS_DAC, &credential) == EPERM,
           "privilege is denied by default");
    usfs_fake_kernel.bypass_dac = 1;
    tap_ok(&tap, privcheck_cr(BYPASS_DAC, &credential) == 0,
           "scripted privilege is granted");
    usfs_fake_kernel.group_member = 1;
    usfs_fake_kernel.member_gid = 42;
    tap_ok(&tap, groupmember_cr(42, &credential),
           "scripted group membership matches only its gid");
    tap_ok(&tap, !groupmember_cr(43, &credential),
           "different group is rejected");
    tap_ok(&tap, usfs_fake_kernel.event_count == 4 &&
                 usfs_fake_kernel.events[0] == USFS_FAKE_EVENT_PRIVCHECK &&
                 usfs_fake_kernel.events[2] == USFS_FAKE_EVENT_GROUPMEMBER,
           "fake calls retain deterministic event order");
    usfs_fake_kernel_reset();
    tap_ok(&tap, usfs_fake_kernel_clean() &&
                 usfs_fake_kernel.event_count == 0,
           "reset clears counters and ownership state");
    return tap_finish(&tap);
}
