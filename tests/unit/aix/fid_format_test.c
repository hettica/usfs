/*
 * Copyright (c) 2026 Raman Dzehtsiar
 * SPDX-License-Identifier: MIT
 */

#include "tap.h"
#include "definitions.h"
#include "fs/fid_format.h"

static void test_file_identifier_round_trip (struct tap_state * state)
{
    static const uint64_t backend_token = UINT64_C (0xabcdef0123456789);
    static const uint32_t mount_serial = UINT32_C (0x12345678);
    struct fileid file_id;
    uint64_t decoded_token = 0;

    memset (&file_id, 0xa5, sizeof (file_id));
    encode_file_identifier (&file_id, mount_serial, backend_token);

    tap_ok (state, file_id.fid_len == USFS_FID_PAYLOAD_LENGTH, "fid declares its complete encoded payload");
    tap_ok (
        state,
        decode_file_identifier (&file_id, mount_serial, &decoded_token) == 0 && decoded_token == backend_token,
        "fid retains all 64 backend-token bits"
    );

    static const unsigned char expected_token_high[] = { 0xab, 0xcd, 0xef, 0x01 };
    static const unsigned char expected_token_low[] = { 0x23, 0x45, 0x67, 0x89 };
    static const unsigned char expected_mount_and_version[] = { 0x12, 0x34, 0x56, 0x78, 0x00, 0x01 };

    tap_ok (
        state,
        memcmp (&file_id.fid_ino, expected_token_high, sizeof (expected_token_high)) == 0 &&
            memcmp (&file_id.fid_gen, expected_token_low, sizeof (expected_token_low)) == 0 &&
            memcmp (file_id.fid_x, expected_mount_and_version, sizeof (expected_mount_and_version)) == 0,
        "fid fields use explicit big-endian bytes"
    );
    tap_ok (
        state,
        file_id.fid_x[USFS_FID_EXTRA_OFFSET] == 0 && file_id.fid_x[USFS_FID_EXTRA_OFFSET + 1] == 0,
        "unused named fileid bytes are cleared"
    );

    const unsigned char * bytes = (const unsigned char *)&file_id;
    int trailing_padding_is_zero = true;

    for (size_t index = sizeof (file_id) - 2u; index < sizeof (file_id); index++)
    {
        if (bytes[index] != 0)
            trailing_padding_is_zero = false;
    }

    tap_ok (state, trailing_padding_is_zero, "fileid ABI padding is cleared");
}

static void test_file_identifier_rejection (struct tap_state * state)
{
    static const uint32_t mount_serial = UINT32_C (0x12345678);
    struct fileid file_id = { 0 };
    uint64_t token = 0;

    encode_file_identifier (&file_id, mount_serial, UINT64_C (0x100000001));
    tap_ok (state, decode_file_identifier (&file_id, mount_serial + 1u, &token) == ESTALE, "fid from another mount incarnation is stale");

    file_id.fid_len--;
    tap_ok (state, decode_file_identifier (&file_id, mount_serial, &token) == EINVAL, "truncated fid payload is rejected");

    encode_file_identifier (&file_id, mount_serial, 7);
    file_id.fid_x[USFS_FID_VERSION_OFFSET]++;
    tap_ok (state, decode_file_identifier (&file_id, mount_serial, &token) == EINVAL, "unknown fid format is rejected");

    tap_ok (
        state,
        decode_file_identifier (&file_id, mount_serial + 1u, &token) == EINVAL,
        "malformed format takes precedence over a stale mount serial"
    );

    encode_file_identifier (&file_id, mount_serial, 7);
    file_id.fid_x[USFS_FID_RESERVED_OFFSET] = 1;
    tap_ok (state, decode_file_identifier (&file_id, mount_serial, &token) == EINVAL, "nonzero reserved fid byte is rejected");

    encode_file_identifier (&file_id, mount_serial, 7);
    file_id.fid_x[USFS_FID_EXTRA_OFFSET] = 1;
    tap_ok (
        state,
        decode_file_identifier (&file_id, mount_serial, &token) == 0 && token == 7,
        "decoder ignores bytes beyond the declared fid payload"
    );

    encode_file_identifier (&file_id, mount_serial, 0);
    tap_ok (state, decode_file_identifier (&file_id, mount_serial, &token) == EINVAL, "zero backend token is rejected");
    tap_ok (state, decode_file_identifier (NULL, mount_serial, &token) == EINVAL, "missing fid input is rejected");
    tap_ok (state, decode_file_identifier (&file_id, mount_serial, NULL) == EINVAL, "missing token output is rejected");
}

int main (void)
{
    struct tap_state state;

    tap_plan (&state, 14);
    test_file_identifier_round_trip (&state);
    test_file_identifier_rejection (&state);

    return tap_finish (&state);
}
