#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "werewolf_messages.h"
#include "werewolf_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Discovery is fed by unauthenticated broadcast Beacons.  Keep its storage
 * fixed and small so a noisy room cannot allocate memory or grow controller
 * work without bound. */
#define WEREWOLF_ROOM_DIRECTORY_CAPACITY 8U
#define WEREWOLF_ROOM_DIRECTORY_STALE_MS UINT32_C(2000)
#define WEREWOLF_ROOM_TOKEN_NONE         UINT32_C(0)

typedef struct {
    bool used;
    uint32_t token;
    uint32_t last_seen_ms;
    uint8_t host_mac[WEREWOLF_NET_MAC_SIZE];
    uint64_t session_id;
    uint32_t epoch;
    werewolf_beacon_message_t beacon;
} werewolf_room_candidate_t;

typedef struct {
    werewolf_room_candidate_t entries[WEREWOLF_ROOM_DIRECTORY_CAPACITY];
    size_t count;
    uint32_t next_token;
} werewolf_room_directory_t;

typedef enum {
    WEREWOLF_ROOM_DIRECTORY_INVALID = -1,
    WEREWOLF_ROOM_DIRECTORY_FULL = -2,
    /* The same room sent an identical Beacon. Only last_seen_ms changed. */
    WEREWOLF_ROOM_DIRECTORY_REFRESHED = 0,
    WEREWOLF_ROOM_DIRECTORY_ADDED = 1,
    /* A room retained its token while Beacon-visible/pairing fields changed. */
    WEREWOLF_ROOM_DIRECTORY_UPDATED = 2,
    /* The same host MAC announced a new session or epoch; its token changed. */
    WEREWOLF_ROOM_DIRECTORY_REPLACED = 3,
    /* A full directory evicted its least-recently-seen unselected room. */
    WEREWOLF_ROOM_DIRECTORY_EVICTED = 4,
} werewolf_room_directory_result_t;

void werewolf_room_directory_init(werewolf_room_directory_t *directory);
void werewolf_room_directory_clear(werewolf_room_directory_t *directory);
size_t werewolf_room_directory_count(
    const werewolf_room_directory_t *directory);

/* Observe one already-decoded Beacon. Identity is host MAC + session + epoch.
 * selected_token may be WEREWOLF_ROOM_TOKEN_NONE. It protects only against
 * capacity eviction; an actually stale selected room is still expired.
 * out_token receives the stable token of the added/updated room. */
werewolf_room_directory_result_t werewolf_room_directory_observe(
    werewolf_room_directory_t *directory,
    const uint8_t host_mac[WEREWOLF_NET_MAC_SIZE],
    uint64_t session_id,
    uint32_t epoch,
    const werewolf_beacon_message_t *beacon,
    uint32_t now_ms,
    uint32_t selected_token,
    uint32_t *out_token);

/* Unsigned subtraction makes both freshness checks safe across uint32 wrap.
 * A room is stale at exactly WEREWOLF_ROOM_DIRECTORY_STALE_MS. */
bool werewolf_room_candidate_is_fresh(
    const werewolf_room_candidate_t *candidate, uint32_t now_ms);
size_t werewolf_room_directory_expire(werewolf_room_directory_t *directory,
                                      uint32_t now_ms);

bool werewolf_room_directory_find(
    const werewolf_room_directory_t *directory,
    uint32_t token,
    werewolf_room_candidate_t *candidate);
bool werewolf_room_directory_find_fresh(
    const werewolf_room_directory_t *directory,
    uint32_t token,
    uint32_t now_ms,
    werewolf_room_candidate_t *candidate);

/* Copy candidates in deterministic fingerprint-then-MAC order. Storage slots
 * and tokens never move, so UI selection must bind to token, not list index. */
size_t werewolf_room_directory_snapshot_sorted(
    const werewolf_room_directory_t *directory,
    werewolf_room_candidate_t *candidates,
    size_t capacity);

#ifdef __cplusplus
}
#endif
