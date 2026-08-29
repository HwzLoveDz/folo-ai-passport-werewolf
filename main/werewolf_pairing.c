#include "werewolf_pairing.h"

#include <stdbool.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "psa/crypto.h"
#endif

#define WW_PAIRING_INITIALIZED_MARKER 0xa7u
#define WW_PAIRING_FIRST_CLIENT_SEAT 1u
#define WW_PAIRING_LAST_CLIENT_SEAT 6u
#define WW_SHA256_BLOCK_SIZE 64u
#define WW_SHA256_DIGEST_SIZE 32u

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[WW_SHA256_BLOCK_SIZE];
    size_t block_len;
} ww_sha256_t;

static void secure_zero(void *data, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)data;

    while (len > 0u) {
        *p = 0u;
        ++p;
        --len;
    }
}

static bool is_all_zero(const uint8_t *data, size_t len)
{
    uint8_t aggregate = 0u;
    size_t i;

    for (i = 0u; i < len; ++i) {
        aggregate |= data[i];
    }
    return aggregate == 0u;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right,
                                size_t len)
{
    volatile uint8_t difference = 0u;
    size_t i;

    for (i = 0u; i < len; ++i) {
        difference = (uint8_t)(difference | (uint8_t)(left[i] ^ right[i]));
    }
    return difference == 0u;
}

static bool client_seat_valid(uint8_t seat)
{
    return seat >= WW_PAIRING_FIRST_CLIENT_SEAT &&
           seat <= WW_PAIRING_LAST_CLIENT_SEAT;
}

static bool mac_is_valid_unicast(const uint8_t mac[WW_PAIRING_MAC_SIZE])
{
    return !is_all_zero(mac, WW_PAIRING_MAC_SIZE) && (mac[0] & 0x01u) == 0u;
}

static uint32_t rotr32(uint32_t value, unsigned int shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static uint32_t read_be32(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24u) |
           ((uint32_t)in[1] << 16u) |
           ((uint32_t)in[2] << 8u) |
           (uint32_t)in[3];
}

static void write_be32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)(value >> 24u);
    out[1] = (uint8_t)(value >> 16u);
    out[2] = (uint8_t)(value >> 8u);
    out[3] = (uint8_t)value;
}

static void write_be64(uint8_t out[8], uint64_t value)
{
    out[0] = (uint8_t)(value >> 56u);
    out[1] = (uint8_t)(value >> 48u);
    out[2] = (uint8_t)(value >> 40u);
    out[3] = (uint8_t)(value >> 32u);
    out[4] = (uint8_t)(value >> 24u);
    out[5] = (uint8_t)(value >> 16u);
    out[6] = (uint8_t)(value >> 8u);
    out[7] = (uint8_t)value;
}

static void sha256_transform(ww_sha256_t *ctx, const uint8_t block[WW_SHA256_BLOCK_SIZE])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0u; i < 16u; ++i) {
        w[i] = read_be32(&block[i * 4u]);
    }
    for (i = 16u; i < 64u; ++i) {
        const uint32_t s0 = rotr32(w[i - 15u], 7u) ^
                            rotr32(w[i - 15u], 18u) ^
                            (w[i - 15u] >> 3u);
        const uint32_t s1 = rotr32(w[i - 2u], 17u) ^
                            rotr32(w[i - 2u], 19u) ^
                            (w[i - 2u] >> 10u);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0u; i < 64u; ++i) {
        const uint32_t sum1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + sum1 + choose + k[i] + w[i];
        const uint32_t sum0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
    secure_zero(w, sizeof(w));
}

static void sha256_init(ww_sha256_t *ctx)
{
    static const uint32_t initial_state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->state, initial_state, sizeof(initial_state));
}

static void sha256_update(ww_sha256_t *ctx, const uint8_t *data, size_t len)
{
    size_t take;

    ctx->total_bytes += (uint64_t)len;
    while (len > 0u) {
        take = WW_SHA256_BLOCK_SIZE - ctx->block_len;
        if (take > len) {
            take = len;
        }
        memcpy(&ctx->block[ctx->block_len], data, take);
        ctx->block_len += take;
        data += take;
        len -= take;
        if (ctx->block_len == WW_SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->block);
            ctx->block_len = 0u;
        }
    }
}

static void sha256_final(ww_sha256_t *ctx, uint8_t digest[WW_SHA256_DIGEST_SIZE])
{
    uint8_t length_be[8];
    uint8_t pad[WW_SHA256_BLOCK_SIZE] = { 0x80u };
    const uint64_t total_bits = ctx->total_bytes * 8u;
    const size_t pad_len = (ctx->block_len < 56u) ?
                           (56u - ctx->block_len) :
                           (120u - ctx->block_len);
    size_t i;

    write_be64(length_be, total_bits);
    sha256_update(ctx, pad, pad_len);
    sha256_update(ctx, length_be, sizeof(length_be));
    for (i = 0u; i < 8u; ++i) {
        write_be32(&digest[i * 4u], ctx->state[i]);
    }
    secure_zero(length_be, sizeof(length_be));
    secure_zero(pad, sizeof(pad));
    secure_zero(ctx, sizeof(*ctx));
}

ww_pairing_status_t ww_pairing_make_host_commitment(
    const uint8_t host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    uint8_t commitment[WW_PAIRING_COMMITMENT_SIZE])
{
    static const uint8_t domain[] =
        "AI-PASSPORT/WEREWOLF-HOST-COMMIT/v2";
    uint8_t session_be[8];
    ww_sha256_t hash;

    if (commitment == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    memset(commitment, 0, WW_PAIRING_COMMITMENT_SIZE);
    if (host_public_key == NULL || host_nonce == NULL || host_mac == NULL ||
        session_id == 0u || !client_seat_valid(seat) ||
        is_all_zero(host_public_key, WW_PAIRING_PUBLIC_KEY_SIZE) ||
        is_all_zero(host_nonce, WW_PAIRING_NONCE_SIZE) ||
        !mac_is_valid_unicast(host_mac)) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

    write_be64(session_be, session_id);
    sha256_init(&hash);
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    sha256_update(&hash, session_be, sizeof(session_be));
    sha256_update(&hash, host_mac, WW_PAIRING_MAC_SIZE);
    sha256_update(&hash, &seat, sizeof(seat));
    sha256_update(&hash, host_public_key, WW_PAIRING_PUBLIC_KEY_SIZE);
    sha256_update(&hash, host_nonce, WW_PAIRING_NONCE_SIZE);
    sha256_final(&hash, commitment);
    secure_zero(session_be, sizeof(session_be));
    return is_all_zero(commitment, WW_PAIRING_COMMITMENT_SIZE)
               ? WW_PAIRING_CRYPTO_ERROR
               : WW_PAIRING_OK;
}

ww_pairing_status_t ww_pairing_verify_host_commitment(
    const uint8_t expected[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat)
{
    uint8_t actual[WW_PAIRING_COMMITMENT_SIZE];
    ww_pairing_status_t status;

    if (expected == NULL ||
        is_all_zero(expected, WW_PAIRING_COMMITMENT_SIZE)) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    status = ww_pairing_make_host_commitment(host_public_key, host_nonce,
                                              session_id, host_mac, seat,
                                              actual);
    if (status == WW_PAIRING_OK &&
        !constant_time_equal(expected, actual, sizeof(actual))) {
        status = WW_PAIRING_COMMITMENT_MISMATCH;
    }
    secure_zero(actual, sizeof(actual));
    return status;
}

ww_pairing_status_t ww_pairing_make_client_commitment(
    const uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t client_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE],
    uint8_t commitment[WW_PAIRING_COMMITMENT_SIZE])
{
    static const uint8_t domain[] =
        "AI-PASSPORT/WEREWOLF-CLIENT-COMMIT/v2";
    uint8_t session_be[8];
    ww_sha256_t hash;

    if (commitment == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    memset(commitment, 0, WW_PAIRING_COMMITMENT_SIZE);
    if (client_public_key == NULL || client_nonce == NULL ||
        host_mac == NULL || client_mac == NULL || host_commitment == NULL ||
        session_id == 0u || !client_seat_valid(seat) ||
        is_all_zero(client_public_key, WW_PAIRING_PUBLIC_KEY_SIZE) ||
        is_all_zero(client_nonce, WW_PAIRING_NONCE_SIZE) ||
        is_all_zero(host_commitment, WW_PAIRING_COMMITMENT_SIZE) ||
        !mac_is_valid_unicast(host_mac) ||
        !mac_is_valid_unicast(client_mac) ||
        memcmp(host_mac, client_mac, WW_PAIRING_MAC_SIZE) == 0) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

    write_be64(session_be, session_id);
    sha256_init(&hash);
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    sha256_update(&hash, session_be, sizeof(session_be));
    sha256_update(&hash, host_mac, WW_PAIRING_MAC_SIZE);
    sha256_update(&hash, client_mac, WW_PAIRING_MAC_SIZE);
    sha256_update(&hash, &seat, sizeof(seat));
    sha256_update(&hash, host_commitment, WW_PAIRING_COMMITMENT_SIZE);
    sha256_update(&hash, client_public_key, WW_PAIRING_PUBLIC_KEY_SIZE);
    sha256_update(&hash, client_nonce, WW_PAIRING_NONCE_SIZE);
    sha256_final(&hash, commitment);
    secure_zero(session_be, sizeof(session_be));
    return is_all_zero(commitment, WW_PAIRING_COMMITMENT_SIZE)
               ? WW_PAIRING_CRYPTO_ERROR
               : WW_PAIRING_OK;
}

ww_pairing_status_t ww_pairing_verify_client_commitment(
    const uint8_t expected[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t client_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE])
{
    uint8_t actual[WW_PAIRING_COMMITMENT_SIZE];
    ww_pairing_status_t status;

    if (expected == NULL ||
        is_all_zero(expected, WW_PAIRING_COMMITMENT_SIZE)) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    status = ww_pairing_make_client_commitment(
        client_public_key, client_nonce, session_id, host_mac, client_mac,
        seat, host_commitment, actual);
    if (status == WW_PAIRING_OK &&
        !constant_time_equal(expected, actual, sizeof(actual))) {
        status = WW_PAIRING_COMMITMENT_MISMATCH;
    }
    secure_zero(actual, sizeof(actual));
    return status;
}

static void hmac_sha256_parts(
    const uint8_t *key,
    size_t key_len,
    const uint8_t *part1,
    size_t part1_len,
    const uint8_t *part2,
    size_t part2_len,
    const uint8_t *part3,
    size_t part3_len,
    uint8_t digest[WW_SHA256_DIGEST_SIZE])
{
    uint8_t key_block[WW_SHA256_BLOCK_SIZE] = { 0 };
    uint8_t inner_digest[WW_SHA256_DIGEST_SIZE];
    uint8_t inner_pad[WW_SHA256_BLOCK_SIZE];
    uint8_t outer_pad[WW_SHA256_BLOCK_SIZE];
    ww_sha256_t hash;
    size_t i;

    if (key_len > WW_SHA256_BLOCK_SIZE) {
        sha256_init(&hash);
        sha256_update(&hash, key, key_len);
        sha256_final(&hash, key_block);
    } else {
        memcpy(key_block, key, key_len);
    }

    for (i = 0u; i < WW_SHA256_BLOCK_SIZE; ++i) {
        inner_pad[i] = key_block[i] ^ 0x36u;
        outer_pad[i] = key_block[i] ^ 0x5cu;
    }

    sha256_init(&hash);
    sha256_update(&hash, inner_pad, sizeof(inner_pad));
    sha256_update(&hash, part1, part1_len);
    sha256_update(&hash, part2, part2_len);
    sha256_update(&hash, part3, part3_len);
    sha256_final(&hash, inner_digest);

    sha256_init(&hash);
    sha256_update(&hash, outer_pad, sizeof(outer_pad));
    sha256_update(&hash, inner_digest, sizeof(inner_digest));
    sha256_final(&hash, digest);

    secure_zero(key_block, sizeof(key_block));
    secure_zero(inner_digest, sizeof(inner_digest));
    secure_zero(inner_pad, sizeof(inner_pad));
    secure_zero(outer_pad, sizeof(outer_pad));
}

static void transcript_hash(
    uint64_t session_id,
    uint8_t seat,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t client_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE],
    uint8_t digest[WW_SHA256_DIGEST_SIZE])
{
    static const uint8_t domain[] = "AI-PASSPORT/WEREWOLF-PAIR/v2";
    uint8_t session_be[8];
    ww_sha256_t hash;

    write_be64(session_be, session_id);
    sha256_init(&hash);
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    sha256_update(&hash, session_be, sizeof(session_be));
    sha256_update(&hash, &seat, sizeof(seat));
    sha256_update(&hash, host_mac, WW_PAIRING_MAC_SIZE);
    sha256_update(&hash, client_mac, WW_PAIRING_MAC_SIZE);
    sha256_update(&hash, host_public_key, WW_PAIRING_PUBLIC_KEY_SIZE);
    sha256_update(&hash, client_public_key, WW_PAIRING_PUBLIC_KEY_SIZE);
    sha256_update(&hash, host_nonce, WW_PAIRING_NONCE_SIZE);
    sha256_update(&hash, client_nonce, WW_PAIRING_NONCE_SIZE);
    sha256_update(&hash, host_commitment, WW_PAIRING_COMMITMENT_SIZE);
    sha256_update(&hash, client_commitment, WW_PAIRING_COMMITMENT_SIZE);
    sha256_final(&hash, digest);
    secure_zero(session_be, sizeof(session_be));
}

static void hkdf_expand_one_block(
    const uint8_t prk[WW_SHA256_DIGEST_SIZE],
    const uint8_t *label,
    size_t label_len,
    const uint8_t transcript[WW_SHA256_DIGEST_SIZE],
    uint8_t output[WW_SHA256_DIGEST_SIZE])
{
    static const uint8_t block_index = 1u;

    hmac_sha256_parts(prk, WW_SHA256_DIGEST_SIZE,
                      label, label_len,
                      transcript, WW_SHA256_DIGEST_SIZE,
                      &block_index, sizeof(block_index),
                      output);
}

ww_pairing_status_t ww_pairing_derive_room_pmk(
    uint64_t session_id,
    uint32_t protocol_epoch,
    const uint8_t *room_fingerprint,
    size_t room_fingerprint_len,
    uint8_t pmk[WW_PAIRING_ESPNOW_KEY_SIZE])
{
    static const uint8_t domain[] =
        "AI-PASSPORT/ESP-NOW-ROOM-PMK/v1";
    uint8_t session_be[8];
    uint8_t epoch_be[4];
    uint8_t digest[WW_SHA256_DIGEST_SIZE];
    ww_sha256_t hash;

    if (pmk == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    memset(pmk, 0, WW_PAIRING_ESPNOW_KEY_SIZE);
    if (session_id == 0u || protocol_epoch == 0u ||
        room_fingerprint == NULL || room_fingerprint_len == 0u ||
        room_fingerprint_len > WW_SHA256_DIGEST_SIZE ||
        is_all_zero(room_fingerprint, room_fingerprint_len)) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

    write_be64(session_be, session_id);
    write_be32(epoch_be, protocol_epoch);
    sha256_init(&hash);
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    sha256_update(&hash, session_be, sizeof(session_be));
    sha256_update(&hash, epoch_be, sizeof(epoch_be));
    sha256_update(&hash, room_fingerprint, room_fingerprint_len);
    sha256_final(&hash, digest);
    memcpy(pmk, digest, WW_PAIRING_ESPNOW_KEY_SIZE);

    secure_zero(session_be, sizeof(session_be));
    secure_zero(epoch_be, sizeof(epoch_be));
    secure_zero(digest, sizeof(digest));
    return is_all_zero(pmk, WW_PAIRING_ESPNOW_KEY_SIZE)
               ? WW_PAIRING_CRYPTO_ERROR
               : WW_PAIRING_OK;
}

ww_pairing_status_t ww_pairing_kdf(
    const uint8_t shared_secret[WW_PAIRING_SHARED_SECRET_SIZE],
    ww_pairing_role_t role,
    const uint8_t local_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t peer_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    uint64_t session_id,
    const uint8_t local_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t peer_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE],
    uint8_t lmk[WW_PAIRING_ESPNOW_KEY_SIZE],
    char verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE])
{
    static const uint8_t lmk_label[] = "AI-PASSPORT/ESP-NOW-LMK/v2";
    /* This local-only domain is independent from the LMK and never enters a
     * frame. Keep it stable within protocol v5 for deterministic displays. */
    static const uint8_t verification_code_label[] =
        "AI-PASSPORT/PAIR-VERIFY/v1";
    const uint8_t *host_public_key;
    const uint8_t *client_public_key;
    const uint8_t *host_mac;
    const uint8_t *client_mac;
    uint8_t transcript[WW_SHA256_DIGEST_SIZE];
    uint8_t prk[WW_SHA256_DIGEST_SIZE];
    uint8_t expanded[WW_SHA256_DIGEST_SIZE];
    uint32_t verification_code_value;
    ww_pairing_status_t result = WW_PAIRING_INVALID_ARGUMENT;

    if (lmk != NULL) {
        memset(lmk, 0, WW_PAIRING_ESPNOW_KEY_SIZE);
    }
    if (verification_code != NULL) {
        memset(verification_code, 0, WW_PAIRING_VERIFY_CODE_TEXT_SIZE);
    }
    if (lmk == NULL || verification_code == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

    if (shared_secret == NULL || local_public_key == NULL ||
        peer_public_key == NULL || local_mac == NULL || peer_mac == NULL ||
        host_nonce == NULL || client_nonce == NULL ||
        host_commitment == NULL || client_commitment == NULL ||
        session_id == 0u || !client_seat_valid(seat) ||
        (role != WW_PAIRING_ROLE_HOST && role != WW_PAIRING_ROLE_CLIENT) ||
        is_all_zero(shared_secret, WW_PAIRING_SHARED_SECRET_SIZE) ||
        is_all_zero(local_public_key, WW_PAIRING_PUBLIC_KEY_SIZE) ||
        is_all_zero(peer_public_key, WW_PAIRING_PUBLIC_KEY_SIZE) ||
        is_all_zero(host_nonce, WW_PAIRING_NONCE_SIZE) ||
        is_all_zero(client_nonce, WW_PAIRING_NONCE_SIZE) ||
        is_all_zero(host_commitment, WW_PAIRING_COMMITMENT_SIZE) ||
        is_all_zero(client_commitment, WW_PAIRING_COMMITMENT_SIZE) ||
        memcmp(local_public_key, peer_public_key, WW_PAIRING_PUBLIC_KEY_SIZE) == 0 ||
        !mac_is_valid_unicast(local_mac) || !mac_is_valid_unicast(peer_mac) ||
        memcmp(local_mac, peer_mac, WW_PAIRING_MAC_SIZE) == 0) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

    if (role == WW_PAIRING_ROLE_HOST) {
        host_public_key = local_public_key;
        client_public_key = peer_public_key;
        host_mac = local_mac;
        client_mac = peer_mac;
    } else {
        host_public_key = peer_public_key;
        client_public_key = local_public_key;
        host_mac = peer_mac;
        client_mac = local_mac;
    }

    result = ww_pairing_verify_host_commitment(
        host_commitment, host_public_key, host_nonce, session_id, host_mac,
        seat);
    if (result != WW_PAIRING_OK) {
        goto cleanup;
    }
    result = ww_pairing_verify_client_commitment(
        client_commitment, client_public_key, client_nonce, session_id,
        host_mac, client_mac, seat, host_commitment);
    if (result != WW_PAIRING_OK) {
        goto cleanup;
    }

    result = WW_PAIRING_CRYPTO_ERROR;
    transcript_hash(session_id, seat, host_mac, client_mac,
                    host_public_key, client_public_key,
                    host_nonce, client_nonce,
                    host_commitment, client_commitment, transcript);
    hmac_sha256_parts(transcript, sizeof(transcript),
                      shared_secret, WW_PAIRING_SHARED_SECRET_SIZE,
                      NULL, 0u, NULL, 0u, prk);

    hkdf_expand_one_block(prk, lmk_label, sizeof(lmk_label) - 1u,
                          transcript, expanded);
    memcpy(lmk, expanded, WW_PAIRING_ESPNOW_KEY_SIZE);
    if (is_all_zero(lmk, WW_PAIRING_ESPNOW_KEY_SIZE)) {
        goto cleanup;
    }

    hkdf_expand_one_block(
        prk, verification_code_label, sizeof(verification_code_label) - 1u,
        transcript, expanded);
    verification_code_value = read_be32(expanded) % UINT32_C(1000000);
    verification_code[0] =
        (char)('0' + (verification_code_value / UINT32_C(100000)));
    verification_code[1] = (char)('0' +
        ((verification_code_value / UINT32_C(10000)) % UINT32_C(10)));
    verification_code[2] = (char)('0' +
        ((verification_code_value / UINT32_C(1000)) % UINT32_C(10)));
    verification_code[3] = (char)('0' +
        ((verification_code_value / UINT32_C(100)) % UINT32_C(10)));
    verification_code[4] = (char)('0' +
        ((verification_code_value / UINT32_C(10)) % UINT32_C(10)));
    verification_code[5] =
        (char)('0' + (verification_code_value % UINT32_C(10)));
    verification_code[6] = '\0';
    result = WW_PAIRING_OK;

cleanup:
    if (result != WW_PAIRING_OK) {
        memset(lmk, 0, WW_PAIRING_ESPNOW_KEY_SIZE);
        memset(verification_code, 0, WW_PAIRING_VERIFY_CODE_TEXT_SIZE);
    }
    secure_zero(transcript, sizeof(transcript));
    secure_zero(prk, sizeof(prk));
    secure_zero(expanded, sizeof(expanded));
    return result;
}

#ifdef ESP_PLATFORM
static ww_pairing_status_t status_from_psa(psa_status_t status)
{
    if (status == PSA_ERROR_NOT_SUPPORTED) {
        return WW_PAIRING_UNSUPPORTED;
    }
    if (status == PSA_ERROR_BAD_STATE) {
        return WW_PAIRING_BAD_STATE;
    }
    if (status == PSA_ERROR_INVALID_ARGUMENT) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    return WW_PAIRING_CRYPTO_ERROR;
}
#endif

ww_pairing_status_t ww_pairing_init(ww_pairing_t *ctx)
{
    if (ctx == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    if (ctx->initialized != 0u) {
        return WW_PAIRING_BAD_STATE;
    }
    secure_zero(ctx, sizeof(*ctx));

#ifdef ESP_PLATFORM
    {
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        mbedtls_svc_key_id_t private_key = MBEDTLS_SVC_KEY_ID_INIT;
        psa_status_t psa_status;
        size_t public_key_len = 0u;

        psa_status = psa_crypto_init();
        if (psa_status != PSA_SUCCESS) {
            return status_from_psa(psa_status);
        }

        psa_set_key_type(&attributes,
                         PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
        psa_set_key_bits(&attributes, 255u);
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
        psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
        psa_status = psa_generate_key(&attributes, &private_key);
        psa_reset_key_attributes(&attributes);
        if (psa_status != PSA_SUCCESS) {
            return status_from_psa(psa_status);
        }

        psa_status = psa_export_public_key(private_key,
                                           ctx->public_key,
                                           sizeof(ctx->public_key),
                                           &public_key_len);
        if (psa_status != PSA_SUCCESS ||
            public_key_len != WW_PAIRING_PUBLIC_KEY_SIZE ||
            is_all_zero(ctx->public_key, sizeof(ctx->public_key))) {
            (void)psa_destroy_key(private_key);
            secure_zero(ctx, sizeof(*ctx));
            return (psa_status == PSA_SUCCESS) ?
                   WW_PAIRING_CRYPTO_ERROR : status_from_psa(psa_status);
        }

        ctx->private_key = private_key;
        ctx->initialized = WW_PAIRING_INITIALIZED_MARKER;
        return WW_PAIRING_OK;
    }
#else
    return WW_PAIRING_UNSUPPORTED;
#endif
}

ww_pairing_status_t ww_pairing_deinit(ww_pairing_t *ctx)
{
    ww_pairing_status_t result = WW_PAIRING_OK;

    if (ctx == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

#ifdef ESP_PLATFORM
    if (ctx->initialized == WW_PAIRING_INITIALIZED_MARKER) {
        const psa_status_t psa_status = psa_destroy_key(ctx->private_key);

        if (psa_status != PSA_SUCCESS) {
            result = status_from_psa(psa_status);
        }
    }
#endif
    secure_zero(ctx, sizeof(*ctx));
    return result;
}

ww_pairing_status_t ww_pairing_get_public_key(
    const ww_pairing_t *ctx,
    uint8_t public_key[WW_PAIRING_PUBLIC_KEY_SIZE])
{
    if (ctx == NULL || public_key == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    if (ctx->initialized != WW_PAIRING_INITIALIZED_MARKER) {
        memset(public_key, 0, WW_PAIRING_PUBLIC_KEY_SIZE);
        return WW_PAIRING_BAD_STATE;
    }
    memcpy(public_key, ctx->public_key, WW_PAIRING_PUBLIC_KEY_SIZE);
    return WW_PAIRING_OK;
}

ww_pairing_status_t ww_pairing_derive(
    const ww_pairing_t *ctx,
    ww_pairing_role_t role,
    const uint8_t peer_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    uint64_t session_id,
    const uint8_t local_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t peer_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE],
    uint8_t lmk[WW_PAIRING_ESPNOW_KEY_SIZE],
    char verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE])
{
    if (lmk != NULL) {
        memset(lmk, 0, WW_PAIRING_ESPNOW_KEY_SIZE);
    }
    if (verification_code != NULL) {
        memset(verification_code, 0, WW_PAIRING_VERIFY_CODE_TEXT_SIZE);
    }
    if (lmk == NULL || verification_code == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    if (ctx == NULL || peer_public_key == NULL || local_mac == NULL ||
        peer_mac == NULL || host_nonce == NULL || client_nonce == NULL ||
        host_commitment == NULL || client_commitment == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    if (ctx->initialized != WW_PAIRING_INITIALIZED_MARKER) {
        return WW_PAIRING_BAD_STATE;
    }
    if (session_id == 0u || !client_seat_valid(seat) ||
        (role != WW_PAIRING_ROLE_HOST && role != WW_PAIRING_ROLE_CLIENT)) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }

#ifdef ESP_PLATFORM
    {
        uint8_t shared_secret[WW_PAIRING_SHARED_SECRET_SIZE];
        size_t shared_secret_len = 0u;
        psa_status_t psa_status;
        ww_pairing_status_t result;

        if (is_all_zero(peer_public_key, WW_PAIRING_PUBLIC_KEY_SIZE)) {
            return WW_PAIRING_INVALID_ARGUMENT;
        }
        psa_status = psa_raw_key_agreement(PSA_ALG_ECDH,
                                           ctx->private_key,
                                           peer_public_key,
                                           WW_PAIRING_PUBLIC_KEY_SIZE,
                                           shared_secret,
                                           sizeof(shared_secret),
                                           &shared_secret_len);
        if (psa_status != PSA_SUCCESS) {
            secure_zero(shared_secret, sizeof(shared_secret));
            return status_from_psa(psa_status);
        }
        if (shared_secret_len != WW_PAIRING_SHARED_SECRET_SIZE ||
            is_all_zero(shared_secret, sizeof(shared_secret))) {
            secure_zero(shared_secret, sizeof(shared_secret));
            return WW_PAIRING_CRYPTO_ERROR;
        }

        result = ww_pairing_kdf(shared_secret, role,
                                ctx->public_key, peer_public_key,
                                session_id, local_mac, peer_mac, seat,
                                host_nonce, client_nonce,
                                host_commitment, client_commitment,
                                lmk, verification_code);
        secure_zero(shared_secret, sizeof(shared_secret));
        return result;
    }
#else
    return WW_PAIRING_UNSUPPORTED;
#endif
}

ww_pairing_status_t ww_pairing_generate_nonce(
    uint8_t nonce[WW_PAIRING_NONCE_SIZE])
{
    if (nonce == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    memset(nonce, 0, WW_PAIRING_NONCE_SIZE);

#ifdef ESP_PLATFORM
    {
        unsigned int attempt;
        psa_status_t psa_status = psa_crypto_init();

        if (psa_status != PSA_SUCCESS) {
            return status_from_psa(psa_status);
        }
        for (attempt = 0u; attempt < 4u; ++attempt) {
            psa_status = psa_generate_random(nonce, WW_PAIRING_NONCE_SIZE);
            if (psa_status != PSA_SUCCESS) {
                secure_zero(nonce, WW_PAIRING_NONCE_SIZE);
                return status_from_psa(psa_status);
            }
            if (!is_all_zero(nonce, WW_PAIRING_NONCE_SIZE)) {
                return WW_PAIRING_OK;
            }
        }
        secure_zero(nonce, WW_PAIRING_NONCE_SIZE);
        return WW_PAIRING_CRYPTO_ERROR;
    }
#else
    return WW_PAIRING_UNSUPPORTED;
#endif
}

ww_pairing_status_t ww_pairing_generate_local_pmk(
    uint8_t pmk[WW_PAIRING_ESPNOW_KEY_SIZE])
{
    if (pmk == NULL) {
        return WW_PAIRING_INVALID_ARGUMENT;
    }
    memset(pmk, 0, WW_PAIRING_ESPNOW_KEY_SIZE);

#ifdef ESP_PLATFORM
    {
        unsigned int attempt;
        psa_status_t psa_status = psa_crypto_init();

        if (psa_status != PSA_SUCCESS) {
            return status_from_psa(psa_status);
        }
        for (attempt = 0u; attempt < 4u; ++attempt) {
            psa_status = psa_generate_random(pmk, WW_PAIRING_ESPNOW_KEY_SIZE);
            if (psa_status != PSA_SUCCESS) {
                secure_zero(pmk, WW_PAIRING_ESPNOW_KEY_SIZE);
                return status_from_psa(psa_status);
            }
            if (!is_all_zero(pmk, WW_PAIRING_ESPNOW_KEY_SIZE)) {
                return WW_PAIRING_OK;
            }
        }
        secure_zero(pmk, WW_PAIRING_ESPNOW_KEY_SIZE);
        return WW_PAIRING_CRYPTO_ERROR;
    }
#else
    return WW_PAIRING_UNSUPPORTED;
#endif
}
