/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "support/fake_kernel.h"

#include <limits.h>
#include <string.h>
#include <sys/statfs.h>

struct gfs gfs;
static struct usfs_request statistics_request;
static struct usfs_statfs_out statistics_reply;
static int allocation_error, transport_error, request_balance;

int allocate_request(const struct usfs_request_allocation_spec *spec,
                      struct usfs_request **request)
{
    (void)spec;
    if (allocation_error) return allocation_error;
    ++request_balance;
    *request = &statistics_request;
    return 0;
}

void free_request(struct usfs_request *request)
{
    (void)request;
    --request_balance;
}

int usfs_call(struct usfs_connection *connection, struct usfs_request *request)
{
    (void)connection;
    if (transport_error) free_request(request);
    return transport_error;
}

#include "fs/operations/vfs_statfs.h"

static void test_lock_occurrences(struct tap_state *tap)
{
    uint32_t used[USFS_LOCK_OCCURRENCE_WORDS];
    short occurrence = -1;

    memset(used, 0, sizeof(used));
    tap_ok(tap, usfs_allocate_lock_occurrence(used, &occurrence) == 0 &&
                 occurrence == 0 && used[0] == 1,
           "first lock occurrence is allocated");
    tap_ok(tap, usfs_allocate_lock_occurrence(used, &occurrence) == 0 &&
                 occurrence == 1 && used[0] == 3,
           "lock occurrences advance through the bitmap");
    usfs_free_lock_occurrence(used, 0);
    tap_ok(tap, usfs_allocate_lock_occurrence(used, &occurrence) == 0 &&
                 occurrence == 0,
           "freed lock occurrence is reused");
    usfs_free_lock_occurrence(used, -1);
    usfs_free_lock_occurrence(used, SHRT_MAX);
    tap_ok(tap, used[0] == 3,
           "invalid lock occurrences do not alter the bitmap");
    memset(used, 0xff, sizeof(used));
    tap_ok(tap, usfs_allocate_lock_occurrence(used, &occurrence) == EAGAIN,
           "exhausted lock occurrence bitmap fails safely");
}

static void test_mount_text(struct tap_state *tap)
{
    unsigned char storage[sizeof(struct vmount) + 8u];
    const size_t allocation = sizeof(storage);
    struct vmount *mount = (struct vmount *)storage;
    const char *text = NULL;
    uint32_t length = 0;

    memset(storage, 0, sizeof(storage));
    tap_ok(tap, 1, "vmount fixture allocated");
    mount->vmt_revision = VMT_REVISION;
    mount->vmt_length = (uint32_t)allocation;
    mount->vmt_data[VMT_OBJECT].vmt_off = (short)sizeof(*mount);
    mount->vmt_data[VMT_OBJECT].vmt_size = 5;
    memcpy((char *)mount + sizeof(*mount), "name", 5);
    tap_ok(tap, usfs_vmt_text_field(mount, VMT_OBJECT, &text, &length) == 0 &&
                 length == 5 && strcmp(text, "name") == 0,
           "bounded vmount text is returned");
    mount->vmt_revision += 1;
    tap_ok(tap, usfs_vmt_text_field(mount, VMT_OBJECT, &text, &length) ==
                 EINVAL,
           "wrong vmount revision is rejected");
    mount->vmt_revision = VMT_REVISION;
    mount->vmt_data[VMT_OBJECT].vmt_off = 1;
    tap_ok(tap, usfs_vmt_text_field(mount, VMT_OBJECT, &text, &length) ==
                 EINVAL,
           "vmount text overlapping its header is rejected");
    mount->vmt_data[VMT_OBJECT].vmt_off = (short)sizeof(*mount);
    ((char *)mount)[sizeof(*mount) + 4u] = 'x';
    tap_ok(tap, usfs_vmt_text_field(mount, VMT_OBJECT, &text, &length) ==
                 EINVAL,
           "unterminated vmount text is rejected");
    tap_ok(tap, usfs_vmt_text_field(NULL, VMT_OBJECT, &text, &length) ==
                 EINVAL &&
                 usfs_vmt_text_field(mount, -1, &text, &length) == EINVAL,
           "null and invalid vmount inputs are rejected");
}

static void test_type_and_attributes(struct tap_state *tap)
{
    struct usfs_attr source;
    struct vattr target;

    tap_ok(tap, usfs_vtype_from_mode(S_IFREG) == VREG &&
                 usfs_vtype_from_mode(S_IFDIR) == VDIR &&
                 usfs_vtype_from_mode(S_IFLNK) == VLNK,
           "known protocol modes map to AIX vnode types");
    tap_ok(tap, usfs_vtype_from_mode(0) == VREG,
           "unknown protocol mode preserves the regular-file fallback");
    memset(&source, 0, sizeof(source));
    source.ino = 9;
    source.mode = S_IFREG | 0640;
    source.nlink = 2;
    source.uid = 1000;
    source.gid = 100;
    source.size = 123;
    source.blksize = 4096;
    source.blocks = 1;
    source.atime = 10;
    source.atimensec = 20;
    memset(&target, 0xff, sizeof(target));
    usfs_attr_to_vattr(&source, &target);
    tap_ok(tap, target.va_serialno == 9 && target.va_type == VREG &&
                 target.va_mode == (S_IFREG | 0640) &&
                 target.va_size == 123 &&
                 target.va_atime.tv_sec == 10 &&
                 target.va_atime.tv_nsec == 20,
           "protocol attributes convert to an initialized AIX vattr");
}

static void test_statistics(struct tap_state *tap)
{
    struct usfs_mount_data mount = { 0 };
    struct usfs_connection connection = { 0 };
    struct vfs vfs = { 0 };
    struct vmount metadata = { 0 };
    struct statfs output, unchanged;
    unsigned scenario;
    const int errors[] = { ENOMEM, ETIMEDOUT, EACCES, EIO, EIO, EIO, EIO, EIO };
    const char *names[] = {
        "STATFS allocation failure returns ENOMEM without publishing capacity",
        "STATFS transport failure preserves its errno without publishing capacity",
        "STATFS callback failure preserves its errno without publishing capacity",
        "STATFS short reply returns EIO without publishing capacity",
        "STATFS zero block size returns EIO without publishing capacity",
        "STATFS impossible free count returns EIO without publishing capacity",
        "STATFS missing mount returns EIO without publishing capacity",
        "STATFS missing connection returns EIO without publishing capacity"
    };
    vfs.vfs_mdata = &metadata;
    vfs.vfs_number = 17;
    gfs.gfs_type = 37;
    memset(&unchanged, 0x5a, sizeof(unchanged));
    for (scenario = 0; scenario < 8; ++scenario) {
        allocation_error = scenario == 0 ? ENOMEM : 0;
        transport_error = scenario == 1 ? ETIMEDOUT : 0;
        statistics_request.error = scenario == 2 ? EACCES : 0;
        statistics_request.reply_buffer = (char *)&statistics_reply;
        statistics_request.reply_buffer_size = sizeof(statistics_reply) - (scenario == 3);
        memset(&statistics_reply, 0, sizeof(statistics_reply));
        statistics_reply.bsize = scenario == 4 ? 0 : 4096;
        statistics_reply.bfree = scenario == 5 ? 1 : 0;
        statistics_reply.namemax = 255;
        vfs.vfs_data = scenario == 6 ? NULL : (caddr_t)&mount;
        mount.conn = scenario == 7 ? NULL : &connection;
        output = unchanged;
        tap_ok(tap, usfs_statfs(&vfs, &output, NULL) == errors[scenario] &&
               memcmp(&output, &unchanged, sizeof(output)) == 0 && request_balance == 0,
               names[scenario]);
    }
    mount.conn = &connection;
    tap_ok(tap, usfs_statfs(&vfs, &output, NULL) == 0 && output.f_blocks == 0 &&
           output.f_bfree == 0 && output.f_bavail == 0 && output.f_files == 0 &&
           output.f_ffree == 0 && output.f_bsize == 4096,
           "real zero-capacity filesystem is reported without invented space");
    statistics_reply.blocks = 123;
    statistics_reply.files = 19;
    tap_ok(tap, usfs_statfs(&vfs, &output, NULL) == 0 && output.f_blocks == 123 &&
           output.f_bfree == 0 && output.f_bavail == 0 && output.f_ffree == 0,
           "full filesystem retains real totals and zero available capacity");
    statistics_reply.bfree = 12;
    statistics_reply.bavail = 10;
    statistics_reply.ffree = 7;
    tap_ok(tap, usfs_statfs(&vfs, &output, NULL) == 0 && output.f_bfree == 12 &&
           output.f_bavail == 10 && output.f_ffree == 7 && output.f_vfsnumber == 17 &&
           output.f_vfstype == 37 && request_balance == 0,
           "successful STATFS publishes only validated backend statistics and mount identity");
}

int main(void)
{
    struct tap_state tap;

    tap_plan(&tap, 26);
    usfs_fake_kernel_reset();
    test_lock_occurrences(&tap);
    test_mount_text(&tap);
    test_type_and_attributes(&tap);
    test_statistics(&tap);
    tap_ok(&tap, usfs_fake_kernel_clean(),
           "common helpers leave fake ownership balanced");
    return tap_finish(&tap);
}
