#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "werewolf_nickname.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEREWOLF_IDENTITY_RECORD_SIZE 18U

bool werewolf_identity_record_encode(
    uint8_t record[WEREWOLF_IDENTITY_RECORD_SIZE],
    const char *nickname, uint32_t revision);
bool werewolf_identity_record_decode(
    const uint8_t *record, size_t record_size,
    werewolf_nickname_t nickname, uint32_t *revision);

#ifdef __cplusplus
}
#endif
