#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "werewolf_room_directory.h"

static void make_mac(uint8_t mac[WEREWOLF_NET_MAC_SIZE], uint8_t id)
{
    const uint8_t value[WEREWOLF_NET_MAC_SIZE] = {
        UINT8_C(0x02), UINT8_C(0x10), UINT8_C(0x20),
        UINT8_C(0x30), UINT8_C(0x40), id,
    };

    memcpy(mac, value, sizeof(value));
}

static werewolf_beacon_message_t make_beacon(uint8_t fingerprint,
                                              uint8_t occupied_mask,
                                              uint8_t offered_seat)
{
    werewolf_beacon_message_t beacon = {
        .protocol_version = WEREWOLF_PROTOCOL_VERSION,
        .rules_version = WEREWOLF_MESSAGES_RULES_VERSION,
        .occupied_mask = occupied_mask,
        .offered_seat = offered_seat,
    };

    for (size_t i = 0U; i < sizeof(beacon.room_fingerprint); ++i) {
        beacon.room_fingerprint[i] = (uint8_t)(fingerprint + (uint8_t)i);
    }
    for (size_t i = 0U; i < sizeof(beacon.host_commitment); ++i) {
        beacon.host_commitment[i] =
            (uint8_t)(fingerprint ^ (uint8_t)(i + 1U));
    }
    return beacon;
}

static uint32_t observe_room(werewolf_room_directory_t *directory,
                             uint8_t host_id, uint64_t session_id,
                             uint32_t epoch, uint8_t fingerprint,
                             uint32_t now_ms, uint32_t selected_token,
                             werewolf_room_directory_result_t expected)
{
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    werewolf_beacon_message_t beacon =
        make_beacon(fingerprint, UINT8_C(0x01), UINT8_C(1));
    uint32_t token = WEREWOLF_ROOM_TOKEN_NONE;

    make_mac(mac, host_id);
    assert(werewolf_room_directory_observe(
               directory, mac, session_id, epoch, &beacon, now_ms,
               selected_token, &token) == expected);
    assert(token != WEREWOLF_ROOM_TOKEN_NONE);
    return token;
}

static void test_add_refresh_and_update(void)
{
    werewolf_room_directory_t directory;
    werewolf_room_candidate_t candidate;
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    werewolf_beacon_message_t beacon =
        make_beacon(UINT8_C(0x20), UINT8_C(0x01), UINT8_C(1));
    uint32_t token = WEREWOLF_ROOM_TOKEN_NONE;
    uint32_t refreshed_token = WEREWOLF_ROOM_TOKEN_NONE;

    werewolf_room_directory_init(&directory);
    make_mac(mac, UINT8_C(1));
    assert(werewolf_room_directory_count(&directory) == 0U);
    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(10), UINT32_C(20), &beacon,
               UINT32_C(100), WEREWOLF_ROOM_TOKEN_NONE, &token) ==
           WEREWOLF_ROOM_DIRECTORY_ADDED);
    assert(token != WEREWOLF_ROOM_TOKEN_NONE);
    assert(werewolf_room_directory_count(&directory) == 1U);

    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(10), UINT32_C(20), &beacon,
               UINT32_C(200), token, &refreshed_token) ==
           WEREWOLF_ROOM_DIRECTORY_REFRESHED);
    assert(refreshed_token == token);
    assert(werewolf_room_directory_find(&directory, token, &candidate));
    assert(candidate.last_seen_ms == UINT32_C(200));

    beacon.occupied_mask = UINT8_C(0x03);
    beacon.offered_seat = UINT8_C(2);
    beacon.room_fingerprint[0] ^= UINT8_C(0x80);
    beacon.host_commitment[0] ^= UINT8_C(0xff);
    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(10), UINT32_C(20), &beacon,
               UINT32_C(300), token, &refreshed_token) ==
           WEREWOLF_ROOM_DIRECTORY_UPDATED);
    assert(refreshed_token == token);
    assert(werewolf_room_directory_find(&directory, token, &candidate));
    assert(candidate.beacon.occupied_mask == UINT8_C(0x03));
    assert(candidate.beacon.offered_seat == UINT8_C(2));
    assert(candidate.beacon.room_fingerprint[0] ==
           beacon.room_fingerprint[0]);
    assert(candidate.beacon.host_commitment[0] ==
           beacon.host_commitment[0]);
    assert(candidate.session_id == UINT64_C(10));
    assert(candidate.epoch == UINT32_C(20));
    assert(memcmp(candidate.host_mac, mac, sizeof(mac)) == 0);
}

static void test_same_host_new_session_replaces(void)
{
    werewolf_room_directory_t directory;
    werewolf_room_candidate_t candidate;
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    werewolf_beacon_message_t beacon =
        make_beacon(UINT8_C(0x30), UINT8_C(0x01), UINT8_C(1));
    uint32_t old_token;
    uint32_t new_token = WEREWOLF_ROOM_TOKEN_NONE;

    werewolf_room_directory_init(&directory);
    make_mac(mac, UINT8_C(2));
    old_token = observe_room(&directory, UINT8_C(2), UINT64_C(1),
                             UINT32_C(1), UINT8_C(0x10), UINT32_C(0),
                             WEREWOLF_ROOM_TOKEN_NONE,
                             WEREWOLF_ROOM_DIRECTORY_ADDED);
    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(2), UINT32_C(2), &beacon,
               UINT32_C(500), old_token, &new_token) ==
           WEREWOLF_ROOM_DIRECTORY_REPLACED);
    assert(new_token != WEREWOLF_ROOM_TOKEN_NONE);
    assert(new_token != old_token);
    assert(werewolf_room_directory_count(&directory) == 1U);
    assert(!werewolf_room_directory_find(&directory, old_token, &candidate));
    assert(werewolf_room_directory_find(&directory, new_token, &candidate));
    assert(candidate.session_id == UINT64_C(2));
    assert(candidate.epoch == UINT32_C(2));
    assert(candidate.beacon.room_fingerprint[0] == UINT8_C(0x30));

    old_token = new_token;
    beacon = make_beacon(UINT8_C(0x31), UINT8_C(0x01), UINT8_C(1));
    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(2), UINT32_C(3), &beacon,
               UINT32_C(600), old_token, &new_token) ==
           WEREWOLF_ROOM_DIRECTORY_REPLACED);
    assert(new_token != old_token);
    assert(werewolf_room_directory_count(&directory) == 1U);
}

static void test_stable_sorted_snapshot(void)
{
    werewolf_room_directory_t directory;
    werewolf_room_candidate_t sorted[WEREWOLF_ROOM_DIRECTORY_CAPACITY];

    werewolf_room_directory_init(&directory);
    (void)observe_room(&directory, UINT8_C(3), UINT64_C(3), UINT32_C(1),
                       UINT8_C(0x30), UINT32_C(100),
                       WEREWOLF_ROOM_TOKEN_NONE,
                       WEREWOLF_ROOM_DIRECTORY_ADDED);
    (void)observe_room(&directory, UINT8_C(1), UINT64_C(1), UINT32_C(1),
                       UINT8_C(0x10), UINT32_C(100),
                       WEREWOLF_ROOM_TOKEN_NONE,
                       WEREWOLF_ROOM_DIRECTORY_ADDED);
    (void)observe_room(&directory, UINT8_C(2), UINT64_C(2), UINT32_C(1),
                       UINT8_C(0x20), UINT32_C(100),
                       WEREWOLF_ROOM_TOKEN_NONE,
                       WEREWOLF_ROOM_DIRECTORY_ADDED);
    (void)observe_room(&directory, UINT8_C(4), UINT64_C(4), UINT32_C(1),
                       UINT8_C(0x20), UINT32_C(100),
                       WEREWOLF_ROOM_TOKEN_NONE,
                       WEREWOLF_ROOM_DIRECTORY_ADDED);

    assert(werewolf_room_directory_snapshot_sorted(
               &directory, sorted, WEREWOLF_ROOM_DIRECTORY_CAPACITY) == 4U);
    assert(sorted[0].beacon.room_fingerprint[0] == UINT8_C(0x10));
    assert(sorted[1].beacon.room_fingerprint[0] == UINT8_C(0x20));
    assert(sorted[1].host_mac[WEREWOLF_NET_MAC_SIZE - 1U] == UINT8_C(2));
    assert(sorted[2].beacon.room_fingerprint[0] == UINT8_C(0x20));
    assert(sorted[2].host_mac[WEREWOLF_NET_MAC_SIZE - 1U] == UINT8_C(4));
    assert(sorted[3].beacon.room_fingerprint[0] == UINT8_C(0x30));
    assert(werewolf_room_directory_snapshot_sorted(
               &directory, sorted, 2U) == 2U);
    assert(sorted[0].beacon.room_fingerprint[0] == UINT8_C(0x10));
    assert(sorted[1].beacon.room_fingerprint[0] == UINT8_C(0x20));
}

static void test_expiry_is_wrap_safe(void)
{
    werewolf_room_directory_t directory;
    werewolf_room_candidate_t candidate;
    const uint32_t observed_ms = UINT32_MAX - UINT32_C(999);
    uint32_t token;

    werewolf_room_directory_init(&directory);
    token = observe_room(&directory, UINT8_C(1), UINT64_C(1), UINT32_C(1),
                         UINT8_C(0x10), observed_ms,
                         WEREWOLF_ROOM_TOKEN_NONE,
                         WEREWOLF_ROOM_DIRECTORY_ADDED);
    assert(werewolf_room_directory_find_fresh(
        &directory, token, UINT32_C(999), &candidate));
    assert(werewolf_room_directory_expire(&directory, UINT32_C(999)) == 0U);
    assert(!werewolf_room_directory_find_fresh(
        &directory, token, UINT32_C(1000), &candidate));
    assert(werewolf_room_directory_expire(&directory, UINT32_C(1000)) == 1U);
    assert(werewolf_room_directory_count(&directory) == 0U);
}

static void test_full_directory_protects_selection(void)
{
    werewolf_room_directory_t directory;
    werewolf_room_candidate_t candidate;
    uint32_t tokens[WEREWOLF_ROOM_DIRECTORY_CAPACITY];
    uint32_t added_token;

    werewolf_room_directory_init(&directory);
    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        tokens[i] = observe_room(
            &directory, (uint8_t)(i + 1U), (uint64_t)(i + 1U),
            UINT32_C(1), (uint8_t)(UINT8_C(0x10) + (uint8_t)i),
            (uint32_t)(i * 100U), WEREWOLF_ROOM_TOKEN_NONE,
            WEREWOLF_ROOM_DIRECTORY_ADDED);
    }
    assert(werewolf_room_directory_count(&directory) ==
           WEREWOLF_ROOM_DIRECTORY_CAPACITY);

    added_token = observe_room(
        &directory, UINT8_C(20), UINT64_C(20), UINT32_C(1), UINT8_C(0x40),
        UINT32_C(800), tokens[0], WEREWOLF_ROOM_DIRECTORY_EVICTED);
    assert(werewolf_room_directory_find(&directory, tokens[0], &candidate));
    assert(!werewolf_room_directory_find(&directory, tokens[1], &candidate));
    assert(werewolf_room_directory_find(&directory, added_token, &candidate));
    assert(werewolf_room_directory_count(&directory) ==
           WEREWOLF_ROOM_DIRECTORY_CAPACITY);

    added_token = observe_room(
        &directory, UINT8_C(21), UINT64_C(21), UINT32_C(1), UINT8_C(0x41),
        UINT32_C(900), WEREWOLF_ROOM_TOKEN_NONE,
        WEREWOLF_ROOM_DIRECTORY_EVICTED);
    assert(!werewolf_room_directory_find(&directory, tokens[0], &candidate));
    assert(werewolf_room_directory_find(&directory, added_token, &candidate));
}

static void test_rejects_invalid_discovery_data(void)
{
    werewolf_room_directory_t directory;
    uint8_t mac[WEREWOLF_NET_MAC_SIZE] = { 0 };
    werewolf_beacon_message_t beacon =
        make_beacon(UINT8_C(0x10), UINT8_C(0x01), UINT8_C(1));
    uint32_t token = UINT32_C(123);

    werewolf_room_directory_init(&directory);
    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(1), UINT32_C(1), &beacon,
               UINT32_C(0), WEREWOLF_ROOM_TOKEN_NONE, &token) ==
           WEREWOLF_ROOM_DIRECTORY_INVALID);
    assert(token == WEREWOLF_ROOM_TOKEN_NONE);
    make_mac(mac, UINT8_C(1));
    beacon.room_fingerprint[0] = 0U;
    memset(beacon.room_fingerprint, 0, sizeof(beacon.room_fingerprint));
    assert(werewolf_room_directory_observe(
               &directory, mac, UINT64_C(1), UINT32_C(1), &beacon,
               UINT32_C(0), WEREWOLF_ROOM_TOKEN_NONE, &token) ==
           WEREWOLF_ROOM_DIRECTORY_INVALID);
    assert(werewolf_room_directory_count(&directory) == 0U);
}

static void test_token_allocation_skips_zero_after_wrap(void)
{
    werewolf_room_directory_t directory;
    uint32_t token;

    werewolf_room_directory_init(&directory);
    directory.next_token = UINT32_MAX;
    token = observe_room(&directory, UINT8_C(1), UINT64_C(1), UINT32_C(1),
                         UINT8_C(0x10), UINT32_C(0),
                         WEREWOLF_ROOM_TOKEN_NONE,
                         WEREWOLF_ROOM_DIRECTORY_ADDED);
    assert(token == UINT32_C(1));
}

int main(void)
{
    test_add_refresh_and_update();
    test_same_host_new_session_replaces();
    test_stable_sorted_snapshot();
    test_expiry_is_wrap_safe();
    test_full_directory_protects_selection();
    test_rejects_invalid_discovery_data();
    test_token_allocation_skips_zero_after_wrap();
    return 0;
}
