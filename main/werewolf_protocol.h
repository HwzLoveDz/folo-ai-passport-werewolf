#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wire layout (network byte order, no C structure is sent directly):
 *
 *   0  magic          u16
 *   2  version        u8
 *   3  header length  u8
 *   4  message type   u8
 *   5  flags          u8
 *   6  source id      u8
 *   7  destination id u8
 *   8  session id     u64
 *  16  leader epoch   u32
 *  20  message seq    u32
 *  24  ack seq        u32
 *  28  phase seq      u16
 *  30  payload length u16
 *  32  action key     u32
 *  36  payload        0..162 bytes
 *  ... CRC16-CCITT    u16
 *
 * CRC detects malformed frames only. It is not an authentication mechanism;
 * non-discovery traffic must use an encrypted ESP-NOW peer.
 */
#define WEREWOLF_PROTOCOL_MAGIC            0x5757U /* "WW" */
#define WEREWOLF_PROTOCOL_VERSION          5U
#define WEREWOLF_PROTOCOL_HEADER_SIZE      36U
#define WEREWOLF_PROTOCOL_CRC_SIZE         2U
#define WEREWOLF_PROTOCOL_MAX_FRAME_SIZE   200U
#define WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE \
    (WEREWOLF_PROTOCOL_MAX_FRAME_SIZE - WEREWOLF_PROTOCOL_HEADER_SIZE - \
     WEREWOLF_PROTOCOL_CRC_SIZE)

#define WEREWOLF_PLAYER_BROADCAST 0xFFU

typedef enum {
    WEREWOLF_MSG_BEACON = 1,
    WEREWOLF_MSG_JOIN = 2,
    WEREWOLF_MSG_ACCEPT = 3,
    WEREWOLF_MSG_READY = 4,
    WEREWOLF_MSG_START = 5,
    WEREWOLF_MSG_PRIVATE_ROLE = 6,
    WEREWOLF_MSG_PHASE = 7,
    WEREWOLF_MSG_ACTION = 8,
    WEREWOLF_MSG_ACK = 9,
    WEREWOLF_MSG_SNAPSHOT = 10,
    WEREWOLF_MSG_RESUME = 11,
    WEREWOLF_MSG_PAUSE = 12,
    WEREWOLF_MSG_ABORT = 13,
    /* Commit/reveal handshake frames remain cleartext broadcast discovery.
     * No X25519 public key is disclosed by BEACON or JOIN in protocol v5. */
    WEREWOLF_MSG_PAIR_HOST_REVEAL = 14,
    WEREWOLF_MSG_PAIR_CLIENT_REVEAL = 15,
    /* Encrypted after ACCEPT; a client may submit only its own display name. */
    WEREWOLF_MSG_PROFILE = 16,
} werewolf_message_type_t;

enum {
    WEREWOLF_FLAG_ACK_REQUIRED = 1U << 0,
    WEREWOLF_FLAG_IS_ACK = 1U << 1,
    WEREWOLF_FLAG_ALLOWED_MASK = WEREWOLF_FLAG_ACK_REQUIRED |
                                 WEREWOLF_FLAG_IS_ACK,
};

typedef enum {
    WEREWOLF_PROTOCOL_OK = 0,
    WEREWOLF_PROTOCOL_ERR_ARGUMENT = -1,
    WEREWOLF_PROTOCOL_ERR_CAPACITY = -2,
    WEREWOLF_PROTOCOL_ERR_LENGTH = -3,
    WEREWOLF_PROTOCOL_ERR_MAGIC = -4,
    WEREWOLF_PROTOCOL_ERR_VERSION = -5,
    WEREWOLF_PROTOCOL_ERR_TYPE = -6,
    WEREWOLF_PROTOCOL_ERR_FLAGS = -7,
    WEREWOLF_PROTOCOL_ERR_POLICY = -8,
    WEREWOLF_PROTOCOL_ERR_CRC = -9,
} werewolf_protocol_result_t;

typedef struct {
    uint8_t version;
    werewolf_message_type_t type;
    uint8_t flags;
    uint8_t src;
    uint8_t dst;
    uint64_t session_id;
    uint32_t epoch;
    uint32_t msg_seq;
    uint32_t ack_seq;
    uint16_t phase_seq;
    uint16_t payload_len;
    uint32_t action_key;
    uint8_t payload[WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE];
} werewolf_frame_t;

bool werewolf_protocol_type_valid(werewolf_message_type_t type);
bool werewolf_protocol_type_is_discovery(werewolf_message_type_t type);
bool werewolf_protocol_type_requires_encryption(werewolf_message_type_t type);
bool werewolf_protocol_type_is_phase_bound(werewolf_message_type_t type);

size_t werewolf_protocol_encoded_size(uint16_t payload_len);
uint16_t werewolf_protocol_crc16(const uint8_t *data, size_t len);

werewolf_protocol_result_t werewolf_protocol_encode(
    const werewolf_frame_t *frame,
    uint8_t *wire,
    size_t wire_capacity,
    size_t *wire_len);

werewolf_protocol_result_t werewolf_protocol_decode(
    const uint8_t *wire,
    size_t wire_len,
    werewolf_frame_t *frame);

#ifdef __cplusplus
}
#endif
