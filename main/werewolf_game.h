#ifndef WEREWOLF_GAME_H
#define WEREWOLF_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WW_PLAYER_COUNT 7u
#define WW_ALL_PLAYERS_MASK UINT8_C(0x7f)
#define WW_NO_PLAYER UINT8_MAX

typedef uint8_t ww_player_mask_t;

typedef enum {
    WW_OK = 0,
    WW_ERR_NULL = -1,
    WW_ERR_UNINITIALIZED = -2,
    WW_ERR_INVALID_PLAYER = -3,
    WW_ERR_INVALID_TARGET = -4,
    WW_ERR_INVALID_PHASE = -5,
    WW_ERR_STALE_ACTION = -6,
    WW_ERR_NOT_JOINED = -7,
    WW_ERR_ALREADY_JOINED = -8,
    WW_ERR_NOT_READY = -9,
    WW_ERR_ALREADY_SUBMITTED = -10,
    WW_ERR_PLAYER_DEAD = -11,
    WW_ERR_TARGET_DEAD = -12,
    WW_ERR_SELF_TARGET = -13,
    WW_ERR_WOLF_FRIENDLY_FIRE = -14,
    WW_ERR_GUARD_REPEAT = -15,
    WW_ERR_TARGET_NOT_TIED = -16,
    WW_ERR_GAME_NOT_OVER = -17,
    WW_ERR_BAD_STATE = -18,
    WW_ERR_NOT_CURRENT_SPEAKER = -19,
} ww_status_t;

typedef enum {
    WW_ROLE_NONE = 0,
    WW_ROLE_WOLF,
    WW_ROLE_SEER,
    WW_ROLE_GUARD,
    WW_ROLE_VILLAGER,
} ww_role_t;

typedef enum {
    WW_CAMP_UNKNOWN = 0,
    WW_CAMP_GOOD,
    WW_CAMP_WOLF,
} ww_camp_t;

typedef enum {
    WW_WINNER_NONE = 0,
    WW_WINNER_GOOD,
    WW_WINNER_WOLVES,
} ww_winner_t;

typedef enum {
    WW_PHASE_LOBBY = 0,
    WW_PHASE_NIGHT,
    WW_PHASE_WOLF_REVOTE,
    WW_PHASE_DAWN_RESULT,
    WW_PHASE_DISCUSSION,
    WW_PHASE_VOTE,
    WW_PHASE_TIE_DEFENSE,
    WW_PHASE_REVOTE,
    WW_PHASE_EXILE_RESULT,
    WW_PHASE_GAME_OVER,
} ww_phase_t;

/*
 * Host-authoritative, fixed-capacity game state. It contains all private data
 * and must never be copied to a client or written to release logs. Clients
 * receive only ww_public_view_t plus their own ww_private_view_t.
 *
 * The struct is pointer-free so the authority can place it in static storage
 * and checkpoint it without dynamic allocation. Checkpoint serialization and
 * integrity/versioning belong to the transport/persistence layer.
 */
typedef struct {
    uint32_t magic;
    uint32_t phase_epoch;
    uint64_t shuffle_seed;
    ww_phase_t phase;
    ww_winner_t winner;
    ww_player_mask_t joined_mask;
    ww_player_mask_t alive_mask;
    ww_player_mask_t submitted_mask;
    ww_player_mask_t tie_mask;
    ww_player_mask_t discussion_remaining_mask;
    ww_player_mask_t seer_checked_mask;
    ww_player_mask_t seer_wolf_mask;
    ww_role_t roles[WW_PLAYER_COUNT];
    uint8_t action_targets[WW_PLAYER_COUNT];
    uint8_t vote_targets[WW_PLAYER_COUNT];
    uint16_t round_number;
    uint8_t guard_previous_target;
    uint8_t pending_guard_target;
    uint8_t current_speaker;
    uint8_t dawn_victim;
    uint8_t exiled_player;
} ww_game_t;

typedef struct {
    ww_phase_t phase;
    ww_winner_t winner;
    uint32_t phase_epoch;
    uint16_t round_number;
    ww_player_mask_t joined_mask;
    ww_player_mask_t alive_mask;
    ww_player_mask_t submitted_mask;
    ww_player_mask_t tie_mask;
    ww_player_mask_t discussion_remaining_mask;
    uint8_t joined_count;
    uint8_t alive_count;
    uint8_t submitted_count;
    uint8_t current_speaker;
    uint8_t dawn_victim;
    uint8_t exiled_player;
} ww_public_view_t;

typedef struct {
    uint8_t player;
    bool joined;
    bool alive;
    ww_role_t role;
    ww_player_mask_t wolf_teammates_mask;
    ww_player_mask_t seer_checked_mask;
    ww_player_mask_t seer_wolf_mask;
    uint8_t guard_previous_target;
    bool action_submitted;
    uint8_t submitted_target;
} ww_private_view_t;

/* Lobby and deterministic start. */
ww_status_t ww_game_init(ww_game_t *game);
ww_status_t ww_game_join(ww_game_t *game, uint8_t player);
ww_status_t ww_game_leave_lobby(ww_game_t *game, uint8_t player);
ww_status_t ww_game_start(ww_game_t *game, uint64_t shuffle_seed);

/* Safe views. Role reveal is unavailable until WW_PHASE_GAME_OVER. */
ww_status_t ww_game_get_public_view(const ww_game_t *game,
                                    ww_public_view_t *out_view);
ww_status_t ww_game_get_private_view(const ww_game_t *game, uint8_t player,
                                     ww_private_view_t *out_view);
ww_status_t ww_game_get_role_reveal(const ww_game_t *game,
                                    ww_role_t out_roles[WW_PLAYER_COUNT]);

/*
 * Night collection. Every living player submits in both night phases. In
 * WW_PHASE_WOLF_REVOTE only Wolf targets affect resolution; all other targets
 * are privacy-preserving dummy actions. Finalize moves to WOLF_REVOTE when the
 * first Wolf choices differ, otherwise to DAWN_RESULT or GAME_OVER.
 */
ww_status_t ww_game_submit_night_action(ww_game_t *game,
                                        uint32_t expected_epoch,
                                        uint8_t player, uint8_t target);
ww_status_t ww_game_finalize_night(ww_game_t *game,
                                   uint32_t expected_epoch);

/*
 * Public day transitions. Speaker passing is valid during the normal
 * discussion and during tie defence. The authority selects speakers in
 * ascending seat order; only the current living speaker may pass.
 */
ww_status_t ww_game_begin_discussion(ww_game_t *game,
                                     uint32_t expected_epoch);
ww_status_t ww_game_pass_speaker(ww_game_t *game, uint32_t expected_epoch,
                                 uint8_t player);
ww_status_t ww_game_begin_vote(ww_game_t *game, uint32_t expected_epoch);

/*
 * Secret vote collection. The first vote can target any other living player.
 * A revote target must be in tie_mask. Finalize moves to TIE_DEFENSE, an exile
 * result, or GAME_OVER. A first-place tie starts a deterministic defence in
 * ascending tied-seat order. begin_revote is unavailable until every tied
 * speaker has passed. A tied revote records WW_NO_PLAYER as the exile.
 */
ww_status_t ww_game_submit_vote(ww_game_t *game, uint32_t expected_epoch,
                                uint8_t player, uint8_t target);
ww_status_t ww_game_finalize_vote(ww_game_t *game,
                                  uint32_t expected_epoch);
ww_status_t ww_game_begin_revote(ww_game_t *game,
                                 uint32_t expected_epoch);
ww_status_t ww_game_begin_next_night(ww_game_t *game,
                                     uint32_t expected_epoch);

ww_camp_t ww_role_camp(ww_role_t role);
const char *ww_status_string(ww_status_t status);
const char *ww_phase_string(ww_phase_t phase);
const char *ww_role_string(ww_role_t role);

#ifdef __cplusplus
}
#endif

#endif
