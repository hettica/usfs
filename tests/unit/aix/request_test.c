/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "support/fake_kernel.h"

#include <string.h>

static struct usfs_request_allocation_spec make_request_spec(uint16_t opcode,
                                                  uint64_t node_id)
{
    struct usfs_request_allocation_spec spec = { 0 };

    spec.opcode = opcode;
    spec.node_id = node_id;
    return spec;
}

static void expect_allocation_failure(struct tap_state *tap,
                                      enum usfs_allocation_site site,
                                      const char *name)
{
    struct usfs_request *request = (struct usfs_request *)1;
    struct usfs_request_allocation_spec spec = make_request_spec(USFS_OP_GETATTR, 2);

    spec.max_allowed_reply_buffer_size = 8;

    usfs_fake_kernel_reset();
    usfs_fake_kernel.allocation_fail_site = site;
    tap_ok(tap, allocate_request(&spec, &request) == ENOMEM &&
                 request == NULL && usfs_fake_kernel_clean(), name);
}

int main(void)
{
    struct tap_state tap;
    struct usfs_request *request = NULL;
    struct ucred credential;
    struct usfs_write_in body;
    struct usfs_in_hdr *header;
    unsigned char payload[3] = { 1, 2, 3 };
    char unterminated[USFS_MAX_NAME];
    const char entry_name[] = "entry";
    char *cursor;
    struct usfs_request_allocation_spec spec;

    tap_plan(&tap, 18);
    usfs_fake_kernel_reset();
    spec = make_request_spec(USFS_OP_GETATTR, 2);
    tap_ok(&tap, allocate_request(&spec, NULL) == EINVAL,
           "request allocation requires an output pointer");
    request = (struct usfs_request *)1;
    tap_ok(&tap, allocate_request(NULL, &request) == EINVAL && request == NULL,
           "request allocation requires a specification");
    spec = make_request_spec(0, 2);
    tap_ok(&tap, allocate_request(&spec, &request) == EINVAL && request == NULL,
           "request allocation rejects invalid opcode");
    spec = make_request_spec(USFS_OP_GETATTR, 0);
    tap_ok(&tap, allocate_request(&spec, &request) == EINVAL,
           "request allocation rejects zero node id");
    spec = make_request_spec(USFS_OP_GETATTR, 2);
    spec.opcode_specific_body.length = 1;
    tap_ok(&tap, allocate_request(&spec, &request) == EINVAL,
           "request allocation rejects missing body storage");
    memset(unterminated, 'x', sizeof(unterminated));
    spec = make_request_spec(USFS_OP_LOOKUP, 2);
    spec.opcode_specific_argument = unterminated;
    tap_ok(&tap, allocate_request(&spec, &request) == ENAMETOOLONG,
           "request allocation rejects unterminated name");
    spec = make_request_spec(USFS_OP_GETATTR, 2);
    spec.opcode_specific_body.bytes = payload;
    spec.opcode_specific_body.length = USFS_MSG_MAX;
    tap_ok(&tap, allocate_request(&spec, &request) == EINVAL,
           "request allocation rejects oversized total message");

    expect_allocation_failure(&tap, USFS_ALLOC_REQUEST,
                              "request structure allocation failure is clean");
    expect_allocation_failure(
        &tap, USFS_ALLOC_REQUEST_MESSAGE,
        "request message allocation failure releases the structure");
    expect_allocation_failure(
        &tap, USFS_ALLOC_REQUEST_REPLY,
        "request reply allocation failure releases partial ownership");

    usfs_fake_kernel_reset();
    memset(&credential, 0, sizeof(credential));
    credential.cr_uid = 123;
    credential.cr_gid = 456;
    memset(&body, 0, sizeof(body));
    body.fh = 99;
    body.offset = 17;
    body.size = sizeof(payload);
    spec = make_request_spec(USFS_OP_WRITE, 7);
    spec.opcode_specific_body.bytes = &body;
    spec.opcode_specific_body.length = sizeof(body);
    spec.opcode_specific_argument = entry_name;
    spec.opcode_specific_payload.bytes = payload;
    spec.opcode_specific_payload.length = sizeof(payload);
    spec.max_allowed_reply_buffer_size = 16;
    spec.credentials = &credential;
    tap_ok(&tap, allocate_request(&spec, &request) == 0 &&
                 request != NULL,
           "valid request allocation succeeds");
    header = (struct usfs_in_hdr *)request->request_buffer;
    tap_ok(&tap, header->version == USFS_PROTOCOL_VERSION &&
                 header->opcode == USFS_OP_WRITE && header->nodeid == 7 &&
                 header->uid == 123 && header->gid == 456 &&
                 header->pid == (uint32_t)getpid() &&
                 header->len == request->request_buffer_size,
           "request header is marshalled exactly");
    cursor = request->request_buffer + sizeof(*header);
    tap_ok(&tap, memcmp(cursor, &body, sizeof(body)) == 0,
           "request body is copied exactly");
    cursor += sizeof(body);
    tap_ok(&tap, memcmp(cursor, entry_name, sizeof(entry_name)) == 0,
           "request name includes its terminator");
    cursor += sizeof(entry_name);
    tap_ok(&tap, memcmp(cursor, payload, sizeof(payload)) == 0 &&
                 usfs_msg_data(request, sizeof(body), sizeof(entry_name)) ==
                     cursor,
           "request payload and payload accessor agree");
    tap_ok(&tap, request->reply_buffer != NULL && request->max_allowed_reply_buffer_size == 16 &&
                 request->wait == NULL,
           "reply capacity and wait ownership are initialized");
    free_request(request);
    tap_ok(&tap, usfs_fake_kernel_clean(),
           "free releases complete request ownership");
    free_request(NULL);
    tap_ok(&tap, usfs_fake_kernel_clean(),
           "free accepts a null request");
    return tap_finish(&tap);
}
