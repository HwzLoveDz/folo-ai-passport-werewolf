#include "werewolf_room_directory.h"

#include <string.h>

static bool bytes_have_nonzero(const uint8_t *bytes, size_t size)
{
    uint8_t combined = 0U;

    if (bytes == NULL) {
        return false;
    }
    for (size_t i = 0U; i < size; ++i) {
        combined = (uint8_t)(combined | bytes[i]);
    }
    return combined != 0U;
}

static bool mac_valid(const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    static const uint8_t broadcast[WEREWOLF_NET_MAC_SIZE] = {
        UINT8_MAX, UINT8_MAX, UINT8_MAX,
        UINT8_MAX, UINT8_MAX, UINT8_MAX,
    };

    return mac != NULL && (mac[0] & UINT8_C(1)) == 0U &&
           bytes_have_nonzero(mac, WEREWOLF_NET_MAC_SIZE) &&
           memcmp(mac, broadcast, sizeof(broadcast)) != 0;
}

static bool beacon_valid(const werewolf_beacon_message_t *beacon)
{
    uint8_t offered_bit;

    if (beacon == NULL ||
        beacon->protocol_version != WEREWOLF_PROTOCOL_VERSION ||
        beacon->rules_version != WEREWOLF_MESSAGES_RULES_VERSION ||
        (beacon->occupied_mask & UINT8_C(1)) == 0U ||
        (beacon->occupied_mask & (uint8_t)~WW_ALL_PLAYERS_MASK) != 0U ||
        beacon->offered_seat == 0U ||
        beacon->offered_seat >= WW_PLAYER_COUNT) {
        return false;
    }
    offered_bit = (uint8_t)(UINT8_C(1) << beacon->offered_seat);
    return (beacon->occupied_mask & offered_bit) == 0U &&
           bytes_have_nonzero(beacon->room_fingerprint,
                              sizeof(beacon->room_fingerprint)) &&
           bytes_have_nonzero(beacon->host_commitment,
                              sizeof(beacon->host_commitment));
}

static bool same_identity(const werewolf_room_candidate_t *candidate,
                          const uint8_t host_mac[WEREWOLF_NET_MAC_SIZE],
                          uint64_t session_id, uint32_t epoch)
{
    return candidate->used && candidate->session_id == session_id &&
           candidate->epoch == epoch &&
           memcmp(candidate->host_mac, host_mac,
                  WEREWOLF_NET_MAC_SIZE) == 0;
}

static bool same_host(const werewolf_room_candidate_t *candidate,
                      const uint8_t host_mac[WEREWOLF_NET_MAC_SIZE])
{
    return candidate->used &&
           memcmp(candidate->host_mac, host_mac,
                  WEREWOLF_NET_MAC_SIZE) == 0;
}

static bool same_beacon(const werewolf_beacon_message_t *left,
                        const werewolf_beacon_message_t *right)
{
    return left->protocol_version == right->protocol_version &&
           left->rules_version == right->rules_version &&
           left->occupied_mask == right->occupied_mask &&
           left->offered_seat == right->offered_seat &&
           memcmp(left->room_fingerprint, right->room_fingerprint,
                  sizeof(left->room_fingerprint)) == 0 &&
           memcmp(left->host_commitment, right->host_commitment,
                  sizeof(left->host_commitment)) == 0;
}

static bool token_in_use(const werewolf_room_directory_t *directory,
                         uint32_t token)
{
    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        if (directory->entries[i].used &&
            directory->entries[i].token == token) {
            return true;
        }
    }
    return false;
}

static uint32_t allocate_token(werewolf_room_directory_t *directory)
{
    do {
        ++directory->next_token;
        if (directory->next_token == WEREWOLF_ROOM_TOKEN_NONE) {
            ++directory->next_token;
        }
    } while (token_in_use(directory, directory->next_token));
    return directory->next_token;
}

static void assign_candidate(werewolf_room_directory_t *directory,
                             werewolf_room_candidate_t *candidate,
                             const uint8_t host_mac[WEREWOLF_NET_MAC_SIZE],
                             uint64_t session_id, uint32_t epoch,
                             const werewolf_beacon_message_t *beacon,
                             uint32_t now_ms)
{
    memset(candidate, 0, sizeof(*candidate));
    candidate->used = true;
    candidate->token = allocate_token(directory);
    candidate->last_seen_ms = now_ms;
    memcpy(candidate->host_mac, host_mac, sizeof(candidate->host_mac));
    candidate->session_id = session_id;
    candidate->epoch = epoch;
    candidate->beacon = *beacon;
}

void werewolf_room_directory_init(werewolf_room_directory_t *directory)
{
    if (directory != NULL) {
        memset(directory, 0, sizeof(*directory));
    }
}

void werewolf_room_directory_clear(werewolf_room_directory_t *directory)
{
    werewolf_room_directory_init(directory);
}

size_t werewolf_room_directory_count(
    const werewolf_room_directory_t *directory)
{
    return directory != NULL ? directory->count : 0U;
}

bool werewolf_room_candidate_is_fresh(
    const werewolf_room_candidate_t *candidate, uint32_t now_ms)
{
    return candidate != NULL && candidate->used &&
           (uint32_t)(now_ms - candidate->last_seen_ms) <
               WEREWOLF_ROOM_DIRECTORY_STALE_MS;
}

size_t werewolf_room_directory_expire(werewolf_room_directory_t *directory,
                                      uint32_t now_ms)
{
    size_t expired = 0U;

    if (directory == NULL) {
        return 0U;
    }
    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        werewolf_room_candidate_t *candidate = &directory->entries[i];

        if (candidate->used &&
            !werewolf_room_candidate_is_fresh(candidate, now_ms)) {
            memset(candidate, 0, sizeof(*candidate));
            ++expired;
        }
    }
    if (expired > directory->count) {
        directory->count = 0U;
    } else {
        directory->count -= expired;
    }
    return expired;
}

werewolf_room_directory_result_t werewolf_room_directory_observe(
    werewolf_room_directory_t *directory,
    const uint8_t host_mac[WEREWOLF_NET_MAC_SIZE],
    uint64_t session_id,
    uint32_t epoch,
    const werewolf_beacon_message_t *beacon,
    uint32_t now_ms,
    uint32_t selected_token,
    uint32_t *out_token)
{
    werewolf_room_candidate_t *free_entry = NULL;
    werewolf_room_candidate_t *same_mac_entry = NULL;
    werewolf_room_candidate_t *victim = NULL;
    uint32_t victim_age = 0U;

    if (out_token != NULL) {
        *out_token = WEREWOLF_ROOM_TOKEN_NONE;
    }
    if (directory == NULL || !mac_valid(host_mac) || session_id == 0U ||
        epoch == 0U || !beacon_valid(beacon)) {
        return WEREWOLF_ROOM_DIRECTORY_INVALID;
    }

    (void)werewolf_room_directory_expire(directory, now_ms);
    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        werewolf_room_candidate_t *candidate = &directory->entries[i];

        if (same_identity(candidate, host_mac, session_id, epoch)) {
            bool changed = !same_beacon(&candidate->beacon, beacon);

            candidate->last_seen_ms = now_ms;
            if (changed) {
                candidate->beacon = *beacon;
            }
            if (out_token != NULL) {
                *out_token = candidate->token;
            }
            return changed ? WEREWOLF_ROOM_DIRECTORY_UPDATED
                           : WEREWOLF_ROOM_DIRECTORY_REFRESHED;
        }
        if (same_mac_entry == NULL && same_host(candidate, host_mac)) {
            same_mac_entry = candidate;
        }
        if (!candidate->used && free_entry == NULL) {
            free_entry = candidate;
        }
    }

    if (same_mac_entry != NULL) {
        assign_candidate(directory, same_mac_entry, host_mac, session_id,
                         epoch, beacon, now_ms);
        if (out_token != NULL) {
            *out_token = same_mac_entry->token;
        }
        return WEREWOLF_ROOM_DIRECTORY_REPLACED;
    }

    if (free_entry != NULL) {
        assign_candidate(directory, free_entry, host_mac, session_id, epoch,
                         beacon, now_ms);
        ++directory->count;
        if (out_token != NULL) {
            *out_token = free_entry->token;
        }
        return WEREWOLF_ROOM_DIRECTORY_ADDED;
    }

    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        werewolf_room_candidate_t *candidate = &directory->entries[i];
        uint32_t age;

        if (!candidate->used || candidate->token == selected_token) {
            continue;
        }
        age = (uint32_t)(now_ms - candidate->last_seen_ms);
        if (victim == NULL || age > victim_age) {
            victim = candidate;
            victim_age = age;
        }
    }
    if (victim == NULL) {
        return WEREWOLF_ROOM_DIRECTORY_FULL;
    }
    assign_candidate(directory, victim, host_mac, session_id, epoch, beacon,
                     now_ms);
    if (out_token != NULL) {
        *out_token = victim->token;
    }
    return WEREWOLF_ROOM_DIRECTORY_EVICTED;
}

bool werewolf_room_directory_find(
    const werewolf_room_directory_t *directory,
    uint32_t token,
    werewolf_room_candidate_t *candidate)
{
    if (directory == NULL || token == WEREWOLF_ROOM_TOKEN_NONE ||
        candidate == NULL) {
        return false;
    }
    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        if (directory->entries[i].used &&
            directory->entries[i].token == token) {
            *candidate = directory->entries[i];
            return true;
        }
    }
    return false;
}

bool werewolf_room_directory_find_fresh(
    const werewolf_room_directory_t *directory,
    uint32_t token,
    uint32_t now_ms,
    werewolf_room_candidate_t *candidate)
{
    werewolf_room_candidate_t found;

    if (!werewolf_room_directory_find(directory, token, &found) ||
        !werewolf_room_candidate_is_fresh(&found, now_ms)) {
        return false;
    }
    if (candidate != NULL) {
        *candidate = found;
    }
    return true;
}

static int candidate_compare(const werewolf_room_candidate_t *left,
                             const werewolf_room_candidate_t *right)
{
    int compared = memcmp(left->beacon.room_fingerprint,
                          right->beacon.room_fingerprint,
                          sizeof(left->beacon.room_fingerprint));

    if (compared == 0) {
        compared = memcmp(left->host_mac, right->host_mac,
                          sizeof(left->host_mac));
    }
    if (compared == 0 && left->session_id != right->session_id) {
        compared = left->session_id < right->session_id ? -1 : 1;
    }
    if (compared == 0 && left->epoch != right->epoch) {
        compared = left->epoch < right->epoch ? -1 : 1;
    }
    return compared;
}

size_t werewolf_room_directory_snapshot_sorted(
    const werewolf_room_directory_t *directory,
    werewolf_room_candidate_t *candidates,
    size_t capacity)
{
    werewolf_room_candidate_t sorted[WEREWOLF_ROOM_DIRECTORY_CAPACITY];
    size_t found = 0U;
    size_t copied;

    if (directory == NULL || (candidates == NULL && capacity != 0U)) {
        return 0U;
    }
    for (size_t i = 0U; i < WEREWOLF_ROOM_DIRECTORY_CAPACITY; ++i) {
        if (directory->entries[i].used) {
            sorted[found] = directory->entries[i];
            ++found;
        }
    }
    for (size_t i = 1U; i < found; ++i) {
        werewolf_room_candidate_t value = sorted[i];
        size_t position = i;

        while (position > 0U &&
               candidate_compare(&value, &sorted[position - 1U]) < 0) {
            sorted[position] = sorted[position - 1U];
            --position;
        }
        sorted[position] = value;
    }
    copied = found < capacity ? found : capacity;
    if (copied != 0U) {
        memcpy(candidates, sorted, copied * sizeof(candidates[0]));
    }
    return copied;
}
