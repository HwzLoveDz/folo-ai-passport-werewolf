#include "werewolf_identity_record.h"

#include <string.h>

#define IDENTITY_SCHEMA 1U

static uint32_t get_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) |
           ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) |
           (uint32_t)source[3];
}

static void put_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

bool werewolf_identity_record_encode(
    uint8_t record[WEREWOLF_IDENTITY_RECORD_SIZE],
    const char *nickname, uint32_t revision)
{
    size_t length;

    if (record == NULL || revision == 0U ||
        !werewolf_nickname_valid(nickname)) {
        return false;
    }
    length = strlen(nickname);
    memset(record, 0, WEREWOLF_IDENTITY_RECORD_SIZE);
    record[0] = (uint8_t)'W';
    record[1] = (uint8_t)'N';
    record[2] = IDENTITY_SCHEMA;
    record[3] = (uint8_t)length;
    put_u32_be(&record[4], revision);
    memcpy(&record[8], nickname, length);
    return true;
}

bool werewolf_identity_record_decode(
    const uint8_t *record, size_t record_size,
    werewolf_nickname_t nickname, uint32_t *revision)
{
    uint8_t length;

    if (record == NULL || nickname == NULL || revision == NULL ||
        record_size != WEREWOLF_IDENTITY_RECORD_SIZE ||
        record[0] != (uint8_t)'W' || record[1] != (uint8_t)'N' ||
        record[2] != IDENTITY_SCHEMA) {
        return false;
    }
    length = record[3];
    if (length == 0U || length > WEREWOLF_NICKNAME_MAX_CHARS) {
        return false;
    }
    for (size_t index = length;
         index < WEREWOLF_NICKNAME_MAX_CHARS; ++index) {
        if (record[8U + index] != 0U) {
            return false;
        }
    }
    memset(nickname, 0, WEREWOLF_NICKNAME_CAPACITY);
    memcpy(nickname, &record[8], length);
    *revision = get_u32_be(&record[4]);
    if (*revision == 0U || !werewolf_nickname_valid(nickname)) {
        memset(nickname, 0, WEREWOLF_NICKNAME_CAPACITY);
        *revision = 0U;
        return false;
    }
    return true;
}
