#include "werewolf_messages.h"

#include <string.h>

_Static_assert(WEREWOLF_MESSAGES_BEACON_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "BEACON exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_PAIR_HOST_REVEAL_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "PAIR_HOST_REVEAL exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_PAIR_CLIENT_REVEAL_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "PAIR_CLIENT_REVEAL exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_PROFILE_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "PROFILE exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_START_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "START exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "LOBBY SNAPSHOT exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_SNAPSHOT_GAME_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "SNAPSHOT exceeds protocol payload capacity");
_Static_assert(WEREWOLF_MESSAGES_SNAPSHOT_ROLE_REVEAL_SIZE <=
               WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE,
               "ROLE_REVEAL exceeds protocol payload capacity");

static void put_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void put_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint16_t get_u16_be(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) |
                      (uint16_t)source[1]);
}

static uint32_t get_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) |
           ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) |
           (uint32_t)source[3];
}

static void copy_bytes(uint8_t *destination, const uint8_t *source,
                       size_t length)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        destination[index] = source[index];
    }
}

static void encode_nickname_slot(const char *nickname, uint8_t *payload)
{
    size_t length = strlen(nickname);

    payload[0] = (uint8_t)length;
    memset(&payload[1], 0, WEREWOLF_NICKNAME_MAX_CHARS);
    copy_bytes(&payload[1], (const uint8_t *)nickname, length);
}

static bool decode_nickname_slot(const uint8_t *payload,
                                 werewolf_nickname_t nickname)
{
    size_t length = payload[0];

    if (length == 0U || length > WEREWOLF_NICKNAME_MAX_CHARS) {
        return false;
    }
    for (size_t index = length;
         index < WEREWOLF_NICKNAME_MAX_CHARS; ++index) {
        if (payload[1U + index] != 0U) {
            return false;
        }
    }
    memset(nickname, 0, WEREWOLF_NICKNAME_CAPACITY);
    copy_bytes((uint8_t *)nickname, &payload[1], length);
    if (!werewolf_nickname_valid(nickname)) {
        memset(nickname, 0, WEREWOLF_NICKNAME_CAPACITY);
        return false;
    }
    return true;
}

static void encode_roster(const werewolf_roster_message_t *roster,
                          uint8_t *payload)
{
    put_u32_be(payload, roster->lobby_revision);
    payload[4] = roster->profile_mask;
    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        uint8_t *slot = &payload[5U +
            (size_t)seat * WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE];

        if ((roster->profile_mask &
             (ww_player_mask_t)(UINT8_C(1) << seat)) != 0U) {
            encode_nickname_slot(roster->names[seat], slot);
        } else {
            memset(slot, 0, WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE);
        }
    }
}

static bool decode_roster(const uint8_t *payload,
                          werewolf_roster_message_t *roster)
{
    roster->lobby_revision = get_u32_be(payload);
    roster->profile_mask = payload[4];
    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        const uint8_t *slot = &payload[5U +
            (size_t)seat * WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE];
        bool present = (roster->profile_mask &
            (ww_player_mask_t)(UINT8_C(1) << seat)) != 0U;

        if (present) {
            if (!decode_nickname_slot(slot, roster->names[seat])) {
                return false;
            }
        } else {
            for (size_t index = 0U;
                 index < WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE; ++index) {
                if (slot[index] != 0U) {
                    return false;
                }
            }
            roster->names[seat][0] = '\0';
        }
    }
    return true;
}

static bool bytes_have_nonzero(const uint8_t *bytes, size_t length)
{
    size_t index;
    uint8_t combined = 0u;

    for (index = 0; index < length; ++index) {
        combined |= bytes[index];
    }
    return combined != 0u;
}

static bool mask_valid(ww_player_mask_t mask)
{
    return (mask & (ww_player_mask_t)~WW_ALL_PLAYERS_MASK) == 0u;
}

static bool mask_has_one_player(ww_player_mask_t mask)
{
    return mask != 0u &&
           (mask & (ww_player_mask_t)(mask - UINT8_C(1))) == 0u;
}

static bool player_valid(uint8_t player)
{
    return player < WW_PLAYER_COUNT;
}

static bool player_or_none_valid(uint8_t player)
{
    return player == WW_NO_PLAYER || player_valid(player);
}

static bool phase_valid(ww_phase_t phase)
{
    switch (phase) {
    case WW_PHASE_LOBBY:
    case WW_PHASE_NIGHT:
    case WW_PHASE_WOLF_REVOTE:
    case WW_PHASE_DAWN_RESULT:
    case WW_PHASE_DISCUSSION:
    case WW_PHASE_VOTE:
    case WW_PHASE_TIE_DEFENSE:
    case WW_PHASE_REVOTE:
    case WW_PHASE_EXILE_RESULT:
    case WW_PHASE_GAME_OVER:
        return true;
    default:
        return false;
    }
}

static bool winner_valid(ww_winner_t winner)
{
    return winner == WW_WINNER_NONE || winner == WW_WINNER_GOOD ||
           winner == WW_WINNER_WOLVES;
}

static bool role_valid(ww_role_t role)
{
    return role == WW_ROLE_WOLF || role == WW_ROLE_SEER ||
           role == WW_ROLE_GUARD || role == WW_ROLE_VILLAGER;
}

static bool camp_valid(ww_camp_t camp)
{
    return camp == WW_CAMP_UNKNOWN || camp == WW_CAMP_GOOD ||
           camp == WW_CAMP_WOLF;
}

static bool action_kind_valid(werewolf_action_kind_t kind)
{
    return kind == WEREWOLF_ACTION_NIGHT_TARGET ||
           kind == WEREWOLF_ACTION_PASS_SPEECH ||
           kind == WEREWOLF_ACTION_VOTE_TARGET ||
           kind == WEREWOLF_ACTION_ROLE_SEEN ||
           kind == WEREWOLF_ACTION_ACK_RESULT ||
           kind == WEREWOLF_ACTION_LEAVE_GAME;
}

static bool gate_kind_valid(werewolf_gate_kind_t kind)
{
    return kind == WEREWOLF_GATE_NONE || kind == WEREWOLF_GATE_ROLE ||
           kind == WEREWOLF_GATE_PRIVATE_RESULT ||
           kind == WEREWOLF_GATE_DAWN || kind == WEREWOLF_GATE_EXILE;
}

static bool gate_matches_phase(werewolf_gate_kind_t gate, ww_phase_t phase)
{
    if (gate == WEREWOLF_GATE_ROLE) {
        return phase == WW_PHASE_NIGHT;
    }
    if (gate == WEREWOLF_GATE_PRIVATE_RESULT ||
        gate == WEREWOLF_GATE_DAWN) {
        return phase == WW_PHASE_DAWN_RESULT ||
               phase == WW_PHASE_GAME_OVER;
    }
    if (gate == WEREWOLF_GATE_EXILE) {
        return phase == WW_PHASE_EXILE_RESULT ||
               phase == WW_PHASE_GAME_OVER;
    }
    return gate == WEREWOLF_GATE_NONE;
}

static bool abort_reason_valid(werewolf_abort_reason_t reason)
{
    return reason >= WEREWOLF_ABORT_HOST_LOST &&
           reason <= WEREWOLF_ABORT_KICKED_BY_HOST;
}

static werewolf_messages_result_t validate_public_state(
    const werewolf_public_state_message_t *state)
{
    if (!phase_valid(state->phase) || state->phase == WW_PHASE_LOBBY ||
        !winner_valid(state->winner) ||
        !gate_kind_valid(state->gate_kind)) {
        return WEREWOLF_MESSAGES_ERR_ENUM;
    }
    if (state->phase_epoch == 0u || state->round_number == 0u ||
        state->gate_epoch == 0u ||
        !gate_matches_phase(state->gate_kind, state->phase)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!mask_valid(state->occupied_mask) ||
        !mask_valid(state->alive_mask) ||
        !mask_valid(state->submitted_mask) ||
        !mask_valid(state->tie_mask)) {
        return WEREWOLF_MESSAGES_ERR_MASK;
    }
    if (state->occupied_mask != WW_ALL_PLAYERS_MASK ||
        state->alive_mask == 0u ||
        (state->alive_mask & state->occupied_mask) != state->alive_mask ||
        (state->submitted_mask & state->alive_mask) !=
            state->submitted_mask ||
        (state->tie_mask & state->alive_mask) != state->tie_mask) {
        return WEREWOLF_MESSAGES_ERR_MASK;
    }
    if (!player_or_none_valid(state->current_speaker) ||
        !player_or_none_valid(state->dawn_victim) ||
        !player_or_none_valid(state->exiled_player)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (state->current_speaker != WW_NO_PLAYER) {
        ww_player_mask_t speaker_bit =
            (ww_player_mask_t)(UINT8_C(1) << state->current_speaker);

        if ((state->phase != WW_PHASE_DISCUSSION &&
             state->phase != WW_PHASE_TIE_DEFENSE) ||
            (state->alive_mask & speaker_bit) == 0u ||
            (state->phase == WW_PHASE_TIE_DEFENSE &&
             (state->tie_mask & speaker_bit) == 0u)) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    }
    if (state->phase != WW_PHASE_DISCUSSION &&
        state->phase != WW_PHASE_TIE_DEFENSE &&
        state->current_speaker != WW_NO_PLAYER) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (state->phase == WW_PHASE_GAME_OVER) {
        if (state->winner == WW_WINNER_NONE) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    } else if (state->winner != WW_WINNER_NONE) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_roster(
    const werewolf_roster_message_t *roster,
    ww_player_mask_t occupied_mask, bool require_complete)
{
    if (roster->lobby_revision == 0U) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!mask_valid(roster->profile_mask) ||
        (roster->profile_mask & occupied_mask) != roster->profile_mask ||
        (require_complete && roster->profile_mask != occupied_mask)) {
        return WEREWOLF_MESSAGES_ERR_MASK;
    }
    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        bool profiled = (roster->profile_mask &
            (ww_player_mask_t)(UINT8_C(1) << seat)) != 0U;

        if (profiled != werewolf_nickname_valid(roster->names[seat])) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_profile(
    const werewolf_profile_message_t *profile)
{
    return werewolf_nickname_valid(profile->nickname)
               ? WEREWOLF_MESSAGES_OK
               : WEREWOLF_MESSAGES_ERR_VALUE;
}

static werewolf_messages_result_t validate_start(
    const werewolf_start_message_t *start)
{
    const werewolf_public_state_message_t *state = &start->public_state;
    werewolf_messages_result_t result = validate_public_state(state);

    if (result != WEREWOLF_MESSAGES_OK) {
        return result;
    }
    if (state->phase != WW_PHASE_NIGHT || state->winner != WW_WINNER_NONE ||
        state->round_number != 1u ||
        state->alive_mask != WW_ALL_PLAYERS_MASK ||
        state->submitted_mask != 0u || state->tie_mask != 0u ||
        state->current_speaker != WW_NO_PLAYER ||
        state->dawn_victim != WW_NO_PLAYER ||
        state->exiled_player != WW_NO_PLAYER ||
        state->gate_kind != WEREWOLF_GATE_ROLE) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return validate_roster(&start->roster, state->occupied_mask, true);
}

static werewolf_messages_result_t validate_beacon(
    const werewolf_beacon_message_t *beacon)
{
    if (beacon->protocol_version != WEREWOLF_PROTOCOL_VERSION ||
        beacon->rules_version != WEREWOLF_MESSAGES_RULES_VERSION) {
        return WEREWOLF_MESSAGES_ERR_VERSION;
    }
    if (!mask_valid(beacon->occupied_mask) ||
        (beacon->occupied_mask & UINT8_C(1)) == 0u) {
        return WEREWOLF_MESSAGES_ERR_MASK;
    }
    if (beacon->offered_seat == 0u ||
        beacon->offered_seat >= WW_PLAYER_COUNT ||
        (beacon->occupied_mask &
         (ww_player_mask_t)(UINT8_C(1) << beacon->offered_seat)) != 0u) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!bytes_have_nonzero(beacon->room_fingerprint,
                            WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE) ||
        !bytes_have_nonzero(beacon->host_commitment,
                            WEREWOLF_MESSAGES_COMMITMENT_SIZE)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_join(
    const werewolf_join_message_t *join)
{
    if (join->candidate_seat == 0u ||
        join->candidate_seat >= WW_PLAYER_COUNT) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!bytes_have_nonzero(join->client_commitment,
                            WEREWOLF_MESSAGES_COMMITMENT_SIZE)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_pair_host_reveal(
    const werewolf_pair_host_reveal_message_t *reveal)
{
    if (reveal->offered_seat == 0u ||
        reveal->offered_seat >= WW_PLAYER_COUNT) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!bytes_have_nonzero(reveal->host_public_key,
                            WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE) ||
        !bytes_have_nonzero(reveal->host_nonce,
                            WEREWOLF_MESSAGES_NONCE_SIZE) ||
        !bytes_have_nonzero(reveal->locked_client_commitment,
                            WEREWOLF_MESSAGES_COMMITMENT_SIZE)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_pair_client_reveal(
    const werewolf_pair_client_reveal_message_t *reveal)
{
    if (reveal->candidate_seat == 0u ||
        reveal->candidate_seat >= WW_PLAYER_COUNT) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!bytes_have_nonzero(reveal->client_public_key,
                            WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE) ||
        !bytes_have_nonzero(reveal->client_nonce,
                            WEREWOLF_MESSAGES_NONCE_SIZE) ||
        !bytes_have_nonzero(reveal->echoed_host_commitment,
                            WEREWOLF_MESSAGES_COMMITMENT_SIZE)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_accept(
    const werewolf_accept_message_t *accept)
{
    if (accept->seat == 0u || accept->seat >= WW_PLAYER_COUNT) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_private_role(
    const werewolf_private_role_message_t *private_role)
{
    if (!role_valid(private_role->role) ||
        !camp_valid(private_role->seer_result_faction) ||
        !gate_kind_valid(private_role->gate_kind)) {
        return WEREWOLF_MESSAGES_ERR_ENUM;
    }
    if ((private_role->gate_kind != WEREWOLF_GATE_ROLE &&
         private_role->gate_kind != WEREWOLF_GATE_PRIVATE_RESULT) ||
        private_role->gate_epoch == 0u) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (!mask_valid(private_role->wolf_teammate_mask)) {
        return WEREWOLF_MESSAGES_ERR_MASK;
    }
    if (!player_or_none_valid(private_role->seer_result_seat)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (private_role->role == WW_ROLE_WOLF) {
        if (!mask_has_one_player(private_role->wolf_teammate_mask)) {
            return WEREWOLF_MESSAGES_ERR_MASK;
        }
    } else if (private_role->wolf_teammate_mask != 0u) {
        return WEREWOLF_MESSAGES_ERR_MASK;
    }

    if (private_role->role == WW_ROLE_SEER) {
        if (private_role->seer_result_seat == WW_NO_PLAYER) {
            if (private_role->seer_result_faction != WW_CAMP_UNKNOWN) {
                return WEREWOLF_MESSAGES_ERR_VALUE;
            }
        } else if (private_role->seer_result_faction != WW_CAMP_GOOD &&
                   private_role->seer_result_faction != WW_CAMP_WOLF) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    } else if (private_role->seer_result_seat != WW_NO_PLAYER ||
               private_role->seer_result_faction != WW_CAMP_UNKNOWN) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (private_role->gate_kind == WEREWOLF_GATE_ROLE &&
        (private_role->seer_result_seat != WW_NO_PLAYER ||
         private_role->seer_result_faction != WW_CAMP_UNKNOWN)) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_action(
    const werewolf_action_message_t *action)
{
    if (!action_kind_valid(action->kind) ||
        !gate_kind_valid(action->expected_gate_kind)) {
        return WEREWOLF_MESSAGES_ERR_ENUM;
    }
    if (action->expected_phase_epoch == 0u ||
        action->expected_gate_epoch == 0u) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (action->kind == WEREWOLF_ACTION_NIGHT_TARGET ||
        action->kind == WEREWOLF_ACTION_VOTE_TARGET) {
        if (!player_valid(action->target)) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    } else if (action->target != WW_NO_PLAYER) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    if (action->kind == WEREWOLF_ACTION_ROLE_SEEN) {
        if (action->expected_gate_kind != WEREWOLF_GATE_ROLE) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    } else if (action->kind == WEREWOLF_ACTION_ACK_RESULT) {
        if (action->expected_gate_kind != WEREWOLF_GATE_PRIVATE_RESULT &&
            action->expected_gate_kind != WEREWOLF_GATE_DAWN &&
            action->expected_gate_kind != WEREWOLF_GATE_EXILE) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
    } else if (action->kind != WEREWOLF_ACTION_LEAVE_GAME &&
               action->expected_gate_kind != WEREWOLF_GATE_NONE) {
        return WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_OK;
}

static werewolf_messages_result_t validate_snapshot(
    const werewolf_snapshot_message_t *snapshot)
{
    if (snapshot->kind == WEREWOLF_SNAPSHOT_LOBBY) {
        const werewolf_lobby_snapshot_t *lobby = &snapshot->body.lobby;

        if (lobby->phase_epoch == 0u) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
        if (!mask_valid(lobby->occupied_mask) ||
            !mask_valid(lobby->ready_mask) ||
            (lobby->occupied_mask & UINT8_C(1)) == 0u ||
            (lobby->ready_mask & lobby->occupied_mask) != lobby->ready_mask) {
            return WEREWOLF_MESSAGES_ERR_MASK;
        }
        return validate_roster(&lobby->roster, lobby->occupied_mask, false);
    }
    if (snapshot->kind == WEREWOLF_SNAPSHOT_GAME) {
        return validate_public_state(&snapshot->body.game.public_state);
    }
    if (snapshot->kind == WEREWOLF_SNAPSHOT_ROLE_REVEAL) {
        uint8_t wolves = 0u;
        uint8_t seers = 0u;
        uint8_t guards = 0u;
        uint8_t villagers = 0u;
        uint8_t player;

        for (player = 0u; player < WW_PLAYER_COUNT; ++player) {
            switch (snapshot->body.roles[player]) {
            case WW_ROLE_WOLF:
                ++wolves;
                break;
            case WW_ROLE_SEER:
                ++seers;
                break;
            case WW_ROLE_GUARD:
                ++guards;
                break;
            case WW_ROLE_VILLAGER:
                ++villagers;
                break;
            case WW_ROLE_NONE:
            default:
                return WEREWOLF_MESSAGES_ERR_ENUM;
            }
        }
        return wolves == 2u && seers == 1u && guards == 1u && villagers == 3u
                   ? WEREWOLF_MESSAGES_OK
                   : WEREWOLF_MESSAGES_ERR_VALUE;
    }
    return WEREWOLF_MESSAGES_ERR_ENUM;
}

static werewolf_messages_result_t validate_message(
    const werewolf_message_t *message, size_t *encoded_size)
{
    werewolf_messages_result_t result;

    if (message == NULL || encoded_size == NULL) {
        return WEREWOLF_MESSAGES_ERR_ARGUMENT;
    }

    switch (message->type) {
    case WEREWOLF_MSG_BEACON:
        result = validate_beacon(&message->body.beacon);
        *encoded_size = WEREWOLF_MESSAGES_BEACON_SIZE;
        break;
    case WEREWOLF_MSG_JOIN:
        result = validate_join(&message->body.join);
        *encoded_size = WEREWOLF_MESSAGES_JOIN_SIZE;
        break;
    case WEREWOLF_MSG_PAIR_HOST_REVEAL:
        result = validate_pair_host_reveal(&message->body.pair_host_reveal);
        *encoded_size = WEREWOLF_MESSAGES_PAIR_HOST_REVEAL_SIZE;
        break;
    case WEREWOLF_MSG_PAIR_CLIENT_REVEAL:
        result = validate_pair_client_reveal(
            &message->body.pair_client_reveal);
        *encoded_size = WEREWOLF_MESSAGES_PAIR_CLIENT_REVEAL_SIZE;
        break;
    case WEREWOLF_MSG_ACCEPT:
        result = validate_accept(&message->body.accept);
        *encoded_size = WEREWOLF_MESSAGES_ACCEPT_SIZE;
        break;
    case WEREWOLF_MSG_READY:
        result = WEREWOLF_MESSAGES_OK;
        *encoded_size = WEREWOLF_MESSAGES_READY_SIZE;
        break;
    case WEREWOLF_MSG_PROFILE:
        result = validate_profile(&message->body.profile);
        *encoded_size = WEREWOLF_MESSAGES_PROFILE_SIZE;
        break;
    case WEREWOLF_MSG_START:
        result = validate_start(&message->body.start);
        *encoded_size = WEREWOLF_MESSAGES_START_SIZE;
        break;
    case WEREWOLF_MSG_PRIVATE_ROLE:
        result = validate_private_role(&message->body.private_role);
        *encoded_size = WEREWOLF_MESSAGES_PRIVATE_ROLE_SIZE;
        break;
    case WEREWOLF_MSG_PHASE:
        result = validate_public_state(&message->body.phase.public_state);
        *encoded_size = WEREWOLF_MESSAGES_PHASE_SIZE;
        break;
    case WEREWOLF_MSG_ACTION:
        result = validate_action(&message->body.action);
        *encoded_size = WEREWOLF_MESSAGES_ACTION_SIZE;
        break;
    case WEREWOLF_MSG_SNAPSHOT:
        result = validate_snapshot(&message->body.snapshot);
        if (message->body.snapshot.kind == WEREWOLF_SNAPSHOT_LOBBY) {
            *encoded_size = WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE;
        } else if (message->body.snapshot.kind == WEREWOLF_SNAPSHOT_GAME) {
            *encoded_size = WEREWOLF_MESSAGES_SNAPSHOT_GAME_SIZE;
        } else if (message->body.snapshot.kind ==
                   WEREWOLF_SNAPSHOT_ROLE_REVEAL) {
            *encoded_size = WEREWOLF_MESSAGES_SNAPSHOT_ROLE_REVEAL_SIZE;
        } else {
            *encoded_size = 0u;
        }
        break;
    case WEREWOLF_MSG_ABORT:
        result = abort_reason_valid(message->body.abort.reason)
                     ? WEREWOLF_MESSAGES_OK
                     : WEREWOLF_MESSAGES_ERR_ENUM;
        *encoded_size = WEREWOLF_MESSAGES_ABORT_SIZE;
        break;
    default:
        *encoded_size = 0u;
        return WEREWOLF_MESSAGES_ERR_TYPE;
    }
    return result;
}

static void encode_public_state(const werewolf_public_state_message_t *state,
                                uint8_t *payload)
{
    payload[0] = (uint8_t)state->phase;
    payload[1] = (uint8_t)state->winner;
    put_u32_be(&payload[2], state->phase_epoch);
    put_u16_be(&payload[6], state->round_number);
    payload[8] = state->occupied_mask;
    payload[9] = state->alive_mask;
    payload[10] = state->submitted_mask;
    payload[11] = state->tie_mask;
    payload[12] = state->current_speaker;
    payload[13] = state->dawn_victim;
    payload[14] = state->exiled_player;
    payload[15] = (uint8_t)state->gate_kind;
    put_u32_be(&payload[16], state->gate_epoch);
}

static void decode_public_state(const uint8_t *payload,
                                werewolf_public_state_message_t *state)
{
    state->phase = (ww_phase_t)payload[0];
    state->winner = (ww_winner_t)payload[1];
    state->phase_epoch = get_u32_be(&payload[2]);
    state->round_number = get_u16_be(&payload[6]);
    state->occupied_mask = payload[8];
    state->alive_mask = payload[9];
    state->submitted_mask = payload[10];
    state->tie_mask = payload[11];
    state->current_speaker = payload[12];
    state->dawn_victim = payload[13];
    state->exiled_player = payload[14];
    state->gate_kind = (werewolf_gate_kind_t)payload[15];
    state->gate_epoch = get_u32_be(&payload[16]);
}

werewolf_messages_result_t werewolf_messages_encode(
    const werewolf_message_t *message,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_len)
{
    size_t encoded_size = 0u;
    werewolf_messages_result_t result;

    if (payload_len == NULL) {
        return WEREWOLF_MESSAGES_ERR_ARGUMENT;
    }
    *payload_len = 0u;
    if (message == NULL || payload == NULL) {
        return WEREWOLF_MESSAGES_ERR_ARGUMENT;
    }
    result = validate_message(message, &encoded_size);
    if (result != WEREWOLF_MESSAGES_OK) {
        return result;
    }
    if (payload_capacity < encoded_size) {
        return WEREWOLF_MESSAGES_ERR_CAPACITY;
    }

    switch (message->type) {
    case WEREWOLF_MSG_BEACON:
        payload[0] = message->body.beacon.protocol_version;
        payload[1] = message->body.beacon.rules_version;
        payload[2] = message->body.beacon.occupied_mask;
        payload[3] = message->body.beacon.offered_seat;
        copy_bytes(&payload[4], message->body.beacon.room_fingerprint,
                   WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE);
        copy_bytes(&payload[12], message->body.beacon.host_commitment,
                   WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_JOIN:
        payload[0] = message->body.join.candidate_seat;
        copy_bytes(&payload[1], message->body.join.client_commitment,
                   WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_PAIR_HOST_REVEAL:
        payload[0] = message->body.pair_host_reveal.offered_seat;
        copy_bytes(&payload[1],
                   message->body.pair_host_reveal.host_public_key,
                   WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE);
        copy_bytes(&payload[33], message->body.pair_host_reveal.host_nonce,
                   WEREWOLF_MESSAGES_NONCE_SIZE);
        copy_bytes(&payload[49],
                   message->body.pair_host_reveal.locked_client_commitment,
                   WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_PAIR_CLIENT_REVEAL:
        payload[0] = message->body.pair_client_reveal.candidate_seat;
        copy_bytes(&payload[1],
                   message->body.pair_client_reveal.client_public_key,
                   WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE);
        copy_bytes(&payload[33], message->body.pair_client_reveal.client_nonce,
                   WEREWOLF_MESSAGES_NONCE_SIZE);
        copy_bytes(
            &payload[49],
            message->body.pair_client_reveal.echoed_host_commitment,
            WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_ACCEPT:
        payload[0] = message->body.accept.seat;
        break;
    case WEREWOLF_MSG_READY:
        payload[0] = message->body.ready.ready ? UINT8_C(1) : UINT8_C(0);
        break;
    case WEREWOLF_MSG_PROFILE:
        encode_nickname_slot(message->body.profile.nickname, payload);
        break;
    case WEREWOLF_MSG_START:
        encode_public_state(&message->body.start.public_state, payload);
        encode_roster(&message->body.start.roster,
                      &payload[WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE]);
        break;
    case WEREWOLF_MSG_PRIVATE_ROLE:
        payload[0] = (uint8_t)message->body.private_role.role;
        payload[1] = message->body.private_role.wolf_teammate_mask;
        payload[2] = message->body.private_role.seer_result_seat;
        payload[3] = (uint8_t)message->body.private_role.seer_result_faction;
        payload[4] = (uint8_t)message->body.private_role.gate_kind;
        put_u32_be(&payload[5], message->body.private_role.gate_epoch);
        break;
    case WEREWOLF_MSG_PHASE:
        encode_public_state(&message->body.phase.public_state, payload);
        break;
    case WEREWOLF_MSG_ACTION:
        payload[0] = (uint8_t)message->body.action.kind;
        payload[1] = message->body.action.target;
        put_u32_be(&payload[2], message->body.action.expected_phase_epoch);
        payload[6] = (uint8_t)message->body.action.expected_gate_kind;
        put_u32_be(&payload[7], message->body.action.expected_gate_epoch);
        break;
    case WEREWOLF_MSG_SNAPSHOT:
        payload[0] = (uint8_t)message->body.snapshot.kind;
        if (message->body.snapshot.kind == WEREWOLF_SNAPSHOT_LOBBY) {
            put_u32_be(&payload[1],
                       message->body.snapshot.body.lobby.phase_epoch);
            payload[5] = message->body.snapshot.body.lobby.occupied_mask;
            payload[6] = message->body.snapshot.body.lobby.ready_mask;
            encode_roster(&message->body.snapshot.body.lobby.roster,
                          &payload[7]);
        } else if (message->body.snapshot.kind == WEREWOLF_SNAPSHOT_GAME) {
            encode_public_state(
                                &message->body.snapshot.body.game.public_state,
                                &payload[1]);
            payload[1u + WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE] =
                message->body.snapshot.body.game.local_gate_acknowledged
                    ? UINT8_C(1)
                    : UINT8_C(0);
        } else {
            uint8_t player;

            for (player = 0u; player < WW_PLAYER_COUNT; ++player) {
                payload[1u + player] =
                    (uint8_t)message->body.snapshot.body.roles[player];
            }
        }
        break;
    case WEREWOLF_MSG_ABORT:
        payload[0] = (uint8_t)message->body.abort.reason;
        break;
    default:
        return WEREWOLF_MESSAGES_ERR_TYPE;
    }

    *payload_len = encoded_size;
    return WEREWOLF_MESSAGES_OK;
}

werewolf_messages_result_t werewolf_messages_decode(
    werewolf_message_type_t type,
    const uint8_t *payload,
    size_t payload_len,
    werewolf_message_t *message)
{
    werewolf_message_t decoded = { 0 };
    size_t expected_size = 0u;
    werewolf_messages_result_t result;

    if (message == NULL) {
        return WEREWOLF_MESSAGES_ERR_ARGUMENT;
    }
    *message = (werewolf_message_t){ 0 };
    if (payload == NULL) {
        return WEREWOLF_MESSAGES_ERR_ARGUMENT;
    }
    decoded.type = type;

    switch (type) {
    case WEREWOLF_MSG_BEACON:
        if (payload_len != WEREWOLF_MESSAGES_BEACON_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.beacon.protocol_version = payload[0];
        decoded.body.beacon.rules_version = payload[1];
        decoded.body.beacon.occupied_mask = payload[2];
        decoded.body.beacon.offered_seat = payload[3];
        copy_bytes(decoded.body.beacon.room_fingerprint, &payload[4],
                   WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE);
        copy_bytes(decoded.body.beacon.host_commitment, &payload[12],
                   WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_JOIN:
        if (payload_len != WEREWOLF_MESSAGES_JOIN_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.join.candidate_seat = payload[0];
        copy_bytes(decoded.body.join.client_commitment, &payload[1],
                   WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_PAIR_HOST_REVEAL:
        if (payload_len != WEREWOLF_MESSAGES_PAIR_HOST_REVEAL_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.pair_host_reveal.offered_seat = payload[0];
        copy_bytes(decoded.body.pair_host_reveal.host_public_key,
                   &payload[1], WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE);
        copy_bytes(decoded.body.pair_host_reveal.host_nonce, &payload[33],
                   WEREWOLF_MESSAGES_NONCE_SIZE);
        copy_bytes(decoded.body.pair_host_reveal.locked_client_commitment,
                   &payload[49], WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_PAIR_CLIENT_REVEAL:
        if (payload_len != WEREWOLF_MESSAGES_PAIR_CLIENT_REVEAL_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.pair_client_reveal.candidate_seat = payload[0];
        copy_bytes(decoded.body.pair_client_reveal.client_public_key,
                   &payload[1], WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE);
        copy_bytes(decoded.body.pair_client_reveal.client_nonce, &payload[33],
                   WEREWOLF_MESSAGES_NONCE_SIZE);
        copy_bytes(
            decoded.body.pair_client_reveal.echoed_host_commitment,
            &payload[49], WEREWOLF_MESSAGES_COMMITMENT_SIZE);
        break;
    case WEREWOLF_MSG_ACCEPT:
        if (payload_len != WEREWOLF_MESSAGES_ACCEPT_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.accept.seat = payload[0];
        break;
    case WEREWOLF_MSG_READY:
        if (payload_len != WEREWOLF_MESSAGES_READY_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        if (payload[0] > 1u) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
        decoded.body.ready.ready = payload[0] == 1u;
        break;
    case WEREWOLF_MSG_PROFILE:
        if (payload_len != WEREWOLF_MESSAGES_PROFILE_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        if (!decode_nickname_slot(payload,
                                  decoded.body.profile.nickname)) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
        break;
    case WEREWOLF_MSG_START:
        if (payload_len != WEREWOLF_MESSAGES_START_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decode_public_state(payload, &decoded.body.start.public_state);
        if (!decode_roster(
                &payload[WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE],
                &decoded.body.start.roster)) {
            return WEREWOLF_MESSAGES_ERR_VALUE;
        }
        break;
    case WEREWOLF_MSG_PRIVATE_ROLE:
        if (payload_len != WEREWOLF_MESSAGES_PRIVATE_ROLE_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.private_role.role = (ww_role_t)payload[0];
        decoded.body.private_role.wolf_teammate_mask = payload[1];
        decoded.body.private_role.seer_result_seat = payload[2];
        decoded.body.private_role.seer_result_faction =
            (ww_camp_t)payload[3];
        decoded.body.private_role.gate_kind =
            (werewolf_gate_kind_t)payload[4];
        decoded.body.private_role.gate_epoch = get_u32_be(&payload[5]);
        break;
    case WEREWOLF_MSG_PHASE:
        if (payload_len != WEREWOLF_MESSAGES_PHASE_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decode_public_state(payload, &decoded.body.phase.public_state);
        break;
    case WEREWOLF_MSG_ACTION:
        if (payload_len != WEREWOLF_MESSAGES_ACTION_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.action.kind = (werewolf_action_kind_t)payload[0];
        decoded.body.action.target = payload[1];
        decoded.body.action.expected_phase_epoch = get_u32_be(&payload[2]);
        decoded.body.action.expected_gate_kind =
            (werewolf_gate_kind_t)payload[6];
        decoded.body.action.expected_gate_epoch = get_u32_be(&payload[7]);
        break;
    case WEREWOLF_MSG_SNAPSHOT:
        if (payload_len == 0u) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.snapshot.kind = (werewolf_snapshot_kind_t)payload[0];
        if (decoded.body.snapshot.kind == WEREWOLF_SNAPSHOT_LOBBY) {
            if (payload_len != WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE) {
                return WEREWOLF_MESSAGES_ERR_LENGTH;
            }
            decoded.body.snapshot.body.lobby.phase_epoch =
                get_u32_be(&payload[1]);
            decoded.body.snapshot.body.lobby.occupied_mask = payload[5];
            decoded.body.snapshot.body.lobby.ready_mask = payload[6];
            if (!decode_roster(&payload[7],
                               &decoded.body.snapshot.body.lobby.roster)) {
                return WEREWOLF_MESSAGES_ERR_VALUE;
            }
        } else if (decoded.body.snapshot.kind == WEREWOLF_SNAPSHOT_GAME) {
            if (payload_len != WEREWOLF_MESSAGES_SNAPSHOT_GAME_SIZE) {
                return WEREWOLF_MESSAGES_ERR_LENGTH;
            }
            if (payload[1u + WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE] > 1u) {
                return WEREWOLF_MESSAGES_ERR_VALUE;
            }
            decode_public_state(&payload[1],
                                &decoded.body.snapshot.body.game.public_state);
            decoded.body.snapshot.body.game.local_gate_acknowledged =
                payload[1u + WEREWOLF_MESSAGES_PUBLIC_STATE_SIZE] == 1u;
        } else if (decoded.body.snapshot.kind ==
                   WEREWOLF_SNAPSHOT_ROLE_REVEAL) {
            uint8_t player;

            if (payload_len != WEREWOLF_MESSAGES_SNAPSHOT_ROLE_REVEAL_SIZE) {
                return WEREWOLF_MESSAGES_ERR_LENGTH;
            }
            for (player = 0u; player < WW_PLAYER_COUNT; ++player) {
                decoded.body.snapshot.body.roles[player] =
                    (ww_role_t)payload[1u + player];
            }
        } else {
            return WEREWOLF_MESSAGES_ERR_ENUM;
        }
        break;
    case WEREWOLF_MSG_ABORT:
        if (payload_len != WEREWOLF_MESSAGES_ABORT_SIZE) {
            return WEREWOLF_MESSAGES_ERR_LENGTH;
        }
        decoded.body.abort.reason = (werewolf_abort_reason_t)payload[0];
        break;
    default:
        return WEREWOLF_MESSAGES_ERR_TYPE;
    }

    result = validate_message(&decoded, &expected_size);
    if (result != WEREWOLF_MESSAGES_OK) {
        return result;
    }
    if (expected_size != payload_len) {
        return WEREWOLF_MESSAGES_ERR_LENGTH;
    }
    *message = decoded;
    return WEREWOLF_MESSAGES_OK;
}

bool werewolf_messages_action_matches_gate(
    const werewolf_action_message_t *action,
    werewolf_gate_kind_t current_gate,
    uint32_t current_gate_epoch)
{
    return action != NULL && gate_kind_valid(current_gate) &&
           current_gate_epoch != 0u &&
           action->expected_gate_kind == current_gate &&
           action->expected_gate_epoch == current_gate_epoch;
}

bool werewolf_messages_private_matches_gate(
    const werewolf_private_role_message_t *private_message,
    werewolf_gate_kind_t current_gate,
    uint32_t current_gate_epoch)
{
    return private_message != NULL &&
           (current_gate == WEREWOLF_GATE_ROLE ||
            current_gate == WEREWOLF_GATE_PRIVATE_RESULT) &&
           current_gate_epoch != 0u &&
           private_message->gate_kind == current_gate &&
           private_message->gate_epoch == current_gate_epoch;
}

bool werewolf_messages_public_allows_role_reveal(
    const werewolf_public_state_message_t *public_state)
{
    return public_state != NULL &&
           public_state->phase == WW_PHASE_GAME_OVER &&
           public_state->gate_kind == WEREWOLF_GATE_NONE;
}

bool werewolf_messages_public_confirms_normal_action(
    werewolf_action_kind_t kind,
    uint32_t expected_phase_epoch,
    uint8_t actor,
    uint8_t speaker_at_send,
    const werewolf_public_state_message_t *public_state)
{
    int32_t phase_delta;

    if (public_state == NULL || expected_phase_epoch == 0u ||
        actor >= WW_PLAYER_COUNT ||
        (kind != WEREWOLF_ACTION_NIGHT_TARGET &&
         kind != WEREWOLF_ACTION_VOTE_TARGET &&
         kind != WEREWOLF_ACTION_PASS_SPEECH)) {
        return false;
    }
    phase_delta = (int32_t)(public_state->phase_epoch -
                            expected_phase_epoch);
    if (phase_delta > 0) {
        return true;
    }
    if (phase_delta < 0) {
        return false;
    }
    if (kind == WEREWOLF_ACTION_NIGHT_TARGET ||
        kind == WEREWOLF_ACTION_VOTE_TARGET) {
        return (public_state->submitted_mask &
                (ww_player_mask_t)(UINT8_C(1) << actor)) != 0u;
    }
    return speaker_at_send < WW_PLAYER_COUNT &&
           public_state->current_speaker != speaker_at_send;
}

const char *werewolf_messages_result_string(werewolf_messages_result_t result)
{
    switch (result) {
    case WEREWOLF_MESSAGES_OK:
        return "OK";
    case WEREWOLF_MESSAGES_ERR_ARGUMENT:
        return "ARGUMENT";
    case WEREWOLF_MESSAGES_ERR_CAPACITY:
        return "CAPACITY";
    case WEREWOLF_MESSAGES_ERR_LENGTH:
        return "LENGTH";
    case WEREWOLF_MESSAGES_ERR_TYPE:
        return "TYPE";
    case WEREWOLF_MESSAGES_ERR_VERSION:
        return "VERSION";
    case WEREWOLF_MESSAGES_ERR_ENUM:
        return "ENUM";
    case WEREWOLF_MESSAGES_ERR_MASK:
        return "MASK";
    case WEREWOLF_MESSAGES_ERR_VALUE:
        return "VALUE";
    default:
        return "UNKNOWN";
    }
}
