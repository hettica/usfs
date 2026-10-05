// Copyright (c) 2026 Raman Dzehtsiar
// SPDX-License-Identifier: MIT

#ifndef USFS_FS_FID_FORMAT_H
#define USFS_FS_FID_FORMAT_H

/* AIX's default fileid has 18 named bytes after fid_len. The first 16
 * carry USFS's mount-scoped identity; all remaining bytes are zero. */
enum usfs_fid_format
{
    USFS_FID_FORMAT_VERSION = 1,
    USFS_FID_PAYLOAD_LENGTH = 16,
    USFS_FID_BITS_PER_BYTE = 8,
    USFS_FID_MOUNT_SERIAL_OFFSET = 0,
    USFS_FID_VERSION_OFFSET = 4,
    USFS_FID_RESERVED_OFFSET = 6,
    USFS_FID_EXTRA_OFFSET = 8
};

typedef char usfs_fid_inode_width_check[(sizeof (((struct fileid *)0)->fid_ino) == sizeof (uint32_t)) ? 1 : -1];
typedef char usfs_fid_generation_width_check[(sizeof (((struct fileid *)0)->fid_gen) == sizeof (uint32_t)) ? 1 : -1];
typedef char usfs_fid_extension_width_check[(sizeof (((struct fileid *)0)->fid_x) >= 10u) ? 1 : -1];

static void encode_fid_integer (unsigned char * bytes, const size_t byte_count, uint64_t value)
{
    for (size_t byte_index = byte_count; byte_index != 0; byte_index--)
    {
        bytes[byte_index - 1u] = (unsigned char)value;
        value >>= USFS_FID_BITS_PER_BYTE;
    }
}

static uint64_t decode_fid_integer (const unsigned char * bytes, const size_t byte_count)
{
    uint64_t value = 0;

    for (size_t byte_index = 0; byte_index < byte_count; byte_index++)
        value = (value << USFS_FID_BITS_PER_BYTE) | bytes[byte_index];

    return value;
}

static void encode_file_identifier (struct fileid * file_id, const uint32_t mount_serial, const uint64_t token)
{
    memset (file_id, 0, sizeof (*file_id));
    file_id->fid_len = USFS_FID_PAYLOAD_LENGTH;
    encode_fid_integer ((unsigned char *)&file_id->fid_ino, sizeof (file_id->fid_ino), token >> (sizeof (file_id->fid_gen) * USFS_FID_BITS_PER_BYTE));
    encode_fid_integer ((unsigned char *)&file_id->fid_gen, sizeof (file_id->fid_gen), token);
    encode_fid_integer ((unsigned char *)file_id->fid_x + USFS_FID_MOUNT_SERIAL_OFFSET, sizeof (mount_serial), mount_serial);
    encode_fid_integer ((unsigned char *)file_id->fid_x + USFS_FID_VERSION_OFFSET, sizeof (uint16_t), USFS_FID_FORMAT_VERSION);
}

static int decode_file_identifier (const struct fileid * file_id, const uint32_t mount_serial, uint64_t * token)
{
    if (file_id == NULL || token == NULL)
        return EINVAL;

    if (file_id->fid_len != USFS_FID_PAYLOAD_LENGTH)
        return EINVAL;

    const uint32_t encoded_mount_serial = (uint32_t)decode_fid_integer (
        (const unsigned char *)file_id->fid_x + USFS_FID_MOUNT_SERIAL_OFFSET,
        sizeof (encoded_mount_serial)
    );

    const uint16_t format_version = (uint16_t)decode_fid_integer (
        (const unsigned char *)file_id->fid_x + USFS_FID_VERSION_OFFSET,
        sizeof (format_version)
    );

    if (format_version != USFS_FID_FORMAT_VERSION)
        return EINVAL;

    for (size_t index = USFS_FID_RESERVED_OFFSET; index < USFS_FID_EXTRA_OFFSET; index++)
    {
        if (file_id->fid_x[index] != 0)
            return EINVAL;
    }

    const uint64_t token_high = decode_fid_integer ((const unsigned char *)&file_id->fid_ino, sizeof (file_id->fid_ino));
    const uint64_t token_low = decode_fid_integer ((const unsigned char *)&file_id->fid_gen, sizeof (file_id->fid_gen));
    const uint64_t decoded_token = (token_high << (sizeof (file_id->fid_gen) * USFS_FID_BITS_PER_BYTE)) | token_low;

    if (decoded_token == 0)
        return EINVAL;

    if (encoded_mount_serial != mount_serial)
        return ESTALE;

    *token = decoded_token;

    return 0;
}

#endif
