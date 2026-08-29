#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starting is fail-closed: every occupied seat must have an echoed profile and
 * be READY.  The controller separately guarantees six encrypted client links. */
bool werewolf_lobby_can_start(uint8_t occupied_mask,
                              uint8_t profile_mask,
                              uint8_t ready_mask);

#ifdef __cplusplus
}
#endif
