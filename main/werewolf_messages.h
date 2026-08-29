#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "werewolf_game.h"
#include "werewolf_nickname.h"
#include "werewolf_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEREWOLF_MESSAGES_RULES_VERSION         5U
#define WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE 32U
#define WEREWOLF_MESSAGES_NONCE_SIZE             16U
#define WEREWOLF_MESSAGES_COMMITMENT_SIZE        32U
#define WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE   8U
#define WEREWOLF_MESSAGES_BEACON_SIZE       44U
#define WEREWOLF_MESSAGES_JOIN_SIZE         33U
#define WEREWOLF_MESSAGES_PAIR_HOST_REVEAL_SIZE 81U
#define WEREWOLF_MESSAGES_PAIR_CLIENT_REVEAL_SIZE 81U
#define WEREWOLF_MESSAGES_ACCEPT_SIZE        1U
#define WEREWOLF_MESSAGES_READY_SIZE         1U
#define WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE 20U
#define WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE \
    (1U + WEREWOLF_NICKNAME_MAX_CHARS)
#define WEREWOLF_MESSAGES_ROSTER_SIZE \
    (4U + 1U + WW_PLAYER_COUNT * WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE)
#define WEREWOLF_MESSAGES_PROFILE_SIZE \
    WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE
#define WEREWOLF_MESSAGES_START_SIZE \
    (WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE + WEREWOLF_MESSAGES_ROSTER_SIZE)
#define WEREWOLF_MESSAGES_PRIVATE_ROLE_SIZE  9U
#define WEREWOLF_MESSAGES_PHASE_SIZE        WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE
#define WEREWOLF_MESSAGES_ACTION_SIZE       11U
#define WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE \
    (7U + WEREWOLF_MESSAGES_ROSTER_SIZE)
#define WEREWOLF_MESSAGES_SNAPSHOT_GAME_SIZE \
    (1U + WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE + 1U)
#define WEREWOLF_MESSAGES_SNAPSHOT_ROLE_REVEAL_SIZE \
    (1U + WW_PLAYER_COUNT)
#define WEREWOLF_MESSAGES_ABORT_SIZE         1U

/* Payload layouts are exact and use network byte order for every multibyte
 * integer. Arrays are byte strings, never serialized C structures.
 *
 * BEACON:      protocol u8 | rules u8 | occupied u8 | offered seat u8 |
 *              fingerprint[8] | host commitment[32]
 * JOIN:        candidate seat u8 | client commitment[32]
 * HOST_REVEAL: offered seat u8 | host X25519 public key[32] | nonce[16] |
 *              echo of the locked client commitment[32]
 * CLIENT_REVEAL: candidate seat u8 | client X25519 public key[32] | nonce[16] |
 *                echo of the host commitment[32]
 * ACCEPT:      seat u8
 * READY:       0 or 1
 * PROFILE:     nickname length u8 | nickname[10], zero padded
 * START:       public state | authoritative roster
 * PHASE:       public state (see werewolf_public_state_message_t)
 * PRIVATE:     role u8 | Wolf teammate mask u8 | Seer seat u8 | faction u8 |
 *              gate kind u8 | gate epoch u32
 * PUBLIC:      phase u8 | winner u8 | phase epoch u32 | round u16 |
 *              occupied/alive/submitted/tie masks u8 each |
 *              speaker/dawn victim/exile u8 each | gate kind u8 |
 *              gate epoch u32
 * ACTION:      kind u8 | target u8 | expected phase epoch u32 |
 *              expected gate kind u8 | expected gate epoch u32
 * GAME SNAPSHOT: kind u8 | public state | local gate acknowledged 0/1
 * SNAPSHOT:    kind u8 | lobby body, game body, or seven-role reveal
 * ABORT:       reason u8
 */

typedef enum {
    WEREWOLF_MESSAGES_OK = 0,
    WEREWOLF_MESSAGES_ERR_ARGUMENT = -1,
    WEREWOLF_MESSAGES_ERR_CAPACITY = -2,
    WEREWOLF_MESSAGES_ERR_LENGTH = -3,
    WEREWOLF_MESSAGES_ERR_TYPE = -4,
    WEREWOLF_MESSAGES_ERR_VERSION = -5,
    WEREWOLF_MESSAGES_ERR_ENUM = -6,
    WEREWOLF_MESSAGES_ERR_MASK = -7,
    WEREWOLF_MESSAGES_ERR_VALUE = -8,
} werewolf_messages_result_t;

typedef enum {
    WEREWOLF_ACTION_NIGHT_TARGET = 1,
    WEREWOLF_ACTION_PASS_SPEECH,
    WEREWOLF_ACTION_VOTE_TARGET,
    WEREWOLF_ACTION_ROLE_SEEN,
    WEREWOLF_ACTION_ACK_RESULT,
    WEREWOLF_ACTION_LEAVE_GAME,
} werewolf_action_kind_t;

/* Controller sub-phases which do not necessarily change ww_phase_t.  They are
 * carried on the wire so a client cannot mistake a heartbeat for permission
 * to advance, and so result acknowledgements are bound to one exact gate. */
typedef enum {
    WEREWOLF_GATE_NONE = 0,
    WEREWOLF_GATE_ROLE,
    WEREWOLF_GATE_PRIVATE_RESULT,
    WEREWOLF_GATE_DAWN,
    WEREWOLF_GATE_EXILE,
} werewolf_gate_kind_t;

typedef enum {
    WEREWOLF_SNAPSHOT_LOBBY = 1,
    WEREWOLF_SNAPSHOT_GAME,
    WEREWOLF_SNAPSHOT_ROLE_REVEAL,
} werewolf_snapshot_kind_t;

typedef enum {
    WEREWOLF_ABORT_HOST_LOST = 1,
    WEREWOLF_ABORT_INCOMPATIBLE_VERSION,
    WEREWOLF_ABORT_SECURITY_FAILURE,
    WEREWOLF_ABORT_PROTOCOL_ERROR,
    WEREWOLF_ABORT_INTERNAL_ERROR,
    WEREWOLF_ABORT_USER_CANCELLED,
    WEREWOLF_ABORT_HOST_CLOSED_ROOM,
    WEREWOLF_ABORT_KICKED_BY_HOST,
} werewolf_abort_reason_t;

typedef struct {
    uint8_t protocol_version;
    uint8_t rules_version;
    ww_player_mask_t occupied_mask;
    uint8_t offered_seat;
    uint8_t room_fingerprint[WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE];
    uint8_t host_commitment[WEREWOLF_MESSAGES_COMMITMENT_SIZE];
} werewolf_beacon_message_t;

typedef struct {
    uint8_t candidate_seat;
    uint8_t client_commitment[WEREWOLF_MESSAGES_COMMITMENT_SIZE];
} werewolf_join_message_t;

typedef struct {
    uint8_t offered_seat;
    uint8_t host_public_key[WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE];
    uint8_t host_nonce[WEREWOLF_MESSAGES_NONCE_SIZE];
    uint8_t locked_client_commitment[WEREWOLF_MESSAGES_COMMITMENT_SIZE];
} werewolf_pair_host_reveal_message_t;

typedef struct {
    uint8_t candidate_seat;
    uint8_t client_public_key[WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE];
    uint8_t client_nonce[WEREWOLF_MESSAGES_NONCE_SIZE];
    uint8_t echoed_host_commitment[WEREWOLF_MESSAGES_COMMITMENT_SIZE];
} werewolf_pair_client_reveal_message_t;

typedef struct {
    uint8_t seat;
} werewolf_accept_message_t;

typedef struct {
    bool ready;
} werewolf_ready_message_t;

typedef struct {
    werewolf_nickname_t nickname;
} werewolf_profile_message_t;

typedef struct {
    uint32_t lobby_revision;
    ww_player_mask_t profile_mask;
    werewolf_nickname_t names[WW_PLAYER_COUNT];
} werewolf_roster_message_t;

/* Wire offsets: phase 0, winner 1, phase_epoch 2..5, round 6..7,
 * occupied/alive/submitted/tie 8..11, speaker/victim/exile 12..14,
 * gate kind 15, gate epoch 16..19. */
typedef struct {
    ww_phase_t phase;
    ww_winner_t winner;
    uint32_t phase_epoch;
    uint16_t round_number;
    ww_player_mask_t occupied_mask;
    ww_player_mask_t alive_mask;
    ww_player_mask_t submitted_mask;
    ww_player_mask_t tie_mask;
    uint8_t current_speaker;
    uint8_t dawn_victim;
    uint8_t exiled_player;
    werewolf_gate_kind_t gate_kind;
    uint32_t gate_epoch;
} werewolf_public_state_message_t;

typedef struct {
    werewolf_public_state_message_t public_state;
    werewolf_roster_message_t roster;
} werewolf_start_message_t;

typedef struct {
    ww_role_t role;
    ww_player_mask_t wolf_teammate_mask;
    uint8_t seer_result_seat;
    ww_camp_t seer_result_faction;
    werewolf_gate_kind_t gate_kind;
    uint32_t gate_epoch;
} werewolf_private_role_message_t;

typedef struct {
    werewolf_public_state_message_t public_state;
} werewolf_phase_message_t;

typedef struct {
    werewolf_action_kind_t kind;
    uint8_t target;
    uint32_t expected_phase_epoch;
    werewolf_gate_kind_t expected_gate_kind;
    uint32_t expected_gate_epoch;
} werewolf_action_message_t;

typedef struct {
    uint32_t phase_epoch;
    ww_player_mask_t occupied_mask;
    ww_player_mask_t ready_mask;
    werewolf_roster_message_t roster;
} werewolf_lobby_snapshot_t;

typedef struct {
    werewolf_public_state_message_t public_state;
    /* This field is meaningful only to the encrypted unicast recipient. */
    bool local_gate_acknowledged;
} werewolf_game_snapshot_t;

typedef struct {
    werewolf_snapshot_kind_t kind;
    union {
        werewolf_lobby_snapshot_t lobby;
        werewolf_game_snapshot_t game;
        /* Controller sends this exact deck only after GAME_OVER. */
        ww_role_t roles[WW_PLAYER_COUNT];
    } body;
} werewolf_snapshot_message_t;

typedef struct {
    werewolf_abort_reason_t reason;
} werewolf_abort_message_t;

typedef struct {
    werewolf_message_type_t type;
    union {
        werewolf_beacon_message_t beacon;
        werewolf_join_message_t join;
        werewolf_pair_host_reveal_message_t pair_host_reveal;
        werewolf_pair_client_reveal_message_t pair_client_reveal;
        werewolf_accept_message_t accept;
        werewolf_ready_message_t ready;
        werewolf_profile_message_t profile;
        werewolf_start_message_t start;
        werewolf_private_role_message_t private_role;
        werewolf_phase_message_t phase;
        werewolf_action_message_t action;
        werewolf_snapshot_message_t snapshot;
        werewolf_abort_message_t abort;
    } body;
} werewolf_message_t;

/* On encode failure *payload_len is zero. Decode accepts only the exact length
 * for the supplied protocol message type and clears *message on failure. */
werewolf_messages_result_t werewolf_messages_encode(
    const werewolf_message_t *message,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_len);

werewolf_messages_result_t werewolf_messages_decode(
    werewolf_message_type_t type,
    const uint8_t *payload,
    size_t payload_len,
    werewolf_message_t *message);

/* Pure host-testable predicate used before applying an authenticated action. */
bool werewolf_messages_action_matches_gate(
    const werewolf_action_message_t *action,
    werewolf_gate_kind_t current_gate,
    uint32_t current_gate_epoch);

/* Exact gate binding for PRIVATE_ROLE delivery/reordering decisions. */
bool werewolf_messages_private_matches_gate(
    const werewolf_private_role_message_t *private_message,
    werewolf_gate_kind_t current_gate,
    uint32_t current_gate_epoch);

/* True only after all result-confirmation gates have closed at GAME_OVER. */
bool werewolf_messages_public_allows_role_reveal(
    const werewolf_public_state_message_t *public_state);

/* Snapshot reconciliation for non-gate client actions. */
bool werewolf_messages_public_confirms_normal_action(
    werewolf_action_kind_t kind,
    uint32_t expected_phase_epoch,
    uint8_t actor,
    uint8_t speaker_at_send,
    const werewolf_public_state_message_t *public_state);

const char *werewolf_messages_result_string(werewolf_messages_result_t result);

#ifdef __cplusplus
}
#endif
