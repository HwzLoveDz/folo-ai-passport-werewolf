#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "werewolf_nickname.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads a stable local nickname. A valid NVS record wins; an explicit newer
 * factory seed may update that single record. If NVS is unavailable, a
 * deterministic MOTE-XXXX fallback is returned without erasing any partition. */
bool werewolf_identity_load(werewolf_nickname_t nickname,
                            const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
