#include "werewolf_protocol.h"

#include <string.h>

#define OFFSET_MAGIC       0U
#define OFFSET_VERSION     2U
#define OFFSET_HEADER_LEN  3U
#define OFFSET_TYPE        4U
#define OFFSET_FLAGS       5U
#define OFFSET_SRC         6U
#define OFFSET_DST         7U
#define OFFSET_SESSION     8U
#define OFFSET_EPOCH       16U
#define OFFSET_MSG_SEQ     20U
#define OFFSET_ACK_SEQ     24U
#define OFFSET_PHASE_SEQ   28U
#define OFFSET_PAYLOAD_LEN 30U
#define OFFSET_ACTION_KEY  32U

static void put_u16_be(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void put_u32_be(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void put_u64_be(uint8_t *dst, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(value >> (56U - (uint32_t)(i * 8U)));
    }
}

static uint16_t get_u16_be(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static uint32_t get_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) |
           src[3];
}

static uint64_t get_u64_be(const uint8_t *src)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | src[i];
    }
    return value;
}

bool werewolf_protocol_type_valid(werewolf_message_type_t type)
{
    return type >= WEREWOLF_MSG_BEACON &&
           type <= WEREWOLF_MSG_PROFILE;
}

bool werewolf_protocol_type_is_discovery(werewolf_message_type_t type)
{
    return type == WEREWOLF_MSG_BEACON || type == WEREWOLF_MSG_JOIN ||
           type == WEREWOLF_MSG_PAIR_HOST_REVEAL ||
           type == WEREWOLF_MSG_PAIR_CLIENT_REVEAL;
}

bool werewolf_protocol_type_requires_encryption(werewolf_message_type_t type)
{
    return werewolf_protocol_type_valid(type) &&
           !werewolf_protocol_type_is_discovery(type);
}

bool werewolf_protocol_type_is_phase_bound(werewolf_message_type_t type)
{
    return type == WEREWOLF_MSG_PRIVATE_ROLE ||
           type == WEREWOLF_MSG_PHASE ||
           type == WEREWOLF_MSG_ACTION;
}

size_t werewolf_protocol_encoded_size(uint16_t payload_len)
{
    if (payload_len > WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE) {
        return 0;
    }
    return WEREWOLF_PROTOCOL_HEADER_SIZE + payload_len +
           WEREWOLF_PROTOCOL_CRC_SIZE;
}

uint16_t werewolf_protocol_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFU;

    if (data == NULL && len != 0) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        crc = (uint16_t)((uint32_t)crc ^ ((uint32_t)data[i] << 8));
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t shifted = (uint32_t)crc << 1;

            crc = (crc & UINT16_C(0x8000)) != 0U
                      ? (uint16_t)(shifted ^ UINT32_C(0x1021))
                      : (uint16_t)shifted;
        }
    }
    return crc;
}

static werewolf_protocol_result_t validate_frame_fields(
    const werewolf_frame_t *frame)
{
    bool is_ack;
    bool is_discovery;

    if (frame == NULL) {
        return WEREWOLF_PROTOCOL_ERR_ARGUMENT;
    }
    if (frame->version != WEREWOLF_PROTOCOL_VERSION) {
        return WEREWOLF_PROTOCOL_ERR_VERSION;
    }
    if (!werewolf_protocol_type_valid(frame->type)) {
        return WEREWOLF_PROTOCOL_ERR_TYPE;
    }
    if ((frame->flags & ~WEREWOLF_FLAG_ALLOWED_MASK) != 0) {
        return WEREWOLF_PROTOCOL_ERR_FLAGS;
    }
    if (frame->payload_len > WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE) {
        return WEREWOLF_PROTOCOL_ERR_LENGTH;
    }
    if (frame->session_id == 0 || frame->epoch == 0 || frame->msg_seq == 0 ||
        frame->src == WEREWOLF_PLAYER_BROADCAST) {
        return WEREWOLF_PROTOCOL_ERR_POLICY;
    }
    if (frame->dst != WEREWOLF_PLAYER_BROADCAST && frame->src == frame->dst) {
        return WEREWOLF_PROTOCOL_ERR_POLICY;
    }

    is_discovery = werewolf_protocol_type_is_discovery(frame->type);
    if (is_discovery != (frame->dst == WEREWOLF_PLAYER_BROADCAST)) {
        return WEREWOLF_PROTOCOL_ERR_POLICY;
    }

    is_ack = frame->type == WEREWOLF_MSG_ACK;
    if (is_ack) {
        if (frame->flags != WEREWOLF_FLAG_IS_ACK || frame->ack_seq == 0 ||
            frame->payload_len != 0 || frame->action_key != 0) {
            return WEREWOLF_PROTOCOL_ERR_POLICY;
        }
    } else if ((frame->flags & WEREWOLF_FLAG_IS_ACK) != 0) {
        return WEREWOLF_PROTOCOL_ERR_FLAGS;
    }

    if (is_discovery && (frame->flags & WEREWOLF_FLAG_ACK_REQUIRED) != 0) {
        return WEREWOLF_PROTOCOL_ERR_POLICY;
    }
    if (frame->type == WEREWOLF_MSG_ACTION) {
        if (frame->action_key == 0) {
            return WEREWOLF_PROTOCOL_ERR_POLICY;
        }
    } else if (frame->action_key != 0) {
        return WEREWOLF_PROTOCOL_ERR_POLICY;
    }
    return WEREWOLF_PROTOCOL_OK;
}

werewolf_protocol_result_t werewolf_protocol_encode(
    const werewolf_frame_t *frame,
    uint8_t *wire,
    size_t wire_capacity,
    size_t *wire_len)
{
    werewolf_protocol_result_t result;
    size_t encoded_len;
    uint16_t crc;

    if (frame == NULL || wire == NULL || wire_len == NULL) {
        return WEREWOLF_PROTOCOL_ERR_ARGUMENT;
    }
    *wire_len = 0;
    result = validate_frame_fields(frame);
    if (result != WEREWOLF_PROTOCOL_OK) {
        return result;
    }
    encoded_len = werewolf_protocol_encoded_size(frame->payload_len);
    if (encoded_len == 0) {
        return WEREWOLF_PROTOCOL_ERR_LENGTH;
    }
    if (wire_capacity < encoded_len) {
        return WEREWOLF_PROTOCOL_ERR_CAPACITY;
    }

    memset(wire, 0, encoded_len);
    put_u16_be(&wire[OFFSET_MAGIC], WEREWOLF_PROTOCOL_MAGIC);
    wire[OFFSET_VERSION] = frame->version;
    wire[OFFSET_HEADER_LEN] = WEREWOLF_PROTOCOL_HEADER_SIZE;
    wire[OFFSET_TYPE] = (uint8_t)frame->type;
    wire[OFFSET_FLAGS] = frame->flags;
    wire[OFFSET_SRC] = frame->src;
    wire[OFFSET_DST] = frame->dst;
    put_u64_be(&wire[OFFSET_SESSION], frame->session_id);
    put_u32_be(&wire[OFFSET_EPOCH], frame->epoch);
    put_u32_be(&wire[OFFSET_MSG_SEQ], frame->msg_seq);
    put_u32_be(&wire[OFFSET_ACK_SEQ], frame->ack_seq);
    put_u16_be(&wire[OFFSET_PHASE_SEQ], frame->phase_seq);
    put_u16_be(&wire[OFFSET_PAYLOAD_LEN], frame->payload_len);
    put_u32_be(&wire[OFFSET_ACTION_KEY], frame->action_key);
    if (frame->payload_len != 0) {
        memcpy(&wire[WEREWOLF_PROTOCOL_HEADER_SIZE], frame->payload,
               frame->payload_len);
    }
    crc = werewolf_protocol_crc16(wire, encoded_len - WEREWOLF_PROTOCOL_CRC_SIZE);
    put_u16_be(&wire[encoded_len - WEREWOLF_PROTOCOL_CRC_SIZE], crc);
    *wire_len = encoded_len;
    return WEREWOLF_PROTOCOL_OK;
}

werewolf_protocol_result_t werewolf_protocol_decode(
    const uint8_t *wire,
    size_t wire_len,
    werewolf_frame_t *frame)
{
    werewolf_protocol_result_t result;
    size_t expected_len;
    uint16_t expected_crc;
    uint16_t actual_crc;

    if (wire == NULL || frame == NULL) {
        return WEREWOLF_PROTOCOL_ERR_ARGUMENT;
    }
    if (wire_len < WEREWOLF_PROTOCOL_HEADER_SIZE + WEREWOLF_PROTOCOL_CRC_SIZE ||
        wire_len > WEREWOLF_PROTOCOL_MAX_FRAME_SIZE) {
        return WEREWOLF_PROTOCOL_ERR_LENGTH;
    }
    if (get_u16_be(&wire[OFFSET_MAGIC]) != WEREWOLF_PROTOCOL_MAGIC) {
        return WEREWOLF_PROTOCOL_ERR_MAGIC;
    }
    if (wire[OFFSET_VERSION] != WEREWOLF_PROTOCOL_VERSION) {
        return WEREWOLF_PROTOCOL_ERR_VERSION;
    }
    if (wire[OFFSET_HEADER_LEN] != WEREWOLF_PROTOCOL_HEADER_SIZE) {
        return WEREWOLF_PROTOCOL_ERR_LENGTH;
    }

    memset(frame, 0, sizeof(*frame));
    frame->version = wire[OFFSET_VERSION];
    frame->type = (werewolf_message_type_t)wire[OFFSET_TYPE];
    frame->flags = wire[OFFSET_FLAGS];
    frame->src = wire[OFFSET_SRC];
    frame->dst = wire[OFFSET_DST];
    frame->session_id = get_u64_be(&wire[OFFSET_SESSION]);
    frame->epoch = get_u32_be(&wire[OFFSET_EPOCH]);
    frame->msg_seq = get_u32_be(&wire[OFFSET_MSG_SEQ]);
    frame->ack_seq = get_u32_be(&wire[OFFSET_ACK_SEQ]);
    frame->phase_seq = get_u16_be(&wire[OFFSET_PHASE_SEQ]);
    frame->payload_len = get_u16_be(&wire[OFFSET_PAYLOAD_LEN]);
    frame->action_key = get_u32_be(&wire[OFFSET_ACTION_KEY]);

    expected_len = werewolf_protocol_encoded_size(frame->payload_len);
    if (expected_len == 0 || expected_len != wire_len) {
        return WEREWOLF_PROTOCOL_ERR_LENGTH;
    }
    expected_crc = werewolf_protocol_crc16(
        wire, wire_len - WEREWOLF_PROTOCOL_CRC_SIZE);
    actual_crc = get_u16_be(&wire[wire_len - WEREWOLF_PROTOCOL_CRC_SIZE]);
    if (expected_crc != actual_crc) {
        return WEREWOLF_PROTOCOL_ERR_CRC;
    }

    result = validate_frame_fields(frame);
    if (result != WEREWOLF_PROTOCOL_OK) {
        return result;
    }
    if (frame->payload_len != 0) {
        memcpy(frame->payload, &wire[WEREWOLF_PROTOCOL_HEADER_SIZE],
               frame->payload_len);
    }
    return WEREWOLF_PROTOCOL_OK;
}
