#include "werewolf_pairing.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const uint8_t s_shared_secret[WW_PAIRING_SHARED_SECRET_SIZE] = {
    0x4au, 0x5du, 0x9du, 0x5bu, 0xa4u, 0xceu, 0x2du, 0xe1u,
    0x72u, 0x8eu, 0x3bu, 0xf4u, 0x80u, 0x35u, 0x0fu, 0x25u,
    0xe0u, 0x7eu, 0x21u, 0xc9u, 0x47u, 0xd1u, 0x9eu, 0x33u,
    0x76u, 0xf0u, 0x9bu, 0x3cu, 0x1eu, 0x16u, 0x17u, 0x42u,
};

static const uint8_t s_host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE] = {
    0x85u, 0x20u, 0xf0u, 0x09u, 0x89u, 0x30u, 0xa7u, 0x54u,
    0x74u, 0x8bu, 0x7du, 0xdcu, 0xb4u, 0x3eu, 0xf7u, 0x5au,
    0x0du, 0xbfu, 0x3au, 0x0du, 0x26u, 0x38u, 0x1au, 0xf4u,
    0xebu, 0xa4u, 0xa9u, 0x8eu, 0xaau, 0x9bu, 0x4eu, 0x6au,
};

static const uint8_t s_client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE] = {
    0xdeu, 0x9eu, 0xdbu, 0x7du, 0x7bu, 0x7du, 0xc1u, 0xb4u,
    0xd3u, 0x5bu, 0x61u, 0xc2u, 0xecu, 0xe4u, 0x35u, 0x37u,
    0x3fu, 0x83u, 0x43u, 0xc8u, 0x5bu, 0x78u, 0x67u, 0x4du,
    0xadu, 0xfdu, 0xaau, 0xaau, 0xaau, 0xaau, 0xaau, 0xaau,
};

static const uint8_t s_host_mac[WW_PAIRING_MAC_SIZE] = {
    0x24u, 0x6fu, 0x28u, 0x10u, 0x20u, 0x30u,
};

static const uint8_t s_client_mac[WW_PAIRING_MAC_SIZE] = {
    0x24u, 0x6fu, 0x28u, 0xa0u, 0xb0u, 0xc0u,
};

static const uint8_t s_host_nonce[WW_PAIRING_NONCE_SIZE] = {
    0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
    0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x1du, 0x1eu, 0x1fu,
};

static const uint8_t s_client_nonce[WW_PAIRING_NONCE_SIZE] = {
    0xa0u, 0xa1u, 0xa2u, 0xa3u, 0xa4u, 0xa5u, 0xa6u, 0xa7u,
    0xa8u, 0xa9u, 0xaau, 0xabu, 0xacu, 0xadu, 0xaeu, 0xafu,
};

/* Independently checked with Python hashlib/hmac using the documented v2
 * transcript byte order. */
static const uint8_t s_expected_host_commitment[WW_PAIRING_COMMITMENT_SIZE] = {
    0x9eu, 0x40u, 0xd8u, 0x0du, 0x4bu, 0x7cu, 0x55u, 0x52u,
    0xe2u, 0x08u, 0xf1u, 0xdeu, 0x11u, 0xe6u, 0x92u, 0xc5u,
    0x4fu, 0xc3u, 0xf1u, 0x39u, 0x7fu, 0xeau, 0xc8u, 0xceu,
    0x5du, 0xb8u, 0xa5u, 0x32u, 0x2fu, 0xe3u, 0x68u, 0xe2u,
};

static const uint8_t s_expected_client_commitment[WW_PAIRING_COMMITMENT_SIZE] = {
    0x32u, 0xf2u, 0x6bu, 0xfbu, 0x43u, 0x74u, 0x93u, 0x29u,
    0xa0u, 0x65u, 0x1bu, 0x5cu, 0x58u, 0x27u, 0xb7u, 0x3fu,
    0x3eu, 0x8cu, 0x04u, 0x09u, 0xcau, 0xf5u, 0xf4u, 0x92u,
    0x1fu, 0xa7u, 0x72u, 0x4fu, 0x75u, 0x0eu, 0x32u, 0xc2u,
};

static const uint8_t s_expected_lmk[WW_PAIRING_ESPNOW_KEY_SIZE] = {
    0x42u, 0x4du, 0x78u, 0xc5u, 0x7du, 0xaau, 0xd5u, 0xaeu,
    0x44u, 0x6eu, 0xedu, 0x06u, 0x6bu, 0x86u, 0x45u, 0x43u,
};

#define TEST_SESSION UINT64_C(0x1020304050607080)
#define TEST_SEAT 4u

static bool all_zero(const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0u; i < len; ++i) {
        if (data[i] != 0u) {
            return false;
        }
    }
    return true;
}

static void make_commits(
    const uint8_t host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id, uint8_t seat,
    uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE],
    uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE])
{
    assert(ww_pairing_make_host_commitment(
               host_public_key, host_nonce, session_id, s_host_mac, seat,
               host_commitment) == WW_PAIRING_OK);
    assert(ww_pairing_make_client_commitment(
               client_public_key, client_nonce, session_id,
               s_host_mac, s_client_mac, seat, host_commitment,
               client_commitment) == WW_PAIRING_OK);
}

static void test_commitment_and_constant_time_verify(void)
{
    uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t changed[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t changed_nonce[WW_PAIRING_NONCE_SIZE];

    make_commits(s_host_public_key, s_client_public_key,
                 s_host_nonce, s_client_nonce, TEST_SESSION, TEST_SEAT,
                 host_commitment, client_commitment);
    assert(memcmp(host_commitment, s_expected_host_commitment,
                  sizeof(host_commitment)) == 0);
    assert(memcmp(client_commitment, s_expected_client_commitment,
                  sizeof(client_commitment)) == 0);
    assert(ww_pairing_verify_host_commitment(
               host_commitment, s_host_public_key, s_host_nonce,
               TEST_SESSION, s_host_mac, TEST_SEAT) == WW_PAIRING_OK);
    assert(ww_pairing_verify_client_commitment(
               client_commitment, s_client_public_key, s_client_nonce,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               host_commitment) == WW_PAIRING_OK);

    memcpy(changed, host_commitment, sizeof(changed));
    changed[31] ^= 0x01u;
    assert(ww_pairing_verify_host_commitment(
               changed, s_host_public_key, s_host_nonce,
               TEST_SESSION, s_host_mac, TEST_SEAT) ==
           WW_PAIRING_COMMITMENT_MISMATCH);

    memcpy(changed_nonce, s_client_nonce, sizeof(changed_nonce));
    changed_nonce[0] ^= 0x80u;
    assert(ww_pairing_verify_client_commitment(
               client_commitment, s_client_public_key, changed_nonce,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               host_commitment) == WW_PAIRING_COMMITMENT_MISMATCH);

    memset(changed, 0xa5, sizeof(changed));
    assert(ww_pairing_make_host_commitment(
               s_host_public_key, s_host_nonce, TEST_SESSION, s_host_mac,
               0u, changed) == WW_PAIRING_INVALID_ARGUMENT);
    assert(all_zero(changed, sizeof(changed)));
}

static void test_host_and_client_order_match(void)
{
    uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t host_lmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    uint8_t client_lmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    char host_verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE];
    char client_verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE];
    size_t digit;

    make_commits(s_host_public_key, s_client_public_key,
                 s_host_nonce, s_client_nonce, TEST_SESSION, TEST_SEAT,
                 host_commitment, client_commitment);
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_HOST,
               s_host_public_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               s_host_nonce, s_client_nonce,
               host_commitment, client_commitment,
               host_lmk, host_verification_code) == WW_PAIRING_OK);
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_CLIENT,
               s_client_public_key, s_host_public_key,
               TEST_SESSION, s_client_mac, s_host_mac, TEST_SEAT,
               s_host_nonce, s_client_nonce,
               host_commitment, client_commitment,
               client_lmk, client_verification_code) == WW_PAIRING_OK);
    assert(memcmp(host_lmk, client_lmk, sizeof(host_lmk)) == 0);
    assert(strcmp(host_verification_code, client_verification_code) == 0);
    assert(memcmp(host_lmk, s_expected_lmk, sizeof(host_lmk)) == 0);
    assert(strcmp(host_verification_code, "409365") == 0);
    assert(strlen(host_verification_code) == WW_PAIRING_VERIFY_CODE_DIGITS);
    for (digit = 0u; digit < WW_PAIRING_VERIFY_CODE_DIGITS; ++digit) {
        assert(host_verification_code[digit] >= '0' &&
               host_verification_code[digit] <= '9');
    }
}

static void test_room_pmk_is_shared_and_room_bound(void)
{
    static const uint8_t room_fingerprint[8] = {
        0x21u, 0x43u, 0x65u, 0x87u, 0xa9u, 0xcbu, 0xedu, 0x0fu,
    };
    uint8_t changed_fingerprint[sizeof(room_fingerprint)];
    uint8_t host_pmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    uint8_t client_pmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    uint8_t changed_pmk[WW_PAIRING_ESPNOW_KEY_SIZE];

    assert(ww_pairing_derive_room_pmk(
               TEST_SESSION, 0x12345678u, room_fingerprint,
               sizeof(room_fingerprint), host_pmk) == WW_PAIRING_OK);
    assert(ww_pairing_derive_room_pmk(
               TEST_SESSION, 0x12345678u, room_fingerprint,
               sizeof(room_fingerprint), client_pmk) == WW_PAIRING_OK);
    assert(memcmp(host_pmk, client_pmk, sizeof(host_pmk)) == 0);
    assert(!all_zero(host_pmk, sizeof(host_pmk)));

    assert(ww_pairing_derive_room_pmk(
               TEST_SESSION + 1u, 0x12345678u, room_fingerprint,
               sizeof(room_fingerprint), changed_pmk) == WW_PAIRING_OK);
    assert(memcmp(host_pmk, changed_pmk, sizeof(host_pmk)) != 0);
    assert(ww_pairing_derive_room_pmk(
               TEST_SESSION, 0x12345679u, room_fingerprint,
               sizeof(room_fingerprint), changed_pmk) == WW_PAIRING_OK);
    assert(memcmp(host_pmk, changed_pmk, sizeof(host_pmk)) != 0);

    memcpy(changed_fingerprint, room_fingerprint,
           sizeof(changed_fingerprint));
    changed_fingerprint[0] ^= 0x80u;
    assert(ww_pairing_derive_room_pmk(
               TEST_SESSION, 0x12345678u, changed_fingerprint,
               sizeof(changed_fingerprint), changed_pmk) == WW_PAIRING_OK);
    assert(memcmp(host_pmk, changed_pmk, sizeof(host_pmk)) != 0);

    memset(changed_pmk, 0xa5, sizeof(changed_pmk));
    assert(ww_pairing_derive_room_pmk(
               0u, 0x12345678u, room_fingerprint,
               sizeof(room_fingerprint), changed_pmk) ==
           WW_PAIRING_INVALID_ARGUMENT);
    assert(all_zero(changed_pmk, sizeof(changed_pmk)));
}

static void test_offer_rotation_and_transcript_binding(void)
{
    uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t rotated_host_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t rotated_client_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t rotated_host_key[WW_PAIRING_PUBLIC_KEY_SIZE];
    uint8_t rotated_host_nonce[WW_PAIRING_NONCE_SIZE];
    uint8_t baseline_lmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    uint8_t rotated_lmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    char baseline_verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE];
    char rotated_verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE];

    make_commits(s_host_public_key, s_client_public_key,
                 s_host_nonce, s_client_nonce, TEST_SESSION, TEST_SEAT,
                 host_commitment, client_commitment);
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_HOST,
               s_host_public_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               s_host_nonce, s_client_nonce,
               host_commitment, client_commitment,
               baseline_lmk, baseline_verification_code) == WW_PAIRING_OK);

    memcpy(rotated_host_key, s_host_public_key, sizeof(rotated_host_key));
    memcpy(rotated_host_nonce, s_host_nonce, sizeof(rotated_host_nonce));
    rotated_host_key[7] ^= 0x80u;
    rotated_host_nonce[15] ^= 0x01u;
    make_commits(rotated_host_key, s_client_public_key,
                 rotated_host_nonce, s_client_nonce,
                 TEST_SESSION, TEST_SEAT,
                 rotated_host_commitment, rotated_client_commitment);

    assert(memcmp(host_commitment, rotated_host_commitment,
                  sizeof(host_commitment)) != 0);
    assert(ww_pairing_verify_host_commitment(
               host_commitment, rotated_host_key, rotated_host_nonce,
               TEST_SESSION, s_host_mac, TEST_SEAT) ==
           WW_PAIRING_COMMITMENT_MISMATCH);
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_HOST,
               rotated_host_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               rotated_host_nonce, s_client_nonce,
               rotated_host_commitment, rotated_client_commitment,
               rotated_lmk, rotated_verification_code) == WW_PAIRING_OK);
    assert(memcmp(baseline_lmk, rotated_lmk, sizeof(baseline_lmk)) != 0);
    assert(strcmp(baseline_verification_code,
                  rotated_verification_code) != 0);

    rotated_client_commitment[0] ^= 0x01u;
    memset(rotated_lmk, 0xa5, sizeof(rotated_lmk));
    memset(rotated_verification_code, '9', sizeof(rotated_verification_code));
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_HOST,
               rotated_host_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               rotated_host_nonce, s_client_nonce,
               rotated_host_commitment, rotated_client_commitment,
               rotated_lmk, rotated_verification_code) ==
           WW_PAIRING_COMMITMENT_MISMATCH);
    assert(all_zero(rotated_lmk, sizeof(rotated_lmk)));
    assert(all_zero((const uint8_t *)rotated_verification_code,
                    sizeof(rotated_verification_code)));
}

static void test_invalid_input_clears_output(void)
{
    uint8_t zero_secret[WW_PAIRING_SHARED_SECRET_SIZE] = { 0 };
    uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t lmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    char verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE];

    make_commits(s_host_public_key, s_client_public_key,
                 s_host_nonce, s_client_nonce, TEST_SESSION, TEST_SEAT,
                 host_commitment, client_commitment);
    memset(lmk, 0xa5, sizeof(lmk));
    memset(verification_code, '9', sizeof(verification_code));
    assert(ww_pairing_kdf(
               zero_secret, WW_PAIRING_ROLE_HOST,
               s_host_public_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               s_host_nonce, s_client_nonce,
               host_commitment, client_commitment,
               lmk, verification_code) == WW_PAIRING_INVALID_ARGUMENT);
    assert(all_zero(lmk, sizeof(lmk)));
    assert(all_zero((const uint8_t *)verification_code,
                    sizeof(verification_code)));

    memset(lmk, 0xa5, sizeof(lmk));
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_HOST,
               s_host_public_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               s_host_nonce, s_client_nonce,
               host_commitment, client_commitment,
               lmk, NULL) == WW_PAIRING_INVALID_ARGUMENT);
    assert(all_zero(lmk, sizeof(lmk)));

    memset(verification_code, '9', sizeof(verification_code));
    assert(ww_pairing_kdf(
               s_shared_secret, WW_PAIRING_ROLE_HOST,
               s_host_public_key, s_client_public_key,
               TEST_SESSION, s_host_mac, s_client_mac, TEST_SEAT,
               s_host_nonce, s_client_nonce,
               host_commitment, client_commitment,
               NULL, verification_code) == WW_PAIRING_INVALID_ARGUMENT);
    assert(all_zero((const uint8_t *)verification_code,
                    sizeof(verification_code)));
}

#ifndef ESP_PLATFORM
static void test_host_crypto_boundary(void)
{
    ww_pairing_t ctx = WW_PAIRING_CONTEXT_INIT;
    uint8_t nonce[WW_PAIRING_NONCE_SIZE];
    uint8_t pmk[WW_PAIRING_ESPNOW_KEY_SIZE];
    uint8_t public_key[WW_PAIRING_PUBLIC_KEY_SIZE];

    memset(pmk, 0xa5, sizeof(pmk));
    memset(nonce, 0xa5, sizeof(nonce));
    assert(ww_pairing_init(&ctx) == WW_PAIRING_UNSUPPORTED);
    assert(all_zero((const uint8_t *)&ctx, sizeof(ctx)));
    assert(ww_pairing_get_public_key(&ctx, public_key) == WW_PAIRING_BAD_STATE);
    assert(all_zero(public_key, sizeof(public_key)));
    assert(ww_pairing_generate_nonce(nonce) == WW_PAIRING_UNSUPPORTED);
    assert(all_zero(nonce, sizeof(nonce)));
    assert(ww_pairing_generate_local_pmk(pmk) == WW_PAIRING_UNSUPPORTED);
    assert(all_zero(pmk, sizeof(pmk)));

    memset(&ctx, 0xa5, sizeof(ctx));
    assert(ww_pairing_deinit(&ctx) == WW_PAIRING_OK);
    assert(all_zero((const uint8_t *)&ctx, sizeof(ctx)));
}
#endif

int main(void)
{
    test_commitment_and_constant_time_verify();
    test_host_and_client_order_match();
    test_room_pmk_is_shared_and_room_bound();
    test_offer_rotation_and_transcript_binding();
    test_invalid_input_clears_output();
#ifndef ESP_PLATFORM
    test_host_crypto_boundary();
#endif
    puts("werewolf_pairing: all tests passed");
    return 0;
}
