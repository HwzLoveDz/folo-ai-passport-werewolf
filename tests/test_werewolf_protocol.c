#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "werewolf_net.h"
#include "werewolf_protocol.h"

#define TEST_SESSION UINT64_C(0x0102030405060708)
#define TEST_EPOCH   UINT32_C(0x11223344)

static werewolf_frame_t make_frame(werewolf_message_type_t type,
                                   uint8_t src,
                                   uint8_t dst,
                                   uint32_t seq,
                                   uint16_t phase)
{
    werewolf_frame_t frame = {0};
    frame.version = WEREWOLF_PROTOCOL_VERSION;
    frame.type = type;
    frame.src = src;
    frame.dst = dst;
    frame.session_id = TEST_SESSION;
    frame.epoch = TEST_EPOCH;
    frame.msg_seq = seq;
    frame.phase_seq = phase;
    if (type == WEREWOLF_MSG_ACTION) {
        frame.action_key = 1;
    }
    if (type == WEREWOLF_MSG_ACK) {
        frame.flags = WEREWOLF_FLAG_IS_ACK;
        frame.ack_seq = 1;
    }
    return frame;
}

static void rewrite_crc(uint8_t *wire, size_t wire_len)
{
    uint16_t crc = werewolf_protocol_crc16(
        wire, wire_len - WEREWOLF_PROTOCOL_CRC_SIZE);
    wire[wire_len - 2] = (uint8_t)(crc >> 8);
    wire[wire_len - 1] = (uint8_t)crc;
}

static size_t encode_ok(const werewolf_frame_t *frame, uint8_t *wire)
{
    size_t wire_len = 0;
    assert(werewolf_protocol_encode(frame, wire,
                                     WEREWOLF_PROTOCOL_MAX_FRAME_SIZE,
                                     &wire_len) == WEREWOLF_PROTOCOL_OK);
    assert(wire_len <= WEREWOLF_PROTOCOL_MAX_FRAME_SIZE);
    return wire_len;
}

static void test_round_trip_and_byte_order(void)
{
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    werewolf_frame_t frame = make_frame(WEREWOLF_MSG_ACTION, 2, 1,
                                        UINT32_C(0xA1B2C3D4), 0x5566);
    werewolf_frame_t decoded;
    size_t wire_len;

    frame.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    frame.ack_seq = UINT32_C(0x91A2B3C4);
    frame.action_key = UINT32_C(0xDEADBEEF);
    frame.payload_len = 4;
    memcpy(frame.payload, "vote", 4);
    wire_len = encode_ok(&frame, wire);

    assert(wire_len == WEREWOLF_PROTOCOL_HEADER_SIZE + 4 +
                           WEREWOLF_PROTOCOL_CRC_SIZE);
    assert(wire[0] == 0x57 && wire[1] == 0x57);
    assert(wire[2] == WEREWOLF_PROTOCOL_VERSION);
    assert(wire[3] == WEREWOLF_PROTOCOL_HEADER_SIZE);
    assert(wire[4] == WEREWOLF_MSG_ACTION);
    assert(wire[5] == WEREWOLF_FLAG_ACK_REQUIRED);
    assert(wire[6] == 2 && wire[7] == 1);
    assert(memcmp(&wire[8],
                  (uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8}, 8) == 0);
    assert(memcmp(&wire[16],
                  (uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4) == 0);
    assert(memcmp(&wire[20],
                  (uint8_t[]){0xA1, 0xB2, 0xC3, 0xD4}, 4) == 0);
    assert(memcmp(&wire[24],
                  (uint8_t[]){0x91, 0xA2, 0xB3, 0xC4}, 4) == 0);
    assert(wire[28] == 0x55 && wire[29] == 0x66);
    assert(wire[30] == 0 && wire[31] == 4);
    assert(memcmp(&wire[32],
                  (uint8_t[]){0xDE, 0xAD, 0xBE, 0xEF}, 4) == 0);
    assert(memcmp(&wire[WEREWOLF_PROTOCOL_HEADER_SIZE], "vote", 4) == 0);

    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_OK);
    assert(decoded.version == frame.version);
    assert(decoded.type == frame.type);
    assert(decoded.flags == frame.flags);
    assert(decoded.src == frame.src && decoded.dst == frame.dst);
    assert(decoded.session_id == frame.session_id);
    assert(decoded.epoch == frame.epoch);
    assert(decoded.msg_seq == frame.msg_seq);
    assert(decoded.ack_seq == frame.ack_seq);
    assert(decoded.phase_seq == frame.phase_seq);
    assert(decoded.action_key == frame.action_key);
    assert(decoded.payload_len == frame.payload_len);
    assert(memcmp(decoded.payload, frame.payload, frame.payload_len) == 0);
}

static void test_all_message_types_and_maximum_size(void)
{
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    werewolf_frame_t decoded;

    for (int type = WEREWOLF_MSG_BEACON;
         type <= WEREWOLF_MSG_PROFILE; ++type) {
        bool discovery = werewolf_protocol_type_is_discovery(
            (werewolf_message_type_t)type);
        werewolf_frame_t frame = make_frame(
            (werewolf_message_type_t)type, 1,
            discovery ? WEREWOLF_PLAYER_BROADCAST : 2,
            (uint32_t)type, 0);
        size_t wire_len = encode_ok(&frame, wire);
        assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
               WEREWOLF_PROTOCOL_OK);
        assert(decoded.type == (werewolf_message_type_t)type);
    }
    assert(werewolf_protocol_type_is_discovery(
        WEREWOLF_MSG_PAIR_HOST_REVEAL));
    assert(werewolf_protocol_type_is_discovery(
        WEREWOLF_MSG_PAIR_CLIENT_REVEAL));
    assert(!werewolf_protocol_type_requires_encryption(
        WEREWOLF_MSG_PAIR_HOST_REVEAL));
    assert(werewolf_protocol_type_requires_encryption(WEREWOLF_MSG_ACCEPT));
    assert(!werewolf_protocol_type_is_discovery(WEREWOLF_MSG_PROFILE));
    assert(werewolf_protocol_type_requires_encryption(WEREWOLF_MSG_PROFILE));
    assert(!werewolf_protocol_type_is_phase_bound(WEREWOLF_MSG_PROFILE));

    werewolf_frame_t maximum = make_frame(WEREWOLF_MSG_SNAPSHOT, 1, 2, 99, 7);
    maximum.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    maximum.payload_len = WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE;
    for (size_t i = 0; i < maximum.payload_len; ++i) {
        maximum.payload[i] = (uint8_t)i;
    }
    size_t maximum_len = encode_ok(&maximum, wire);
    assert(maximum_len == WEREWOLF_PROTOCOL_MAX_FRAME_SIZE);
    assert(werewolf_protocol_decode(wire, maximum_len, &decoded) ==
           WEREWOLF_PROTOCOL_OK);
    assert(decoded.payload_len == WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE);
    assert(memcmp(decoded.payload, maximum.payload, maximum.payload_len) == 0);

    maximum.payload_len = WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE + 1U;
    size_t ignored_len = 123;
    assert(werewolf_protocol_encode(&maximum, wire, sizeof(wire),
                                     &ignored_len) ==
           WEREWOLF_PROTOCOL_ERR_LENGTH);
    assert(ignored_len == 0);
}

static void test_malformed_frames(void)
{
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE + 1];
    uint8_t original[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    werewolf_frame_t decoded;
    werewolf_frame_t frame = make_frame(WEREWOLF_MSG_PHASE, 1, 2, 10, 3);
    size_t wire_len;

    frame.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    frame.payload_len = 3;
    memcpy(frame.payload, "day", 3);
    wire_len = encode_ok(&frame, wire);
    memcpy(original, wire, wire_len);

    assert(werewolf_protocol_decode(wire, 1, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_LENGTH);
    assert(werewolf_protocol_decode(wire,
                                     WEREWOLF_PROTOCOL_MAX_FRAME_SIZE + 1,
                                     &decoded) ==
           WEREWOLF_PROTOCOL_ERR_LENGTH);

    wire[0] ^= 1;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_MAGIC);
    memcpy(wire, original, wire_len);

    wire[2] = WEREWOLF_PROTOCOL_VERSION + 1;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_VERSION);
    memcpy(wire, original, wire_len);

    wire[2] = WEREWOLF_PROTOCOL_VERSION - 1U;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_VERSION);
    memcpy(wire, original, wire_len);

    wire[3] = WEREWOLF_PROTOCOL_HEADER_SIZE - 1;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_LENGTH);
    memcpy(wire, original, wire_len);

    wire[4] = 0x7F;
    rewrite_crc(wire, wire_len);
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_TYPE);
    memcpy(wire, original, wire_len);

    wire[5] = 0x80;
    rewrite_crc(wire, wire_len);
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_FLAGS);
    memcpy(wire, original, wire_len);

    wire[7] = WEREWOLF_PLAYER_BROADCAST;
    rewrite_crc(wire, wire_len);
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_POLICY);
    memcpy(wire, original, wire_len);

    wire[30] = 0;
    wire[31] = 4;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_LENGTH);
    memcpy(wire, original, wire_len);

    wire[WEREWOLF_PROTOCOL_HEADER_SIZE] ^= 0x01;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_CRC);
    memcpy(wire, original, wire_len);

    memcpy(&wire[wire_len], "x", 1);
    assert(werewolf_protocol_decode(wire, wire_len + 1, &decoded) ==
           WEREWOLF_PROTOCOL_ERR_LENGTH);

    size_t ignored_len = 0;
    werewolf_frame_t invalid_ack = make_frame(WEREWOLF_MSG_ACK, 2, 1, 20, 1);
    invalid_ack.flags = 0;
    assert(werewolf_protocol_encode(&invalid_ack, wire, sizeof(wire),
                                     &ignored_len) ==
           WEREWOLF_PROTOCOL_ERR_POLICY);

    werewolf_frame_t invalid_join = make_frame(
        WEREWOLF_MSG_JOIN, 2, WEREWOLF_PLAYER_BROADCAST, 21, 0);
    invalid_join.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    assert(werewolf_protocol_encode(&invalid_join, wire, sizeof(wire),
                                     &ignored_len) ==
           WEREWOLF_PROTOCOL_ERR_POLICY);

    werewolf_frame_t invalid_reveal = make_frame(
        WEREWOLF_MSG_PAIR_HOST_REVEAL, 0, 2, 22, 0);
    assert(werewolf_protocol_encode(&invalid_reveal, wire, sizeof(wire),
                                    &ignored_len) ==
           WEREWOLF_PROTOCOL_ERR_POLICY);

    werewolf_frame_t invalid_action = make_frame(WEREWOLF_MSG_ACTION, 2, 1,
                                                  23, 2);
    invalid_action.action_key = 0;
    assert(werewolf_protocol_encode(&invalid_action, wire, sizeof(wire),
                                     &ignored_len) ==
           WEREWOLF_PROTOCOL_ERR_POLICY);
}

static void test_sequence_window_and_idempotency(void)
{
    werewolf_reliable_t reliable;
    werewolf_frame_t frame = make_frame(WEREWOLF_MSG_PHASE, 2, 1, 10, 5);

    werewolf_reliable_init(&reliable, 1, TEST_SESSION, TEST_EPOCH,
                            100, 100, 2);
    werewolf_reliable_set_phase(&reliable, 5);
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACCEPT);
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_DUPLICATE);

    frame.msg_seq = 12;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACCEPT);
    frame.msg_seq = 11;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACCEPT_REORDERED);
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_DUPLICATE);

    frame.msg_seq = 100;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACCEPT);
    frame.msg_seq = 60;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_LATE);

    frame = make_frame(WEREWOLF_MSG_ACTION, 2, 1, 101, 4);
    frame.action_key = 0xAA;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_LATE);

    werewolf_reliable_init(&reliable, 1, TEST_SESSION, TEST_EPOCH,
                            100, 100, 2);
    werewolf_reliable_set_phase(&reliable, 5);
    frame = make_frame(WEREWOLF_MSG_ACTION, 2, 1, 20, 5);
    frame.action_key = 0x1234;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACCEPT);
    frame.msg_seq = 21;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACTION_DUPLICATE);
    frame.msg_seq = 22;
    frame.action_key = 0x1235;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_ACCEPT);

    frame.session_id++;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_WRONG_SESSION);
    frame.session_id = TEST_SESSION;
    frame.epoch++;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_WRONG_EPOCH);
    frame.epoch = TEST_EPOCH;
    frame.dst = 9;
    assert(werewolf_reliable_observe(&reliable, &frame) ==
           WEREWOLF_RX_WRONG_DESTINATION);
}

static void test_ack_retry_and_exhaustion(void)
{
    werewolf_reliable_t reliable;
    werewolf_frame_t outbound = make_frame(WEREWOLF_MSG_ACTION, 1, 2, 50, 5);
    werewolf_frame_t received = make_frame(WEREWOLF_MSG_ACTION, 2, 1, 70, 5);
    werewolf_frame_t ack;
    werewolf_retry_item_t retry;
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    size_t wire_len;

    werewolf_reliable_init(&reliable, 1, TEST_SESSION, TEST_EPOCH,
                            100, 100, 2);
    outbound.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    outbound.action_key = 0x50;
    wire_len = encode_ok(&outbound, wire);
    assert(werewolf_reliable_track(&reliable, 2, 7, &outbound, wire,
                                    wire_len, 0) == WEREWOLF_NET_OK);
    assert(werewolf_reliable_pending_count(&reliable) == 1);
    assert(werewolf_reliable_pending_count_for_peer(&reliable, 2) == 1U);
    assert(werewolf_reliable_pending_count_for_peer(&reliable, 3) == 0U);
    assert(werewolf_reliable_delivery_status(&reliable, 2, 7, 50) ==
           WEREWOLF_DELIVERY_PENDING);
    assert(werewolf_reliable_delivery_status(&reliable, 2, 8, 50) ==
           WEREWOLF_DELIVERY_UNKNOWN);
    assert(werewolf_reliable_poll(&reliable, 99, &retry) ==
           WEREWOLF_RETRY_NONE);
    assert(werewolf_reliable_poll(&reliable, 100, &retry) ==
           WEREWOLF_RETRY_SEND);
    assert(retry.peer_id == 2 && retry.peer_generation == 7 &&
           retry.msg_seq == 50 && retry.attempt == 2);
    assert(retry.wire_len == wire_len);
    assert(memcmp(retry.wire, wire, wire_len) == 0);
    assert(!werewolf_reliable_ack(&reliable, 3, 50));
    assert(werewolf_reliable_ack(&reliable, 2, 50));
    assert(werewolf_reliable_pending_count(&reliable) == 0);
    assert(werewolf_reliable_delivery_status(&reliable, 2, 7, 50) ==
           WEREWOLF_DELIVERY_ACKNOWLEDGED);

    received.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    received.action_key = 0x70;
    assert(werewolf_reliable_make_ack(&reliable, &received, &ack) ==
           WEREWOLF_NET_OK);
    assert(ack.type == WEREWOLF_MSG_ACK);
    assert(ack.flags == WEREWOLF_FLAG_IS_ACK);
    assert(ack.src == 1 && ack.dst == 2);
    assert(ack.msg_seq == 100 && ack.ack_seq == 70);
    wire_len = encode_ok(&ack, wire);
    werewolf_frame_t decoded_ack;
    assert(werewolf_protocol_decode(wire, wire_len, &decoded_ack) ==
           WEREWOLF_PROTOCOL_OK);

    outbound.msg_seq = 51;
    outbound.action_key = 0x51;
    wire_len = encode_ok(&outbound, wire);
    assert(werewolf_reliable_track(&reliable, 2, 7, &outbound, wire,
                                    wire_len, 0) == WEREWOLF_NET_OK);
    assert(werewolf_reliable_poll(&reliable, 100, &retry) ==
           WEREWOLF_RETRY_SEND);
    assert(retry.attempt == 2);
    assert(werewolf_reliable_poll(&reliable, 300, &retry) ==
           WEREWOLF_RETRY_SEND);
    assert(retry.attempt == 3);
    assert(werewolf_reliable_poll(&reliable, 700, &retry) ==
           WEREWOLF_RETRY_EXHAUSTED);
    assert(retry.peer_id == 2 && retry.msg_seq == 51);
    assert(werewolf_reliable_pending_count(&reliable) == 0);
    assert(werewolf_reliable_delivery_status(&reliable, 2, 7, 51) ==
           WEREWOLF_DELIVERY_FAILED);

    /* A GAME_OVER heartbeat may enqueue a fresh encrypted ROLE_REVEAL after
     * the first frame exhausted.  Exhaustion must not poison that peer. */
    outbound.type = WEREWOLF_MSG_SNAPSHOT;
    outbound.msg_seq = 52;
    outbound.action_key = 0;
    outbound.payload_len = 1;
    outbound.payload[0] = 3; /* WEREWOLF_SNAPSHOT_ROLE_REVEAL on app wire. */
    wire_len = encode_ok(&outbound, wire);
    assert(werewolf_reliable_track(&reliable, 2, 7, &outbound, wire,
                                    wire_len, 800) == WEREWOLF_NET_OK);
    assert(werewolf_reliable_pending_count(&reliable) == 1);
    assert(werewolf_reliable_ack(&reliable, 2, 52));
    assert(werewolf_reliable_pending_count(&reliable) == 0);
}

static void test_initial_transport_failure_keeps_reliable_retry(void)
{
    werewolf_reliable_t reliable;
    werewolf_frame_t outbound = make_frame(WEREWOLF_MSG_SNAPSHOT, 1, 2,
                                            52, 5);
    werewolf_retry_item_t retry;
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    size_t wire_len;

    werewolf_reliable_init(&reliable, 1, TEST_SESSION, TEST_EPOCH,
                           100, 100, 2);
    outbound.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    wire_len = encode_ok(&outbound, wire);

    /* werewolf_net_send_unicast() tracks before calling esp_now_send().
     * Simulate that first hardware call failing by leaving the tracked frame
     * unacknowledged: the retry queue must retain the authoritative frame. */
    assert(werewolf_reliable_track(&reliable, 2, 7, &outbound, wire,
                                   wire_len, 1000) == WEREWOLF_NET_OK);
    assert(werewolf_reliable_pending_count(&reliable) == 1);
    assert(werewolf_reliable_poll(&reliable, 1099, &retry) ==
           WEREWOLF_RETRY_NONE);
    assert(werewolf_reliable_poll(&reliable, 1100, &retry) ==
           WEREWOLF_RETRY_SEND);
    assert(retry.peer_id == 2 && retry.peer_generation == 7 &&
           retry.msg_seq == 52 && retry.attempt == 2);
    assert(retry.wire_len == wire_len);
    assert(memcmp(retry.wire, wire, wire_len) == 0);

    /* A successful retry is completed by the normal authenticated ACK path. */
    assert(werewolf_reliable_ack(&reliable, 2, 52));
    assert(werewolf_reliable_pending_count(&reliable) == 0);
}

static void test_forget_peer_resets_security_association_state(void)
{
    werewolf_reliable_t reliable;
    werewolf_frame_t inbound_two = make_frame(
        WEREWOLF_MSG_ACTION, 2, 1, UINT32_C(0x80000000), 5);
    werewolf_frame_t inbound_three = make_frame(
        WEREWOLF_MSG_ACTION, 3, 1, 10, 5);
    werewolf_frame_t outbound_two = make_frame(
        WEREWOLF_MSG_ACTION, 1, 2, 50, 5);
    werewolf_frame_t outbound_three = make_frame(
        WEREWOLF_MSG_ACTION, 1, 3, 51, 5);
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    size_t wire_len;

    werewolf_reliable_init(&reliable, 1, TEST_SESSION, TEST_EPOCH,
                           100, 100, 2);
    werewolf_reliable_set_phase(&reliable, 5);
    inbound_two.action_key = UINT32_C(0x2222);
    inbound_three.action_key = UINT32_C(0x3333);
    assert(werewolf_reliable_observe(&reliable, &inbound_two) ==
           WEREWOLF_RX_ACCEPT);
    assert(werewolf_reliable_observe(&reliable, &inbound_three) ==
           WEREWOLF_RX_ACCEPT);

    outbound_two.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    outbound_two.action_key = UINT32_C(0x5050);
    wire_len = encode_ok(&outbound_two, wire);
    assert(werewolf_reliable_track(&reliable, 2, 21, &outbound_two, wire,
                                   wire_len, 0) == WEREWOLF_NET_OK);
    outbound_three.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    outbound_three.action_key = UINT32_C(0x5151);
    wire_len = encode_ok(&outbound_three, wire);
    assert(werewolf_reliable_track(&reliable, 3, 31, &outbound_three, wire,
                                   wire_len, 0) == WEREWOLF_NET_OK);
    assert(werewolf_reliable_pending_count(&reliable) == 2);
    assert(werewolf_reliable_pending_count_for_peer(&reliable, 2) == 1U);
    assert(werewolf_reliable_pending_count_for_peer(&reliable, 3) == 1U);

    werewolf_reliable_forget_peer(&reliable, 2);
    assert(werewolf_reliable_pending_count(&reliable) == 1);
    assert(werewolf_reliable_pending_count_for_peer(&reliable, 2) == 0U);
    assert(werewolf_reliable_pending_count_for_peer(&reliable, 3) == 1U);
    assert(werewolf_reliable_delivery_status(&reliable, 2, 21, 50) ==
           WEREWOLF_DELIVERY_UNKNOWN);
    assert(!werewolf_reliable_ack(&reliable, 2, 50));
    assert(werewolf_reliable_ack(&reliable, 3, 51));
    assert(werewolf_reliable_delivery_status(&reliable, 3, 31, 51) ==
           WEREWOLF_DELIVERY_ACKNOWLEDGED);

    /* A replacement using seat 2 gets a fresh sequence and action namespace. */
    inbound_two.msg_seq = 1;
    assert(werewolf_reliable_observe(&reliable, &inbound_two) ==
           WEREWOLF_RX_ACCEPT);

    /* Forgetting seat 2 must not weaken replay/idempotency state for seat 3. */
    inbound_three.msg_seq = 11;
    assert(werewolf_reliable_observe(&reliable, &inbound_three) ==
           WEREWOLF_RX_ACTION_DUPLICATE);
}

#ifndef ESP_PLATFORM
static void test_transport_pmk_boundary(void)
{
    uint8_t zero[WEREWOLF_NET_KEY_SIZE] = { 0 };
    uint8_t pmk[WEREWOLF_NET_KEY_SIZE] = { 1u };

    assert(werewolf_net_set_pmk(NULL, 0u) == WEREWOLF_NET_ERR_ARGUMENT);
    assert(werewolf_net_set_pmk(zero, sizeof(zero)) ==
           WEREWOLF_NET_ERR_SECURITY);
    assert(werewolf_net_set_pmk(pmk, sizeof(pmk)) ==
           WEREWOLF_NET_ERR_UNSUPPORTED);
}
#endif

int main(void)
{
    assert(WEREWOLF_PROTOCOL_VERSION == 5U);
    test_round_trip_and_byte_order();
    test_all_message_types_and_maximum_size();
    test_malformed_frames();
    test_sequence_window_and_idempotency();
    test_ack_retry_and_exhaustion();
    test_initial_transport_failure_keeps_reliable_retry();
    test_forget_peer_resets_security_association_state();
#ifndef ESP_PLATFORM
    test_transport_pmk_boundary();
#endif
    puts("werewolf protocol tests passed");
    return 0;
}
