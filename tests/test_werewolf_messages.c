#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "werewolf_messages.h"

#define PAYLOAD_CAPACITY WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE

static void fill_nonzero(uint8_t *bytes, size_t length, uint8_t first)
{
    size_t index;

    for (index = 0; index < length; ++index) {
        bytes[index] = (uint8_t)(first + (uint8_t)index);
    }
}

static werewolf_public_state_message_t make_public_state(ww_phase_t phase)
{
    werewolf_public_state_message_t state = { 0 };

    state.phase = phase;
    state.winner = phase == WW_PHASE_GAME_OVER ? WW_WINNER_GOOD
                                                : WW_WINNER_NONE;
    state.phase_epoch = UINT32_C(0x11223344);
    state.round_number = UINT16_C(0x5566);
    state.occupied_mask = WW_ALL_PLAYERS_MASK;
    state.alive_mask = UINT8_C(0x5f);
    state.submitted_mask = UINT8_C(0x05);
    state.tie_mask = UINT8_C(0x12);
    state.current_speaker = phase == WW_PHASE_DISCUSSION ? 1u : WW_NO_PLAYER;
    state.dawn_victim = 5u;
    state.exiled_player = 6u;
    state.gate_kind = WEREWOLF_GATE_NONE;
    state.gate_epoch = UINT32_C(0x778899aa);
    return state;
}

static void fill_roster(werewolf_roster_message_t *roster,
                        ww_player_mask_t profile_mask,
                        uint32_t revision)
{
    static const char *const names[WW_PLAYER_COUNT] = {
        "Harvey", "GG Bond", "PLAYER3", "PLAYER4",
        "PLAYER5", "PLAYER6", "PLAYER7",
    };

    memset(roster, 0, sizeof(*roster));
    roster->lobby_revision = revision;
    roster->profile_mask = profile_mask;
    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        if ((profile_mask & (ww_player_mask_t)(UINT8_C(1) << seat)) != 0U) {
            (void)snprintf(roster->names[seat], sizeof(roster->names[seat]),
                           "%s", names[seat]);
        }
    }
}

static werewolf_message_t make_beacon(void)
{
    werewolf_message_t message = { 0 };

    message.type = WEREWOLF_MSG_BEACON;
    message.body.beacon.protocol_version = WEREWOLF_PROTOCOL_VERSION;
    message.body.beacon.rules_version = WEREWOLF_MESSAGES_RULES_VERSION;
    message.body.beacon.occupied_mask = UINT8_C(0x25);
    message.body.beacon.offered_seat = 4u;
    fill_nonzero(message.body.beacon.room_fingerprint,
                 WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE, 0x10u);
    fill_nonzero(message.body.beacon.host_commitment,
                 WEREWOLF_MESSAGES_COMMITMENT_SIZE, 0x40u);
    return message;
}

static werewolf_message_t make_join(void)
{
    werewolf_message_t message = { 0 };

    message.type = WEREWOLF_MSG_JOIN;
    message.body.join.candidate_seat = 4u;
    fill_nonzero(message.body.join.client_commitment,
                 WEREWOLF_MESSAGES_COMMITMENT_SIZE, 0x20u);
    return message;
}

static werewolf_message_t make_pair_host_reveal(void)
{
    werewolf_message_t message = { 0 };

    message.type = WEREWOLF_MSG_PAIR_HOST_REVEAL;
    message.body.pair_host_reveal.offered_seat = 4u;
    fill_nonzero(message.body.pair_host_reveal.host_public_key,
                 WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE, 0x60u);
    fill_nonzero(message.body.pair_host_reveal.host_nonce,
                 WEREWOLF_MESSAGES_NONCE_SIZE, 0x90u);
    fill_nonzero(message.body.pair_host_reveal.locked_client_commitment,
                 WEREWOLF_MESSAGES_COMMITMENT_SIZE, 0xb0u);
    return message;
}

static werewolf_message_t make_pair_client_reveal(void)
{
    werewolf_message_t message = { 0 };

    message.type = WEREWOLF_MSG_PAIR_CLIENT_REVEAL;
    message.body.pair_client_reveal.candidate_seat = 4u;
    fill_nonzero(message.body.pair_client_reveal.client_public_key,
                 WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE, 0x20u);
    fill_nonzero(message.body.pair_client_reveal.client_nonce,
                 WEREWOLF_MESSAGES_NONCE_SIZE, 0x80u);
    fill_nonzero(message.body.pair_client_reveal.echoed_host_commitment,
                 WEREWOLF_MESSAGES_COMMITMENT_SIZE, 0x40u);
    return message;
}

static werewolf_message_t make_start(void)
{
    werewolf_message_t message = { 0 };
    werewolf_public_state_message_t *state;

    message.type = WEREWOLF_MSG_START;
    state = &message.body.start.public_state;
    state->phase = WW_PHASE_NIGHT;
    state->winner = WW_WINNER_NONE;
    state->phase_epoch = UINT32_C(0x01020304);
    state->round_number = 1u;
    state->occupied_mask = WW_ALL_PLAYERS_MASK;
    state->alive_mask = WW_ALL_PLAYERS_MASK;
    state->current_speaker = WW_NO_PLAYER;
    state->dawn_victim = WW_NO_PLAYER;
    state->exiled_player = WW_NO_PLAYER;
    state->gate_kind = WEREWOLF_GATE_ROLE;
    state->gate_epoch = UINT32_C(0x05060708);
    fill_roster(&message.body.start.roster, WW_ALL_PLAYERS_MASK,
                UINT32_C(0x0a0b0c0d));
    return message;
}

static size_t encode_ok(const werewolf_message_t *message, uint8_t *payload)
{
    size_t payload_len = 0u;

    assert(werewolf_messages_encode(message, payload, PAYLOAD_CAPACITY,
                                     &payload_len) == WEREWOLF_MESSAGES_OK);
    assert(payload_len > 0u && payload_len <= PAYLOAD_CAPACITY);
    return payload_len;
}

static werewolf_message_t round_trip(const werewolf_message_t *message,
                                     size_t expected_len)
{
    uint8_t first[PAYLOAD_CAPACITY];
    uint8_t second[PAYLOAD_CAPACITY];
    werewolf_message_t decoded;
    size_t first_len = encode_ok(message, first);
    size_t second_len = 0u;

    assert(first_len == expected_len);
    assert(werewolf_messages_decode(message->type, first, first_len,
                                     &decoded) == WEREWOLF_MESSAGES_OK);
    assert(decoded.type == message->type);
    assert(werewolf_messages_encode(&decoded, second, sizeof(second),
                                     &second_len) == WEREWOLF_MESSAGES_OK);
    assert(second_len == first_len);
    assert(memcmp(first, second, first_len) == 0);
    return decoded;
}

static void expect_bad_lengths(const werewolf_message_t *message)
{
    uint8_t payload[PAYLOAD_CAPACITY + 1u];
    werewolf_message_t decoded;
    size_t payload_len = encode_ok(message, payload);

    assert(werewolf_messages_decode(message->type, payload, payload_len - 1u,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_LENGTH);
    payload[payload_len] = 0u;
    assert(werewolf_messages_decode(message->type, payload, payload_len + 1u,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_LENGTH);
}

static void expect_encode_error(const werewolf_message_t *message,
                                werewolf_messages_result_t expected)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    size_t payload_len = 123u;

    assert(werewolf_messages_encode(message, payload, sizeof(payload),
                                     &payload_len) == expected);
    assert(payload_len == 0u);
}

static void test_commit_reveal_messages(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t beacon = make_beacon();
    werewolf_message_t join = make_join();
    werewolf_message_t host_reveal = make_pair_host_reveal();
    werewolf_message_t client_reveal = make_pair_client_reveal();
    werewolf_message_t decoded;
    size_t payload_len;

    decoded = round_trip(&beacon, WEREWOLF_MESSAGES_BEACON_SIZE);
    assert(decoded.body.beacon.occupied_mask == UINT8_C(0x25));
    payload_len = encode_ok(&beacon, payload);
    assert(payload[0] == WEREWOLF_PROTOCOL_VERSION);
    assert(payload[1] == WEREWOLF_MESSAGES_RULES_VERSION);
    assert(payload[2] == UINT8_C(0x25));
    assert(payload[3] == 4u);
    assert(memcmp(&payload[4], beacon.body.beacon.room_fingerprint,
                  WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE) == 0);
    assert(memcmp(&payload[12], beacon.body.beacon.host_commitment,
                  WEREWOLF_MESSAGES_COMMITMENT_SIZE) == 0);
    assert(payload_len == 44u);
    expect_bad_lengths(&beacon);

    beacon.body.beacon.protocol_version++;
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_VERSION);
    beacon = make_beacon();
    beacon.body.beacon.rules_version++;
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_VERSION);
    beacon = make_beacon();
    beacon.body.beacon.occupied_mask = UINT8_C(0x80);
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_MASK);
    beacon = make_beacon();
    beacon.body.beacon.offered_seat = 0u;
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_VALUE);
    beacon = make_beacon();
    beacon.body.beacon.offered_seat = 5u;
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_VALUE);
    beacon = make_beacon();
    memset(beacon.body.beacon.room_fingerprint, 0,
           sizeof(beacon.body.beacon.room_fingerprint));
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_VALUE);
    beacon = make_beacon();
    memset(beacon.body.beacon.host_commitment, 0,
           sizeof(beacon.body.beacon.host_commitment));
    expect_encode_error(&beacon, WEREWOLF_MESSAGES_ERR_VALUE);

    decoded = round_trip(&join, WEREWOLF_MESSAGES_JOIN_SIZE);
    assert(decoded.body.join.candidate_seat == 4u);
    payload_len = encode_ok(&join, payload);
    assert(payload[0] == 4u);
    assert(memcmp(&payload[1], join.body.join.client_commitment,
                  WEREWOLF_MESSAGES_COMMITMENT_SIZE) == 0);
    assert(payload_len == 33u);
    expect_bad_lengths(&join);

    join.body.join.candidate_seat = 0u;
    expect_encode_error(&join, WEREWOLF_MESSAGES_ERR_VALUE);
    join = make_join();
    join.body.join.candidate_seat = WW_PLAYER_COUNT;
    expect_encode_error(&join, WEREWOLF_MESSAGES_ERR_VALUE);
    join = make_join();
    memset(join.body.join.client_commitment, 0,
           sizeof(join.body.join.client_commitment));
    expect_encode_error(&join, WEREWOLF_MESSAGES_ERR_VALUE);

    decoded = round_trip(&host_reveal,
                         WEREWOLF_MESSAGES_PAIR_HOST_REVEAL_SIZE);
    assert(decoded.body.pair_host_reveal.offered_seat == 4u);
    payload_len = encode_ok(&host_reveal, payload);
    assert(payload[0] == 4u);
    assert(memcmp(&payload[1], host_reveal.body.pair_host_reveal.host_public_key,
                  WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE) == 0);
    assert(memcmp(&payload[33], host_reveal.body.pair_host_reveal.host_nonce,
                  WEREWOLF_MESSAGES_NONCE_SIZE) == 0);
    assert(memcmp(&payload[49],
                  host_reveal.body.pair_host_reveal.locked_client_commitment,
                  WEREWOLF_MESSAGES_COMMITMENT_SIZE) == 0);
    assert(payload_len == 81u);
    expect_bad_lengths(&host_reveal);
    memset(host_reveal.body.pair_host_reveal.locked_client_commitment, 0,
           sizeof(host_reveal.body.pair_host_reveal.locked_client_commitment));
    expect_encode_error(&host_reveal, WEREWOLF_MESSAGES_ERR_VALUE);

    decoded = round_trip(&client_reveal,
                         WEREWOLF_MESSAGES_PAIR_CLIENT_REVEAL_SIZE);
    assert(decoded.body.pair_client_reveal.candidate_seat == 4u);
    payload_len = encode_ok(&client_reveal, payload);
    assert(payload[0] == 4u);
    assert(memcmp(&payload[1],
                  client_reveal.body.pair_client_reveal.client_public_key,
                  WEREWOLF_MESSAGES_X25519_PUBLIC_KEY_SIZE) == 0);
    assert(memcmp(&payload[33],
                  client_reveal.body.pair_client_reveal.client_nonce,
                  WEREWOLF_MESSAGES_NONCE_SIZE) == 0);
    assert(memcmp(
               &payload[49],
               client_reveal.body.pair_client_reveal.echoed_host_commitment,
               WEREWOLF_MESSAGES_COMMITMENT_SIZE) == 0);
    assert(payload_len == 81u);
    expect_bad_lengths(&client_reveal);
    memset(client_reveal.body.pair_client_reveal.client_nonce, 0,
           sizeof(client_reveal.body.pair_client_reveal.client_nonce));
    expect_encode_error(&client_reveal, WEREWOLF_MESSAGES_ERR_VALUE);
    client_reveal = make_pair_client_reveal();
    memset(client_reveal.body.pair_client_reveal.echoed_host_commitment, 0,
           sizeof(client_reveal.body.pair_client_reveal
                      .echoed_host_commitment));
    expect_encode_error(&client_reveal, WEREWOLF_MESSAGES_ERR_VALUE);
}

static void test_accept_ready_and_abort(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t message = { 0 };
    werewolf_message_t decoded;
    size_t payload_len;

    message.type = WEREWOLF_MSG_ACCEPT;
    message.body.accept.seat = 6u;
    decoded = round_trip(&message, WEREWOLF_MESSAGES_ACCEPT_SIZE);
    assert(decoded.body.accept.seat == 6u);
    payload_len = encode_ok(&message, payload);
    assert(payload_len == 1u && payload[0] == 6u);
    expect_bad_lengths(&message);
    message.body.accept.seat = 0u;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_READY;
    message.body.ready.ready = true;
    decoded = round_trip(&message, WEREWOLF_MESSAGES_READY_SIZE);
    assert(decoded.body.ready.ready);
    payload_len = encode_ok(&message, payload);
    assert(payload_len == 1u && payload[0] == 1u);
    payload[0] = 2u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_READY, payload, 1u,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_VALUE);
    expect_bad_lengths(&message);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_PROFILE;
    (void)snprintf(message.body.profile.nickname,
                   sizeof(message.body.profile.nickname), "GG Bond");
    decoded = round_trip(&message, WEREWOLF_MESSAGES_PROFILE_SIZE);
    assert(strcmp(decoded.body.profile.nickname, "GG Bond") == 0);
    payload_len = encode_ok(&message, payload);
    assert(payload_len == 11U && payload[0] == 7U);
    assert(memcmp(&payload[1], "GG Bond", 7U) == 0);
    assert(payload[8] == 0U && payload[10] == 0U);
    expect_bad_lengths(&message);
    (void)snprintf(message.body.profile.nickname,
                   sizeof(message.body.profile.nickname), "ABCDEFGHIJ");
    decoded = round_trip(&message, WEREWOLF_MESSAGES_PROFILE_SIZE);
    assert(strcmp(decoded.body.profile.nickname, "ABCDEFGHIJ") == 0);
    message.body.profile.nickname[0] = '\0';
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    (void)snprintf(message.body.profile.nickname,
                   sizeof(message.body.profile.nickname), "Harvey");
    payload_len = encode_ok(&message, payload);
    payload[10] = 1U;
    assert(werewolf_messages_decode(WEREWOLF_MSG_PROFILE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);
    payload_len = encode_ok(&message, payload);
    payload[0] = 0U;
    assert(werewolf_messages_decode(WEREWOLF_MSG_PROFILE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);
    payload[0] = 11U;
    assert(werewolf_messages_decode(WEREWOLF_MSG_PROFILE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);
    memset(payload, 0, payload_len);
    payload[0] = 3U;
    payload[1] = (uint8_t)'A';
    payload[2] = (uint8_t)'\n';
    payload[3] = (uint8_t)'B';
    assert(werewolf_messages_decode(WEREWOLF_MSG_PROFILE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);
    memset(payload, 0, payload_len);
    payload[0] = 4U;
    memcpy(&payload[1], " BAD", 4U);
    assert(werewolf_messages_decode(WEREWOLF_MSG_PROFILE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_ABORT;
    message.body.abort.reason = WEREWOLF_ABORT_HOST_LOST;
    decoded = round_trip(&message, WEREWOLF_MESSAGES_ABORT_SIZE);
    assert(decoded.body.abort.reason == WEREWOLF_ABORT_HOST_LOST);
    expect_bad_lengths(&message);
    message.body.abort.reason = (werewolf_abort_reason_t)0;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);
    message.body.abort.reason = WEREWOLF_ABORT_HOST_CLOSED_ROOM;
    decoded = round_trip(&message, WEREWOLF_MESSAGES_ABORT_SIZE);
    assert(decoded.body.abort.reason == WEREWOLF_ABORT_HOST_CLOSED_ROOM);
    message.body.abort.reason = WEREWOLF_ABORT_KICKED_BY_HOST;
    decoded = round_trip(&message, WEREWOLF_MESSAGES_ABORT_SIZE);
    assert(decoded.body.abort.reason == WEREWOLF_ABORT_KICKED_BY_HOST);
    message.body.abort.reason =
        (werewolf_abort_reason_t)(WEREWOLF_ABORT_KICKED_BY_HOST + 1);
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);
}

static void test_start_phase_and_byte_order(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t start = make_start();
    werewolf_message_t phase = { 0 };
    werewolf_message_t decoded;
    werewolf_public_state_message_t *state;
    size_t payload_len;

    decoded = round_trip(&start, WEREWOLF_MESSAGES_START_SIZE);
    assert(decoded.body.start.public_state.phase_epoch ==
           UINT32_C(0x01020304));
    assert(decoded.body.start.roster.lobby_revision ==
           UINT32_C(0x0a0b0c0d));
    assert(strcmp(decoded.body.start.roster.names[1], "GG Bond") == 0);
    payload_len = encode_ok(&start, payload);
    assert(payload_len == WEREWOLF_MESSAGES_START_SIZE);
    assert(payload[0] == WW_PHASE_NIGHT && payload[1] == WW_WINNER_NONE);
    assert(memcmp(&payload[2], (uint8_t[]){1u, 2u, 3u, 4u}, 4u) == 0);
    assert(payload[6] == 0u && payload[7] == 1u);
    assert(payload[8] == WW_ALL_PLAYERS_MASK);
    assert(payload[15] == WEREWOLF_GATE_ROLE);
    assert(memcmp(&payload[16], (uint8_t[]){5u, 6u, 7u, 8u}, 4u) == 0);
    assert(memcmp(&payload[20],
                  (uint8_t[]){0x0au, 0x0bu, 0x0cu, 0x0du}, 4u) == 0);
    assert(payload[24] == WW_ALL_PLAYERS_MASK);
    expect_bad_lengths(&start);

    start.body.start.public_state.phase = WW_PHASE_DAWN_RESULT;
    expect_encode_error(&start, WEREWOLF_MESSAGES_ERR_VALUE);
    start = make_start();
    start.body.start.public_state.round_number = 2u;
    expect_encode_error(&start, WEREWOLF_MESSAGES_ERR_VALUE);
    start = make_start();
    start.body.start.public_state.alive_mask = UINT8_C(0x3f);
    expect_encode_error(&start, WEREWOLF_MESSAGES_ERR_VALUE);
    start = make_start();
    start.body.start.roster.profile_mask &= (uint8_t)~UINT8_C(0x40);
    start.body.start.roster.names[6][0] = '\0';
    expect_encode_error(&start, WEREWOLF_MESSAGES_ERR_MASK);

    phase.type = WEREWOLF_MSG_PHASE;
    phase.body.phase.public_state = make_public_state(WW_PHASE_DISCUSSION);
    decoded = round_trip(&phase, WEREWOLF_MESSAGES_PHASE_SIZE);
    state = &decoded.body.phase.public_state;
    assert(state->phase == WW_PHASE_DISCUSSION);
    assert(state->phase_epoch == UINT32_C(0x11223344));
    assert(state->round_number == UINT16_C(0x5566));
    assert(state->current_speaker == 1u);
    payload_len = encode_ok(&phase, payload);
    assert(memcmp(&payload[2],
                  (uint8_t[]){0x11u, 0x22u, 0x33u, 0x44u}, 4u) == 0);
    assert(payload[6] == 0x55u && payload[7] == 0x66u);
    assert(payload[12] == 1u && payload[13] == 5u && payload[14] == 6u);
    assert(payload[15] == WEREWOLF_GATE_NONE);
    assert(memcmp(&payload[16],
                  (uint8_t[]){0x77u, 0x88u, 0x99u, 0xaau}, 4u) == 0);
    expect_bad_lengths(&phase);

    phase.body.phase.public_state =
        make_public_state(WW_PHASE_TIE_DEFENSE);
    phase.body.phase.public_state.current_speaker = 1u;
    decoded = round_trip(&phase, WEREWOLF_MESSAGES_PHASE_SIZE);
    assert(decoded.body.phase.public_state.current_speaker == 1u);
    phase.body.phase.public_state.current_speaker = 2u;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);

    phase.body.phase.public_state.phase = (ww_phase_t)0x7f;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_ENUM);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.winner = (ww_winner_t)0x7f;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_ENUM);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.phase_epoch = 0u;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.round_number = 0u;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.occupied_mask = UINT8_C(0x3f);
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_MASK);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.alive_mask = 0u;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_MASK);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.submitted_mask = UINT8_C(0x20);
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_MASK);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.tie_mask = UINT8_C(0x80);
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_MASK);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.tie_mask = UINT8_C(0x20);
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_MASK);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.current_speaker = 1u;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_DISCUSSION);
    phase.body.phase.public_state.current_speaker = WW_PLAYER_COUNT;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_GAME_OVER);
    phase.body.phase.public_state.winner = WW_WINNER_NONE;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.winner = WW_WINNER_GOOD;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.gate_epoch = 0u;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_DISCUSSION);
    phase.body.phase.public_state.gate_kind = WEREWOLF_GATE_ROLE;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_VALUE);
    phase.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    phase.body.phase.public_state.gate_kind =
        (werewolf_gate_kind_t)0x7f;
    expect_encode_error(&phase, WEREWOLF_MESSAGES_ERR_ENUM);
}

static void test_private_role(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t message = { 0 };
    werewolf_message_t decoded;
    size_t payload_len;

    message.type = WEREWOLF_MSG_PRIVATE_ROLE;
    message.body.private_role.role = WW_ROLE_WOLF;
    message.body.private_role.wolf_teammate_mask = UINT8_C(1) << 4;
    message.body.private_role.seer_result_seat = WW_NO_PLAYER;
    message.body.private_role.seer_result_faction = WW_CAMP_UNKNOWN;
    message.body.private_role.gate_kind = WEREWOLF_GATE_ROLE;
    message.body.private_role.gate_epoch = UINT32_C(0x01020304);
    decoded = round_trip(&message, WEREWOLF_MESSAGES_PRIVATE_ROLE_SIZE);
    assert(decoded.body.private_role.role == WW_ROLE_WOLF);
    assert(decoded.body.private_role.wolf_teammate_mask ==
           (UINT8_C(1) << 4));
    assert(decoded.body.private_role.gate_kind == WEREWOLF_GATE_ROLE);
    assert(decoded.body.private_role.gate_epoch == UINT32_C(0x01020304));
    assert(werewolf_messages_private_matches_gate(
        &decoded.body.private_role, WEREWOLF_GATE_ROLE,
        UINT32_C(0x01020304)));
    assert(!werewolf_messages_private_matches_gate(
        &decoded.body.private_role, WEREWOLF_GATE_ROLE,
        UINT32_C(0x01020305)));
    assert(!werewolf_messages_private_matches_gate(
        &decoded.body.private_role, WEREWOLF_GATE_PRIVATE_RESULT,
        UINT32_C(0x01020304)));
    assert(!werewolf_messages_private_matches_gate(
        NULL, WEREWOLF_GATE_ROLE, UINT32_C(0x01020304)));
    payload_len = encode_ok(&message, payload);
    assert(payload_len == 9u);
    assert(memcmp(payload,
                  (uint8_t[]){WW_ROLE_WOLF, 0x10u, WW_NO_PLAYER,
                              WW_CAMP_UNKNOWN, WEREWOLF_GATE_ROLE,
                              1u, 2u, 3u, 4u}, 9u) == 0);
    expect_bad_lengths(&message);

    message.body.private_role.wolf_teammate_mask = 0u;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_MASK);
    message.body.private_role.wolf_teammate_mask = UINT8_C(0x03);
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_MASK);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_PRIVATE_ROLE;
    message.body.private_role.role = WW_ROLE_SEER;
    message.body.private_role.wolf_teammate_mask = 0u;
    message.body.private_role.seer_result_seat = 5u;
    message.body.private_role.seer_result_faction = WW_CAMP_GOOD;
    message.body.private_role.gate_kind = WEREWOLF_GATE_PRIVATE_RESULT;
    message.body.private_role.gate_epoch = UINT32_C(0xa1b2c3d4);
    decoded = round_trip(&message, WEREWOLF_MESSAGES_PRIVATE_ROLE_SIZE);
    assert(decoded.body.private_role.seer_result_seat == 5u);
    assert(decoded.body.private_role.seer_result_faction == WW_CAMP_GOOD);
    assert(decoded.body.private_role.gate_kind ==
           WEREWOLF_GATE_PRIVATE_RESULT);
    payload_len = encode_ok(&message, payload);
    assert(payload[4] == WEREWOLF_GATE_PRIVATE_RESULT);
    assert(memcmp(&payload[5],
                  (uint8_t[]){0xa1u, 0xb2u, 0xc3u, 0xd4u}, 4u) == 0);
    message.body.private_role.gate_kind = WEREWOLF_GATE_ROLE;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.private_role.gate_kind = WEREWOLF_GATE_PRIVATE_RESULT;
    message.body.private_role.seer_result_faction = WW_CAMP_UNKNOWN;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.private_role.seer_result_faction = WW_CAMP_WOLF;
    message.body.private_role.seer_result_seat = WW_NO_PLAYER;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.private_role.seer_result_faction = (ww_camp_t)0x7f;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);

    message.body.private_role.role = WW_ROLE_GUARD;
    message.body.private_role.seer_result_seat = 2u;
    message.body.private_role.seer_result_faction = WW_CAMP_GOOD;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.private_role.role = WW_ROLE_NONE;
    message.body.private_role.seer_result_seat = WW_NO_PLAYER;
    message.body.private_role.seer_result_faction = WW_CAMP_UNKNOWN;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);
    message.body.private_role.role = WW_ROLE_VILLAGER;
    message.body.private_role.gate_kind = WEREWOLF_GATE_NONE;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.private_role.gate_kind = (werewolf_gate_kind_t)0x7f;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);
    message.body.private_role.gate_kind = WEREWOLF_GATE_PRIVATE_RESULT;
    message.body.private_role.gate_epoch = 0u;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
}

static void test_actions(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    const werewolf_action_kind_t kinds[] = {
        WEREWOLF_ACTION_NIGHT_TARGET,
        WEREWOLF_ACTION_PASS_SPEECH,
        WEREWOLF_ACTION_VOTE_TARGET,
        WEREWOLF_ACTION_ROLE_SEEN,
        WEREWOLF_ACTION_ACK_RESULT,
        WEREWOLF_ACTION_LEAVE_GAME,
    };
    size_t index;

    for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
        werewolf_message_t message = { 0 };
        werewolf_message_t decoded;
        size_t payload_len;
        bool has_target = kinds[index] == WEREWOLF_ACTION_NIGHT_TARGET ||
                          kinds[index] == WEREWOLF_ACTION_VOTE_TARGET;
        werewolf_gate_kind_t gate = WEREWOLF_GATE_NONE;

        if (kinds[index] == WEREWOLF_ACTION_ROLE_SEEN) {
            gate = WEREWOLF_GATE_ROLE;
        } else if (kinds[index] == WEREWOLF_ACTION_ACK_RESULT) {
            gate = WEREWOLF_GATE_DAWN;
        }

        message.type = WEREWOLF_MSG_ACTION;
        message.body.action.kind = kinds[index];
        message.body.action.target = has_target ? 3u : WW_NO_PLAYER;
        message.body.action.expected_phase_epoch = UINT32_C(0xa1b2c3d4);
        message.body.action.expected_gate_kind = gate;
        message.body.action.expected_gate_epoch = UINT32_C(0x01020304);
        decoded = round_trip(&message, WEREWOLF_MESSAGES_ACTION_SIZE);
        assert(decoded.body.action.kind == kinds[index]);
        assert(decoded.body.action.target == (has_target ? 3u : WW_NO_PLAYER));
        payload_len = encode_ok(&message, payload);
        assert(decoded.body.action.expected_gate_kind == gate);
        assert(decoded.body.action.expected_gate_epoch == UINT32_C(0x01020304));
        assert(payload_len == 11u);
        assert(payload[0] == (uint8_t)kinds[index]);
        assert(payload[1] == (has_target ? 3u : WW_NO_PLAYER));
        assert(memcmp(&payload[2],
                      (uint8_t[]){0xa1u, 0xb2u, 0xc3u, 0xd4u}, 4u) == 0);
        assert(payload[6] == (uint8_t)gate);
        assert(memcmp(&payload[7],
                      (uint8_t[]){1u, 2u, 3u, 4u}, 4u) == 0);
        assert(werewolf_messages_action_matches_gate(
            &decoded.body.action, gate, UINT32_C(0x01020304)));
        assert(!werewolf_messages_action_matches_gate(
            &decoded.body.action, gate, UINT32_C(0x01020305)));
    }

    werewolf_message_t invalid = { 0 };
    invalid.type = WEREWOLF_MSG_ACTION;
    invalid.body.action.kind = (werewolf_action_kind_t)0;
    invalid.body.action.target = WW_NO_PLAYER;
    invalid.body.action.expected_phase_epoch = 1u;
    invalid.body.action.expected_gate_kind = WEREWOLF_GATE_NONE;
    invalid.body.action.expected_gate_epoch = 1u;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_ENUM);
    invalid.body.action.kind = WEREWOLF_ACTION_NIGHT_TARGET;
    invalid.body.action.target = WW_NO_PLAYER;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_VALUE);
    invalid.body.action.kind = WEREWOLF_ACTION_ROLE_SEEN;
    invalid.body.action.target = 1u;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_VALUE);
    invalid.body.action.target = WW_NO_PLAYER;
    invalid.body.action.expected_phase_epoch = 0u;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_VALUE);
    invalid.body.action.expected_phase_epoch = 1u;
    invalid.body.action.expected_gate_kind = WEREWOLF_GATE_ROLE;
    expect_bad_lengths(&invalid);
    invalid.body.action.expected_gate_kind = WEREWOLF_GATE_NONE;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_VALUE);
    invalid.body.action.expected_gate_kind = WEREWOLF_GATE_ROLE;
    invalid.body.action.expected_gate_epoch = 0u;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_VALUE);
    invalid.body.action.expected_gate_epoch = 1u;
    invalid.body.action.kind = WEREWOLF_ACTION_ACK_RESULT;
    expect_encode_error(&invalid, WEREWOLF_MESSAGES_ERR_VALUE);
    invalid.body.action.expected_gate_kind = WEREWOLF_GATE_PRIVATE_RESULT;
    assert(!werewolf_messages_action_matches_gate(
        &invalid.body.action, WEREWOLF_GATE_DAWN, 1u));
    assert(werewolf_messages_action_matches_gate(
        &invalid.body.action, WEREWOLF_GATE_PRIVATE_RESULT, 1u));
    assert(!werewolf_messages_action_matches_gate(
        NULL, WEREWOLF_GATE_PRIVATE_RESULT, 1u));
}

static void test_snapshots(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t message = { 0 };
    werewolf_message_t decoded;
    size_t payload_len;

    message.type = WEREWOLF_MSG_SNAPSHOT;
    message.body.snapshot.kind = WEREWOLF_SNAPSHOT_LOBBY;
    message.body.snapshot.body.lobby.phase_epoch = UINT32_C(0x01020304);
    message.body.snapshot.body.lobby.occupied_mask = UINT8_C(0x17);
    message.body.snapshot.body.lobby.ready_mask = UINT8_C(0x05);
    fill_roster(&message.body.snapshot.body.lobby.roster,
                UINT8_C(0x17), UINT32_C(0x10203040));
    decoded = round_trip(&message, WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE);
    assert(decoded.body.snapshot.body.lobby.ready_mask == UINT8_C(0x05));
    assert(strcmp(decoded.body.snapshot.body.lobby.roster.names[1],
                  "GG Bond") == 0);
    payload_len = encode_ok(&message, payload);
    assert(payload_len == WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE);
    assert(memcmp(payload,
                  (uint8_t[]){WEREWOLF_SNAPSHOT_LOBBY, 1u, 2u, 3u, 4u,
                              0x17u, 0x05u}, 7u) == 0);
    assert(memcmp(&payload[7],
                  (uint8_t[]){0x10u, 0x20u, 0x30u, 0x40u}, 4u) == 0);
    expect_bad_lengths(&message);

    message.body.snapshot.body.lobby.roster.lobby_revision = 0U;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.snapshot.body.lobby.roster.lobby_revision =
        UINT32_C(0x10203040);
    message.body.snapshot.body.lobby.roster.profile_mask |= UINT8_C(0x80);
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_MASK);
    message.body.snapshot.body.lobby.roster.profile_mask = UINT8_C(0x17);
    payload_len = encode_ok(&message, payload);
    payload[12U + 3U * WEREWOLF_MESSAGES_NICKNAME_SLOT_SIZE] = 1U;
    assert(werewolf_messages_decode(WEREWOLF_MSG_SNAPSHOT, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);

    message.body.snapshot.body.lobby.phase_epoch = 0u;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.snapshot.body.lobby.phase_epoch = 1u;
    message.body.snapshot.body.lobby.occupied_mask = UINT8_C(0x02);
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_MASK);
    message.body.snapshot.body.lobby.occupied_mask = UINT8_C(0x03);
    message.body.snapshot.body.lobby.ready_mask = UINT8_C(0x04);
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_MASK);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_SNAPSHOT;
    message.body.snapshot.kind = WEREWOLF_SNAPSHOT_GAME;
    message.body.snapshot.body.game.public_state =
        make_public_state(WW_PHASE_VOTE);
    message.body.snapshot.body.game.local_gate_acknowledged = true;
    decoded = round_trip(&message, WEREWOLF_MESSAGES_SNAPSHOT_GAME_SIZE);
    assert(decoded.body.snapshot.body.game.public_state.phase ==
           WW_PHASE_VOTE);
    assert(decoded.body.snapshot.body.game.local_gate_acknowledged);
    payload_len = encode_ok(&message, payload);
    assert(payload_len == 22u && payload[0] == WEREWOLF_SNAPSHOT_GAME);
    assert(payload[21] == 1u);
    expect_bad_lengths(&message);

    message.body.snapshot.body.game.public_state.phase_epoch = 0u;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.snapshot.body.game.public_state =
        make_public_state(WW_PHASE_VOTE);
    payload_len = encode_ok(&message, payload);
    payload[21] = 2u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_SNAPSHOT, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_SNAPSHOT;
    message.body.snapshot.kind = WEREWOLF_SNAPSHOT_ROLE_REVEAL;
    message.body.snapshot.body.roles[0] = WW_ROLE_WOLF;
    message.body.snapshot.body.roles[1] = WW_ROLE_SEER;
    message.body.snapshot.body.roles[2] = WW_ROLE_VILLAGER;
    message.body.snapshot.body.roles[3] = WW_ROLE_WOLF;
    message.body.snapshot.body.roles[4] = WW_ROLE_GUARD;
    message.body.snapshot.body.roles[5] = WW_ROLE_VILLAGER;
    message.body.snapshot.body.roles[6] = WW_ROLE_VILLAGER;
    decoded = round_trip(&message,
                         WEREWOLF_MESSAGES_SNAPSHOT_ROLE_REVEAL_SIZE);
    assert(decoded.body.snapshot.body.roles[0] == WW_ROLE_WOLF);
    assert(decoded.body.snapshot.body.roles[4] == WW_ROLE_GUARD);
    payload_len = encode_ok(&message, payload);
    assert(payload_len == 8u);
    assert(memcmp(payload,
                  (uint8_t[]){WEREWOLF_SNAPSHOT_ROLE_REVEAL,
                              WW_ROLE_WOLF, WW_ROLE_SEER, WW_ROLE_VILLAGER,
                              WW_ROLE_WOLF, WW_ROLE_GUARD, WW_ROLE_VILLAGER,
                              WW_ROLE_VILLAGER}, 8u) == 0);
    expect_bad_lengths(&message);

    message.body.snapshot.body.roles[6] = WW_ROLE_SEER;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_VALUE);
    message.body.snapshot.body.roles[6] = WW_ROLE_NONE;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);

    message.body.snapshot.body.roles[6] = WW_ROLE_VILLAGER;
    payload_len = encode_ok(&message, payload);
    payload[7] = WW_ROLE_WOLF;
    assert(werewolf_messages_decode(WEREWOLF_MSG_SNAPSHOT, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);
    payload[7] = WW_ROLE_NONE;
    assert(werewolf_messages_decode(WEREWOLF_MSG_SNAPSHOT, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_ENUM);

    message.body.snapshot.kind = (werewolf_snapshot_kind_t)0;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_ENUM);
    payload[0] = 0u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_SNAPSHOT, payload, 1u,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_ENUM);
}

static void test_role_reveal_policy(void)
{
    werewolf_public_state_message_t state =
        make_public_state(WW_PHASE_GAME_OVER);

    assert(werewolf_messages_public_allows_role_reveal(&state));
    state.gate_kind = WEREWOLF_GATE_EXILE;
    assert(!werewolf_messages_public_allows_role_reveal(&state));
    state.gate_kind = WEREWOLF_GATE_NONE;
    state.phase = WW_PHASE_EXILE_RESULT;
    assert(!werewolf_messages_public_allows_role_reveal(&state));
    assert(!werewolf_messages_public_allows_role_reveal(NULL));
}

static void test_normal_action_snapshot_policy(void)
{
    werewolf_public_state_message_t state =
        make_public_state(WW_PHASE_NIGHT);
    uint32_t sent_epoch = state.phase_epoch;

    state.submitted_mask = 0u;
    assert(!werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_NIGHT_TARGET, sent_epoch, 2u, WW_NO_PLAYER,
        &state));
    state.submitted_mask = (ww_player_mask_t)(UINT8_C(1) << 2);
    assert(werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_NIGHT_TARGET, sent_epoch, 2u, WW_NO_PLAYER,
        &state));
    state.submitted_mask = 0u;
    ++state.phase_epoch;
    assert(werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_VOTE_TARGET, sent_epoch, 2u, WW_NO_PLAYER,
        &state));

    state = make_public_state(WW_PHASE_DISCUSSION);
    sent_epoch = state.phase_epoch;
    assert(!werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_PASS_SPEECH, sent_epoch, 1u, 1u, &state));
    state.current_speaker = 2u;
    assert(werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_PASS_SPEECH, sent_epoch, 1u, 1u, &state));
    assert(!werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_ACK_RESULT, sent_epoch, 1u, 1u, &state));
    assert(!werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_PASS_SPEECH, sent_epoch, WW_PLAYER_COUNT, 1u,
        &state));
    assert(!werewolf_messages_public_confirms_normal_action(
        WEREWOLF_ACTION_PASS_SPEECH, sent_epoch, 1u, 1u, NULL));
}

static void test_api_failures_and_decode_validation(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t message = make_beacon();
    werewolf_message_t decoded;
    size_t payload_len = 99u;
    size_t valid_len;

    assert(werewolf_messages_encode(NULL, payload, sizeof(payload),
                                     &payload_len) ==
           WEREWOLF_MESSAGES_ERR_ARGUMENT);
    assert(payload_len == 0u);
    assert(werewolf_messages_encode(&message, NULL, sizeof(payload),
                                     &payload_len) ==
           WEREWOLF_MESSAGES_ERR_ARGUMENT);
    assert(werewolf_messages_encode(&message, payload, sizeof(payload),
                                     NULL) == WEREWOLF_MESSAGES_ERR_ARGUMENT);
    valid_len = encode_ok(&message, payload);
    assert(werewolf_messages_encode(&message, payload, valid_len - 1u,
                                     &payload_len) ==
           WEREWOLF_MESSAGES_ERR_CAPACITY);
    assert(payload_len == 0u);

    assert(werewolf_messages_decode(message.type, NULL, valid_len,
                                     &decoded) ==
           WEREWOLF_MESSAGES_ERR_ARGUMENT);
    assert(werewolf_messages_decode(message.type, payload, valid_len,
                                     NULL) ==
           WEREWOLF_MESSAGES_ERR_ARGUMENT);
    decoded.type = WEREWOLF_MSG_ABORT;
    assert(werewolf_messages_decode(message.type, payload, valid_len - 1u,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_LENGTH);
    assert((int)decoded.type == 0);

    message.type = WEREWOLF_MSG_ACK;
    expect_encode_error(&message, WEREWOLF_MESSAGES_ERR_TYPE);
    assert(werewolf_messages_decode(WEREWOLF_MSG_RESUME, payload, valid_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_TYPE);

    message = make_beacon();
    valid_len = encode_ok(&message, payload);
    payload[0] = (uint8_t)(WEREWOLF_PROTOCOL_VERSION + 1u);
    assert(werewolf_messages_decode(WEREWOLF_MSG_BEACON, payload, valid_len,
                                     &decoded) ==
           WEREWOLF_MESSAGES_ERR_VERSION);
    message = make_join();
    valid_len = encode_ok(&message, payload);
    payload[0] = 0u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_JOIN, payload, valid_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_VALUE);
}

static void test_decoder_rejects_invalid_fields(void)
{
    uint8_t payload[PAYLOAD_CAPACITY];
    werewolf_message_t message = { 0 };
    werewolf_message_t decoded;
    size_t payload_len;

    message.type = WEREWOLF_MSG_ACCEPT;
    message.body.accept.seat = 1u;
    payload_len = encode_ok(&message, payload);
    payload[0] = 0u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_ACCEPT, payload, payload_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_VALUE);

    message = make_start();
    payload_len = encode_ok(&message, payload);
    payload[0] = WW_PHASE_DAWN_RESULT;
    assert(werewolf_messages_decode(WEREWOLF_MSG_START, payload, payload_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_VALUE);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_PHASE;
    message.body.phase.public_state = make_public_state(WW_PHASE_NIGHT);
    payload_len = encode_ok(&message, payload);
    payload[9] = UINT8_C(0x80);
    assert(werewolf_messages_decode(WEREWOLF_MSG_PHASE, payload, payload_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_MASK);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_PRIVATE_ROLE;
    message.body.private_role.role = WW_ROLE_VILLAGER;
    message.body.private_role.seer_result_seat = WW_NO_PLAYER;
    message.body.private_role.seer_result_faction = WW_CAMP_UNKNOWN;
    message.body.private_role.gate_kind = WEREWOLF_GATE_ROLE;
    message.body.private_role.gate_epoch = 4u;
    payload_len = encode_ok(&message, payload);
    payload[0] = WW_ROLE_NONE;
    assert(werewolf_messages_decode(WEREWOLF_MSG_PRIVATE_ROLE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_ENUM);
    payload_len = encode_ok(&message, payload);
    payload[4] = WEREWOLF_GATE_DAWN;
    assert(werewolf_messages_decode(WEREWOLF_MSG_PRIVATE_ROLE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);
    payload_len = encode_ok(&message, payload);
    memset(&payload[5], 0, 4u);
    assert(werewolf_messages_decode(WEREWOLF_MSG_PRIVATE_ROLE, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_VALUE);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_ACTION;
    message.body.action.kind = WEREWOLF_ACTION_ACK_RESULT;
    message.body.action.target = WW_NO_PLAYER;
    message.body.action.expected_phase_epoch = 7u;
    message.body.action.expected_gate_kind = WEREWOLF_GATE_DAWN;
    message.body.action.expected_gate_epoch = 9u;
    payload_len = encode_ok(&message, payload);
    payload[0] = 0u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_ACTION, payload, payload_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_ENUM);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_SNAPSHOT;
    message.body.snapshot.kind = WEREWOLF_SNAPSHOT_LOBBY;
    message.body.snapshot.body.lobby.phase_epoch = 2u;
    message.body.snapshot.body.lobby.occupied_mask = UINT8_C(0x03);
    message.body.snapshot.body.lobby.ready_mask = UINT8_C(0x01);
    fill_roster(&message.body.snapshot.body.lobby.roster,
                UINT8_C(0x03), 2U);
    payload_len = encode_ok(&message, payload);
    payload[6] = UINT8_C(0x04);
    assert(werewolf_messages_decode(WEREWOLF_MSG_SNAPSHOT, payload,
                                     payload_len, &decoded) ==
           WEREWOLF_MESSAGES_ERR_MASK);

    message = (werewolf_message_t){ 0 };
    message.type = WEREWOLF_MSG_ABORT;
    message.body.abort.reason = WEREWOLF_ABORT_PROTOCOL_ERROR;
    payload_len = encode_ok(&message, payload);
    payload[0] = 0u;
    assert(werewolf_messages_decode(WEREWOLF_MSG_ABORT, payload, payload_len,
                                     &decoded) == WEREWOLF_MESSAGES_ERR_ENUM);
}

int main(void)
{
    assert(WEREWOLF_MESSAGES_RULES_VERSION == 5U);
    assert(WEREWOLF_MESSAGES_PROFILE_SIZE == 11U);
    assert(WEREWOLF_MESSAGES_START_SIZE <
           WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE);
    assert(WEREWOLF_MESSAGES_SNAPSHOT_LOBBY_SIZE <
           WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE);
    test_commit_reveal_messages();
    test_accept_ready_and_abort();
    test_start_phase_and_byte_order();
    test_private_role();
    test_actions();
    test_snapshots();
    test_role_reveal_policy();
    test_normal_action_snapshot_policy();
    test_api_failures_and_decode_validation();
    test_decoder_rejects_invalid_fields();
    assert(strcmp(werewolf_messages_result_string(WEREWOLF_MESSAGES_ERR_MASK),
                  "MASK") == 0);
    assert(strcmp(werewolf_messages_result_string(
                      (werewolf_messages_result_t)-99),
                  "UNKNOWN") == 0);
    puts("werewolf_messages tests passed");
    return 0;
}
