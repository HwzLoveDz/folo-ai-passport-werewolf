#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "psa/crypto_types.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WW_PAIRING_PUBLIC_KEY_SIZE 32u
#define WW_PAIRING_SHARED_SECRET_SIZE 32u
#define WW_PAIRING_ESPNOW_KEY_SIZE 16u
#define WW_PAIRING_MAC_SIZE 6u
#define WW_PAIRING_NONCE_SIZE 16u
#define WW_PAIRING_COMMITMENT_SIZE 32u
#define WW_PAIRING_VERIFY_CODE_DIGITS 6u
#define WW_PAIRING_VERIFY_CODE_TEXT_SIZE (WW_PAIRING_VERIFY_CODE_DIGITS + 1u)

typedef enum {
    WW_PAIRING_OK = 0,
    WW_PAIRING_INVALID_ARGUMENT = -1,
    WW_PAIRING_UNSUPPORTED = -2,
    WW_PAIRING_CRYPTO_ERROR = -3,
    WW_PAIRING_BAD_STATE = -4,
    WW_PAIRING_COMMITMENT_MISMATCH = -5,
} ww_pairing_status_t;

typedef enum {
    WW_PAIRING_ROLE_HOST = 0,
    WW_PAIRING_ROLE_CLIENT = 1,
} ww_pairing_role_t;

/*
 * The private scalar remains inside PSA Crypto on ESP-IDF.  The context owns
 * that volatile key handle and must be released with ww_pairing_deinit().
 * Always initialize a context with WW_PAIRING_CONTEXT_INIT before first use.
 */
typedef struct {
    uint8_t public_key[WW_PAIRING_PUBLIC_KEY_SIZE];
#ifdef ESP_PLATFORM
    mbedtls_svc_key_id_t private_key;
#else
    uint32_t private_key;
#endif
    uint8_t initialized;
} ww_pairing_t;

#define WW_PAIRING_CONTEXT_INIT { { 0 }, 0, 0 }

/* Generate one volatile X25519 key pair for this session. */
ww_pairing_status_t ww_pairing_init(ww_pairing_t *ctx);

/* Destroy the PSA private key and clear the complete context. */
ww_pairing_status_t ww_pairing_deinit(ww_pairing_t *ctx);

/* Copy the 32-byte RFC 7748 public key from an initialized context. */
ww_pairing_status_t ww_pairing_get_public_key(
    const ww_pairing_t *ctx,
    uint8_t public_key[WW_PAIRING_PUBLIC_KEY_SIZE]);

/*
 * Commit to one unrevealed, single-use host offer.  A host offer is bound to
 * exactly one non-zero session and one client seat.  The host MAC must be a
 * unicast address.  Never reuse the public-key/nonce pair after it is
 * revealed, after the offer times out, or for a different seat.
 */
ww_pairing_status_t ww_pairing_make_host_commitment(
    const uint8_t host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    uint8_t commitment[WW_PAIRING_COMMITMENT_SIZE]);

/* Constant-time comparison after recomputing the host commitment. */
ww_pairing_status_t ww_pairing_verify_host_commitment(
    const uint8_t expected[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t host_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t host_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat);

/*
 * Commit to the client's unrevealed key/nonce.  Chaining host_commitment into
 * this value prevents a JOIN commitment from being replayed across host offers.
 */
ww_pairing_status_t ww_pairing_make_client_commitment(
    const uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t client_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE],
    uint8_t commitment[WW_PAIRING_COMMITMENT_SIZE]);

/* Constant-time comparison after recomputing the client commitment. */
ww_pairing_status_t ww_pairing_verify_client_commitment(
    const uint8_t expected[WW_PAIRING_COMMITMENT_SIZE],
    const uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE],
    const uint8_t client_nonce[WW_PAIRING_NONCE_SIZE],
    uint64_t session_id,
    const uint8_t host_mac[WW_PAIRING_MAC_SIZE],
    const uint8_t client_mac[WW_PAIRING_MAC_SIZE],
    uint8_t seat,
    const uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE]);

/*
 * Run X25519 and derive the peer-specific ESP-NOW LMK plus a local, read-only
 * six-digit verification code. The code is never sent over the air.
 * The role establishes a canonical host/client ordering for both MACs and
 * public keys.  The complete verified commit/reveal transcript is included,
 * so each endpoint derives identical output and stale offers cannot collide.
 */
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
    char verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE]);

/*
 * Deterministic protocol/KDF half, available to native host tests.  The
 * shared secret must be the 32-byte X25519 output.  Inputs and outputs must
 * not overlap.
 */
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
    char verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE]);

/*
 * Derive the room-scoped ESP-NOW PMK configured by every device in one room.
 * ESP-NOW uses each device's PMK to wrap its LMKs; encrypted peers must
 * install the same peer-specific LMK, but their PMKs are not an on-air shared
 * secret requirement.  This project derives the PMK from public room identity
 * to avoid the SDK default or a compiled constant.  Confidentiality and peer
 * authentication remain provided by the secret X25519-derived LMKs above.
 */
ww_pairing_status_t ww_pairing_derive_room_pmk(
    uint64_t session_id,
    uint32_t protocol_epoch,
    const uint8_t *room_fingerprint,
    size_t room_fingerprint_len,
    uint8_t pmk[WW_PAIRING_ESPNOW_KEY_SIZE]);

/* Generate a non-all-zero 128-bit nonce for exactly one pairing offer. */
ww_pairing_status_t ww_pairing_generate_nonce(
    uint8_t nonce[WW_PAIRING_NONCE_SIZE]);

/* Generate a non-all-zero local ESP-NOW PMK. */
ww_pairing_status_t ww_pairing_generate_local_pmk(
    uint8_t pmk[WW_PAIRING_ESPNOW_KEY_SIZE]);

#ifdef __cplusplus
}
#endif
