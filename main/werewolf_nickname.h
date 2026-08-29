#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bundled Kode Mono fonts cover printable ASCII.  Nicknames therefore use
 * ten visible ASCII characters plus one trailing NUL throughout storage,
 * protocol and UI code.  Seat IDs remain the authoritative player identity. */
#define WEREWOLF_NICKNAME_MAX_CHARS 10U
#define WEREWOLF_NICKNAME_CAPACITY  (WEREWOLF_NICKNAME_MAX_CHARS + 1U)

typedef char werewolf_nickname_t[WEREWOLF_NICKNAME_CAPACITY];

/* Copies at most ten characters. Overlong input is deliberately truncated.
 * Control/non-ASCII bytes, leading/trailing spaces and an empty result are
 * rejected so the same nickname has one canonical wire representation. */
bool werewolf_nickname_normalize(werewolf_nickname_t output,
                                 const char *input);
bool werewolf_nickname_valid(const char *nickname);

#ifdef __cplusplus
}
#endif
