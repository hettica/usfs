/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 *
 * Enable the existing admission-wait seam only in the multithread test daemon.
 * The production library retains its ordinary, uninstrumented implementation.
 */

static void report_namespace_wait (const int exclusive);

#define USFS_NAMESPACE_WAITING(exclusive) report_namespace_wait (exclusive)
#include "../../src/client/lib/usfs_client.c"
#undef USFS_NAMESPACE_WAITING

static void report_namespace_wait (const int exclusive)
{
    if (getenv ("USFS_MT_NAMESPACE_RELEASE") == NULL)
        return;

    const struct usfs_client_request * context = get_callback_request ();

    printf ("USFS_NAMESPACE_WAITING pid=%ld exclusive=%d\n", (long)context->pid, exclusive);
    fflush (stdout);
}
