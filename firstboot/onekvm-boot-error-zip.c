// SPDX-License-Identifier: MIT
/* Pack stored-only ZIP members. Entry names are the file basenames. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BOOT_ERROR_ZIP_MAX_FILES = 32, BOOT_ERROR_ZIP_MAX_BYTES = 1024 * 1024 };

struct zip_item {
    const char *name;
    uint8_t *data;
    uint32_t size;
    uint32_t crc;
    uint32_t offset;
};

static uint32_t crc32_bytes(const uint8_t *data, uint32_t size)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return ~crc;
}

static int write_u16(FILE *file, unsigned value)
{
    unsigned char bytes[2] = { (unsigned char)value, (unsigned char)(value >> 8) };
    return fwrite(bytes, 1, 2, file) == 2 ? 0 : -1;
}

static int write_u32(FILE *file, uint32_t value)
{
    unsigned char bytes[4] = {
        (unsigned char)value, (unsigned char)(value >> 8),
        (unsigned char)(value >> 16), (unsigned char)(value >> 24),
    };
    return fwrite(bytes, 1, 4, file) == 4 ? 0 : -1;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static int read_member(const char *path, struct zip_item *item)
{
    FILE *file = fopen(path, "rb");
    long length;
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (unsigned long)length > BOOT_ERROR_ZIP_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        errno = EFBIG;
        return -1;
    }
    item->name = base_name(path);
    item->size = (uint32_t)length;
    item->data = calloc(item->size ? item->size : 1, 1);
    if (!item->data || (item->size && fread(item->data, 1, item->size, file) != item->size)) {
        free(item->data);
        item->data = NULL;
        fclose(file);
        return -1;
    }
    fclose(file);
    if (!item->name[0] || strchr(item->name, '\\')) {
        free(item->data);
        errno = EINVAL;
        return -1;
    }
    item->crc = crc32_bytes(item->data, item->size);
    return 0;
}

static int write_local(FILE *file, struct zip_item *item)
{
    long offset = ftell(file);
    uint16_t name_len = (uint16_t)strlen(item->name);
    if (offset < 0 || (unsigned long)offset > UINT32_MAX || strlen(item->name) > UINT16_MAX)
        return -1;
    item->offset = (uint32_t)offset;
    if (write_u32(file, 0x04034b50u) || write_u16(file, 20) || write_u16(file, 0x0800) ||
        write_u16(file, 0) || write_u16(file, 0) || write_u16(file, 0) ||
        write_u32(file, item->crc) || write_u32(file, item->size) || write_u32(file, item->size) ||
        write_u16(file, name_len) || write_u16(file, 0) ||
        fwrite(item->name, 1, name_len, file) != name_len ||
        (item->size && fwrite(item->data, 1, item->size, file) != item->size))
        return -1;
    return 0;
}

static int write_central(FILE *file, const struct zip_item *item)
{
    uint16_t name_len = (uint16_t)strlen(item->name);
    if (write_u32(file, 0x02014b50u) || write_u16(file, 20) || write_u16(file, 20) ||
        write_u16(file, 0x0800) || write_u16(file, 0) || write_u16(file, 0) || write_u16(file, 0) ||
        write_u32(file, item->crc) || write_u32(file, item->size) || write_u32(file, item->size) ||
        write_u16(file, name_len) || write_u16(file, 0) || write_u16(file, 0) ||
        write_u16(file, 0) || write_u16(file, 0) || write_u32(file, 0) ||
        write_u32(file, item->offset) || fwrite(item->name, 1, name_len, file) != name_len)
        return -1;
    return 0;
}

static int write_end(FILE *file, unsigned count, uint32_t central_offset, uint32_t central_size)
{
    return write_u32(file, 0x06054b50u) || write_u16(file, 0) || write_u16(file, 0) ||
        write_u16(file, count) || write_u16(file, count) ||
        write_u32(file, central_size) || write_u32(file, central_offset) || write_u16(file, 0);
}

int main(int argc, char **argv)
{
    struct zip_item items[BOOT_ERROR_ZIP_MAX_FILES];
    FILE *archive;
    long central_offset, central_end;
    int count, i, j, failed = 1;
    if (argc < 3 || argc - 2 > BOOT_ERROR_ZIP_MAX_FILES) {
        fprintf(stderr, "Usage: onekvm-boot-error-zip ARCHIVE.zip FILE...\n");
        return 2;
    }
    count = argc - 2;
    memset(items, 0, sizeof(items));
    for (i = 0; i < count; i++) {
        if (read_member(argv[i + 2], &items[i]))
            goto cleanup;
        for (j = 0; j < i; j++) {
            if (!strcmp(items[j].name, items[i].name)) {
                errno = EEXIST;
                goto cleanup;
            }
        }
    }
    archive = fopen(argv[1], "wb");
    if (!archive)
        goto cleanup;
    for (i = 0; i < count; i++) {
        if (write_local(archive, &items[i]))
            goto close_archive;
    }
    central_offset = ftell(archive);
    if (central_offset < 0)
        goto close_archive;
    for (i = 0; i < count; i++) {
        if (write_central(archive, &items[i]))
            goto close_archive;
    }
    central_end = ftell(archive);
    if (central_end < central_offset || central_end > UINT32_MAX ||
        write_end(archive, (unsigned)count, (uint32_t)central_offset,
                  (uint32_t)(central_end - central_offset)))
        goto close_archive;
    if (fclose(archive)) {
        remove(argv[1]);
        goto cleanup;
    }
    failed = 0;
    goto cleanup;
close_archive:
    fclose(archive);
    remove(argv[1]);
cleanup:
    for (i = 0; i < count; i++)
        free(items[i].data);
    if (failed)
        perror("onekvm-boot-error-zip");
    return failed ? 1 : 0;
}
