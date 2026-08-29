#include "png_writer.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8U; bit++) {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t adler32(const uint8_t *data, size_t size)
{
    uint32_t a = 1U;
    uint32_t b = 0U;
    for (size_t i = 0; i < size; i++) {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16U) | a;
}

static void put_be32(uint8_t output[4], uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static int write_chunk(FILE *file, const char type[4],
                       const uint8_t *data, uint32_t size)
{
    uint8_t word[4];
    put_be32(word, size);
    if (fwrite(word, 1U, sizeof(word), file) != sizeof(word) ||
        fwrite(type, 1U, 4U, file) != 4U ||
        (size > 0U && fwrite(data, 1U, size, file) != size)) {
        return -1;
    }

    uint32_t crc = crc32_update(0U, (const uint8_t *)type, 4U);
    if (size > 0U) {
        // Continue the PNG CRC without allocating a combined type/data block.
        crc = ~crc;
        for (uint32_t i = 0; i < size; i++) {
            crc ^= data[i];
            for (unsigned bit = 0; bit < 8U; bit++) {
                uint32_t mask = 0U - (crc & 1U);
                crc = (crc >> 1U) ^ (0xEDB88320U & mask);
            }
        }
        crc = ~crc;
    }
    put_be32(word, crc);
    return fwrite(word, 1U, sizeof(word), file) == sizeof(word) ? 0 : -1;
}

int png_write_rgb565(const char *path, const uint16_t *pixels,
                     unsigned width, unsigned height)
{
    if (!path || !pixels || width == 0U || height == 0U) return -1;

    const size_t row_size = 1U + (size_t)width * 3U;
    const size_t raw_size = row_size * height;
    uint8_t *raw = malloc(raw_size);
    if (!raw) return -1;

    for (unsigned y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * row_size;
        row[0] = 0U;
        for (unsigned x = 0; x < width; x++) {
            uint16_t pixel = pixels[(size_t)y * width + x];
            uint8_t r5 = (uint8_t)((pixel >> 11U) & 0x1FU);
            uint8_t g6 = (uint8_t)((pixel >> 5U) & 0x3FU);
            uint8_t b5 = (uint8_t)(pixel & 0x1FU);
            row[1U + x * 3U] = (uint8_t)((r5 << 3U) | (r5 >> 2U));
            row[2U + x * 3U] = (uint8_t)((g6 << 2U) | (g6 >> 4U));
            row[3U + x * 3U] = (uint8_t)((b5 << 3U) | (b5 >> 2U));
        }
    }

    const size_t block_count = (raw_size + 65534U) / 65535U;
    const size_t zlib_size = 2U + raw_size + block_count * 5U + 4U;
    uint8_t *zlib = malloc(zlib_size);
    if (!zlib) {
        free(raw);
        return -1;
    }

    size_t output = 0U;
    zlib[output++] = 0x78U;
    zlib[output++] = 0x01U;
    size_t input = 0U;
    while (input < raw_size) {
        size_t remaining = raw_size - input;
        uint16_t length = (uint16_t)(remaining > 65535U ? 65535U : remaining);
        bool final = input + length == raw_size;
        zlib[output++] = final ? 0x01U : 0x00U;
        zlib[output++] = (uint8_t)length;
        zlib[output++] = (uint8_t)(length >> 8U);
        uint16_t inverse = (uint16_t)~length;
        zlib[output++] = (uint8_t)inverse;
        zlib[output++] = (uint8_t)(inverse >> 8U);
        memcpy(zlib + output, raw + input, length);
        output += length;
        input += length;
    }
    uint8_t word[4];
    put_be32(word, adler32(raw, raw_size));
    memcpy(zlib + output, word, sizeof(word));
    output += sizeof(word);

    FILE *file = fopen(path, "wb");
    int result = -1;
    if (file) {
        static const uint8_t signature[8] = {
            0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU,
        };
        uint8_t header[13] = {0};
        put_be32(header, width);
        put_be32(header + 4U, height);
        header[8] = 8U;
        header[9] = 2U;
        if (fwrite(signature, 1U, sizeof(signature), file) == sizeof(signature) &&
            write_chunk(file, "IHDR", header, sizeof(header)) == 0 &&
            write_chunk(file, "IDAT", zlib, (uint32_t)output) == 0 &&
            write_chunk(file, "IEND", NULL, 0U) == 0) {
            result = 0;
        }
        if (fclose(file) != 0) result = -1;
    }

    free(zlib);
    free(raw);
    return result;
}
