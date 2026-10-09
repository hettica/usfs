/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "usfs_common.h"
#include "usfs_config_instrumentation.h"

/* Actual transaction code, with no live ODM, device, or kernel side effects. */
static int loaded, available, number_present, node_present, registered;
static uint32_t lifecycle;
static int init_fails, term_fails, query_fails, malformed_query, odm_fails;
static int init_calls, term_calls, unload_calls, release_calls, unsafe_actions;
static dev_t owned_number, last_term_number;
static int minor_query_fails, extra_minor, minor_number = 17;
static int checkpoint_fault;
static int foreign_odm, foreign_node, register_calls;
const char *USFS_DRIVER_NAME = "usfs";

static struct cfg_load usfs_kext_make_query(void)
{
    struct cfg_load query;
    memset(&query, 0, sizeof(query));
    return query;
}
mid_t usfs_kext_get_module_id(void) { return loaded ? 123 : 0; }
int usfs_kext_is_loaded(void) { return loaded; }
int usfs_kext_load(void) { loaded = 1; return 0; }
int usfs_type_is_registered(void) { return registered; }
int usfs_type_register(const char *name) { (void)name; registered = 1; register_calls++; return 0; }
int usfs_type_unregister(void) { registered = 0; return 0; }
int usfs_config_lock_acquire(void) { return 7; }
int usfs_config_lock_release(int lock) { return lock != 7; }
int usfs_config_instrumentation_checkpoint(enum usfs_config_method_checkpoint point)
{ return (int)point == checkpoint_fault; }
int genmajor(char *driver)
{ (void)driver; return 41; }
int *getminor(int major, int *count, char *driver)
{
    (void)major; (void)driver;
    if (minor_query_fails) return NULL;
    *count = number_present ? 1 + extra_minor : 0;
    return number_present ? &minor_number : NULL;
}
int *genminor(char *driver, int major, int base, int count, int step, int group)
{
    (void)driver; (void)major; (void)base; (void)count; (void)step; (void)group;
    number_present = 1;
    return &minor_number;
}
int reldevno(char *driver, int release_major)
{
    (void)driver; (void)release_major;
    if (!number_present) return -1;
    release_calls++;
    if (lifecycle != USFS_KEXT_STATE_DOWN) unsafe_actions++;
    number_present = 0;
    return 0;
}
int relmajor(char *driver)
{
    (void)driver;
    release_calls++;
    if (lifecycle != USFS_KEXT_STATE_DOWN || number_present) unsafe_actions++;
    return 0;
}
static int usfs_create_device_node(const char *name, dev_t number)
{ (void)name; (void)number; node_present = 1; return 0; }
static int usfs_validate_device_node(const char *name, dev_t number, int state)
{ (void)name; (void)number; (void)state; return foreign_node; }
static int usfs_remove_device_node(const char *name, dev_t number, int state)
{ if (usfs_validate_device_node(name, number, state)) return 1; node_present = 0; return 0; }
static int usfs_mark_device_available(const char *name)
{ (void)name; if (odm_fails) return 1; available = 1; return 0; }
static int usfs_mark_device_defined(const char *name)
{ (void)name; available = 0; return 0; }
static int usfs_device_is_available(const char *name)
{ (void)name; return foreign_odm ? USFS_QUERY_ERROR : available; }
static int usfs_devsw_send_command(mid_t module, dev_t number, int command)
{
    (void)module;
    if (command == USFS_CFG_INIT) {
        init_calls++;
        lifecycle = init_fails ? USFS_KEXT_STATE_CLEANUP_REQUIRED :
                                USFS_KEXT_STATE_ACTIVE;
        return init_fails;
    }
    term_calls++;
    last_term_number = number;
    if (number != owned_number) { unsafe_actions++; return 1; }
    if (term_fails) { lifecycle = USFS_KEXT_STATE_CLEANUP_REQUIRED; return 1; }
    lifecycle = USFS_KEXT_STATE_DOWN;
    return 0;
}
static int fake_sysconfig(int command, void *data, int length)
{
    (void)length;
    if (command == SYS_KULOAD) {
        unload_calls++;
        if (lifecycle != USFS_KEXT_STATE_DOWN) unsafe_actions++;
        loaded = 0;
        return 0;
    }
    if (command == SYS_CFGDD) {
        struct cfg_dd *query = data;
        struct usfs_kext_lifecycle_query *request =
            (struct usfs_kext_lifecycle_query *)query->ddsptr;
        struct usfs_kext_lifecycle_state *state =
            (struct usfs_kext_lifecycle_state *)(unsigned long)request->user_buffer;
        if (query_fails) { errno = EIO; return -1; }
        memset(state, 0, sizeof(*state));
        state->magic = USFS_CONFIG_DDS_MAGIC;
        state->abi_version = USFS_CONFIG_ABI_VERSION;
        state->size = sizeof(*state);
        state->state = lifecycle;
        state->reserved[0] = malformed_query;
        return 0;
    }
    unsafe_actions++;
    return -1;
}
#define sysconfig fake_sysconfig
#include "configuration_transactions.h"
#undef sysconfig

static void reset(void)
{
    loaded = available = number_present = node_present = registered = 0;
    lifecycle = USFS_KEXT_STATE_DOWN;
    init_fails = term_fails = query_fails = malformed_query = odm_fails = 0;
    init_calls = term_calls = unload_calls = release_calls = unsafe_actions = 0;
    minor_query_fails = extra_minor = 0;
    checkpoint_fault = 0;
    foreign_odm = foreign_node = register_calls = 0;
    owned_number = makedev64(41, 17);
    last_term_number = 0;
}

#include "vfs_text.h"
static void test_vfs_text(struct tap_state *tap)
{
    FILE *stream = tmpfile();
    char line[16];
    if (stream == NULL) { tap_ok(tap, 0, "VFS text fixture available"); return; }
    fputs("usfs 7 none\nlast", stream); rewind(stream);
    tap_ok(tap, usfs_vfs_read_line(stream, line, sizeof(line)) == 1 &&
           strcmp(line, "usfs 7 none\n") == 0 &&
           usfs_vfs_read_line(stream, line, sizeof(line)) == 1 && strcmp(line, "last") == 0 &&
           usfs_vfs_read_line(stream, line, sizeof(line)) == 0,
           "VFS text parser preserves complete entries and a final line without newline");
    fclose(stream); stream = tmpfile();
    if (stream == NULL) { tap_ok(tap, 0, "VFS NUL fixture available"); return; }
    fwrite("usfs\0hidden\n", 1, 12, stream); rewind(stream);
    tap_ok(tap, usfs_vfs_read_line(stream, line, sizeof(line)) == -1,
           "VFS text parser rejects embedded NUL instead of truncating system configuration");
    fclose(stream); stream = tmpfile();
    if (stream == NULL) { tap_ok(tap, 0, "VFS long-line fixture available"); return; }
    fputs("01234567890123456789\n", stream); rewind(stream);
    tap_ok(tap, usfs_vfs_read_line(stream, line, sizeof(line)) == -1,
           "VFS text parser rejects an oversized line before interpreting its continuation");
    fclose(stream);
}

int main(void)
{
    struct tap_state tap;
    uint32_t state = 99;
    dev_t number;
    int rc;
    tap_plan(&tap, 23);
    reset();
    tap_ok(&tap, usfs_configure("hdisk0") != 0 &&
        usfs_unconfigure("hdisk0") != 0 && !loaded && !init_calls &&
        !term_calls && !release_calls, "foreign names cannot mutate device state");
    reset();
    foreign_odm = 1;
    tap_ok(&tap, usfs_configure("usfs0") != 0 &&
        usfs_unconfigure("usfs0") != 0 && !loaded && !init_calls &&
        !term_calls && !release_calls, "foreign ODM identity fails before mutation");
    reset();
    loaded = available = number_present = node_present = 1;
    lifecycle = USFS_KEXT_STATE_ACTIVE;
    foreign_node = 1;
    tap_ok(&tap, usfs_unconfigure("usfs0") != 0 && loaded && node_present &&
        available && !term_calls && !release_calls && !unload_calls,
        "foreign node is rejected before kernel termination");
    reset();
    foreign_node = 1;
    tap_ok(&tap, usfs_configure("usfs0") != 0 && !loaded && !init_calls &&
        !term_calls && !unload_calls && !release_calls,
        "foreign node is rejected before loading the extension");
    reset();
    loaded = available = number_present = node_present = registered = 1;
    lifecycle = USFS_KEXT_STATE_ACTIVE;
    tap_ok(&tap, usfs_configure("usfs0") == 0 && register_calls == 1 &&
        !init_calls, "existing VFS registration is reconciled with the live type");
    reset();
    query_fails = 1;
    tap_ok(&tap, usfs_query_kext_lifecycle(123, owned_number, &state) ==
        USFS_QUERY_ERROR && state == 99, "failed lifecycle query is an error, not absence");

    reset();
    loaded = available = number_present = node_present = registered = 1;
    lifecycle = USFS_KEXT_STATE_ACTIVE;
    query_fails = 1;
    rc = usfs_unconfigure("usfs0");
    tap_ok(&tap, rc != 0 && loaded && number_present && node_present &&
        !release_calls && !unload_calls && !unsafe_actions,
        "query failure retains every recovery record and forbids unload");
    query_fails = 0;
    tap_ok(&tap, usfs_unconfigure("usfs0") == 0 &&
        last_term_number == owned_number && !loaded && !number_present &&
        !unsafe_actions, "retry terminates using the original device number");

    reset();
    init_fails = term_fails = 1;
    rc = usfs_configure("usfs0");
    tap_ok(&tap, rc != 0 && loaded && number_present && node_present &&
        !release_calls && !unload_calls && !unsafe_actions,
        "failed initialization unwind preserves registrations and recovery ownership");
    init_fails = term_fails = 0;
    tap_ok(&tap, usfs_unconfigure("usfs0") == 0 &&
        last_term_number == owned_number && !loaded && !unsafe_actions,
        "failed initialization remains cleanable through its original registration");

    reset();
    odm_fails = term_fails = 1;
    rc = usfs_configure("usfs0");
    tap_ok(&tap, rc != 0 && loaded && number_present && node_present &&
        term_calls == 1 && !release_calls && !unload_calls && !unsafe_actions,
        "failed CFG_TERM after ODM failure retains all cleanup ownership");
    odm_fails = term_fails = 0;
    tap_ok(&tap, usfs_unconfigure("usfs0") == 0 &&
        last_term_number == owned_number && !loaded && !unsafe_actions,
        "subsequent cleanup after ODM failure uses the original number");

    reset();
    loaded = 1;
    lifecycle = USFS_KEXT_STATE_ACTIVE;
    tap_ok(&tap, usfs_kext_unload() != 0 && loaded && !unload_calls,
        "direct unload rejects live kernel registrations");
    reset();
    loaded = 1;
    query_fails = 1;
    tap_ok(&tap, usfs_kext_unload() != 0 && loaded && !unload_calls,
        "direct unload rejects uncertain lifecycle state");
    reset();
    loaded = available = number_present = node_present = registered = 1;
    lifecycle = USFS_KEXT_STATE_ACTIVE;
    malformed_query = 1;
    tap_ok(&tap, usfs_unconfigure("usfs0") != 0 && loaded &&
        number_present && node_present && !release_calls && !unload_calls,
        "malformed lifecycle replies retain ownership");
    reset();
    minor_query_fails = 1;
    tap_ok(&tap, usfs_lookup_device_number(&number) == USFS_QUERY_ERROR &&
        !release_calls, "failed minor query cannot release the existing allocation");
    reset();
    tap_ok(&tap, usfs_lookup_device_number(&number) == USFS_QUERY_ABSENT &&
        !release_calls, "verified minor absence does not release a potentially owned major");
    reset();
    number_present = extra_minor = 1;
    tap_ok(&tap, usfs_lookup_device_number(&number) == USFS_QUERY_ERROR &&
        !release_calls, "ambiguous multiple minors fail without choosing a registration");
    reset();
    loaded = available = number_present = node_present = registered = 1;
    lifecycle = USFS_KEXT_STATE_ACTIVE;
    checkpoint_fault = USFS_CONFIG_METHOD_AFTER_DEVICE_CLEANUP;
    tap_ok(&tap, usfs_unconfigure("usfs0") != 0 && loaded && available &&
        !number_present && lifecycle == USFS_KEXT_STATE_DOWN && !unsafe_actions,
        "post-device-cleanup failure retains loaded DOWN state for retry");
    checkpoint_fault = 0;
    tap_ok(&tap, usfs_unconfigure("usfs0") == 0 && !loaded && !available &&
        term_calls == 1 && !unsafe_actions,
        "minor-free retry releases only its major and never repeats CFG_TERM");
    test_vfs_text(&tap);
    return tap_finish(&tap);
}
