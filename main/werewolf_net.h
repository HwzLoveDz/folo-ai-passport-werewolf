#pragma once

#include "werewolf_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEREWOLF_NET_MAC_SIZE          6U
#define WEREWOLF_NET_KEY_SIZE          16U
#define WEREWOLF_NET_MAX_PEERS         12U
#define WEREWOLF_NET_RX_WINDOW_BITS    32U
#define WEREWOLF_NET_ACTION_CACHE_SIZE 16U
#define WEREWOLF_NET_PENDING_MAX       12U
#define WEREWOLF_NET_DELIVERY_CACHE_SIZE 16U
#define WEREWOLF_NET_MAX_RETRIES       8U

typedef enum {
    WEREWOLF_NET_ROLE_HOST = 1,
    WEREWOLF_NET_ROLE_CLIENT = 2,
} werewolf_net_role_t;

typedef enum {
    WEREWOLF_NET_OK = 0,
    WEREWOLF_NET_ERR_ARGUMENT = -1,
    WEREWOLF_NET_ERR_STATE = -2,
    WEREWOLF_NET_ERR_CAPACITY = -3,
    WEREWOLF_NET_ERR_SECURITY = -4,
    WEREWOLF_NET_ERR_PROTOCOL = -5,
    WEREWOLF_NET_ERR_TRANSPORT = -6,
    WEREWOLF_NET_ERR_NOT_FOUND = -7,
    WEREWOLF_NET_ERR_UNSUPPORTED = -8,
} werewolf_net_result_t;

typedef enum {
    WEREWOLF_RX_ACCEPT = 0,
    WEREWOLF_RX_ACCEPT_REORDERED = 1,
    WEREWOLF_RX_DUPLICATE = 2,
    WEREWOLF_RX_ACTION_DUPLICATE = 3,
    WEREWOLF_RX_LATE = 4,
    WEREWOLF_RX_WRONG_SESSION = 5,
    WEREWOLF_RX_WRONG_EPOCH = 6,
    WEREWOLF_RX_WRONG_DESTINATION = 7,
    WEREWOLF_RX_NO_SLOT = 8,
    WEREWOLF_RX_INVALID = 9,
} werewolf_rx_result_t;

typedef struct {
    bool used;
    uint8_t src;
    uint32_t highest_seq;
    uint32_t seen_bitmap;
} werewolf_rx_window_t;

typedef struct {
    bool used;
    uint8_t src;
    uint16_t phase_seq;
    uint32_t action_key;
} werewolf_action_cache_entry_t;

typedef struct {
    bool used;
    uint8_t peer_id;
    uint8_t attempts;
    uint32_t peer_generation;
    uint32_t msg_seq;
    uint32_t deadline_ms;
    uint16_t wire_len;
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
} werewolf_pending_tx_t;

typedef enum {
    WEREWOLF_DELIVERY_UNKNOWN = 0,
    WEREWOLF_DELIVERY_PENDING,
    WEREWOLF_DELIVERY_ACKNOWLEDGED,
    WEREWOLF_DELIVERY_FAILED,
} werewolf_delivery_status_t;

typedef struct {
    bool used;
    uint8_t peer_id;
    uint32_t peer_generation;
    uint32_t msg_seq;
    werewolf_delivery_status_t status;
} werewolf_delivery_record_t;

typedef struct {
    uint8_t local_id;
    uint64_t session_id;
    uint32_t epoch;
    uint32_t next_tx_seq;
    uint16_t current_phase_seq;
    uint16_t ack_timeout_ms;
    uint8_t max_retries;
    uint8_t action_cache_cursor;
    uint8_t delivery_cache_cursor;
    werewolf_rx_window_t rx[WEREWOLF_NET_MAX_PEERS];
    werewolf_action_cache_entry_t
        action_cache[WEREWOLF_NET_ACTION_CACHE_SIZE];
    werewolf_pending_tx_t pending[WEREWOLF_NET_PENDING_MAX];
    werewolf_delivery_record_t
        delivery_cache[WEREWOLF_NET_DELIVERY_CACHE_SIZE];
} werewolf_reliable_t;

typedef enum {
    WEREWOLF_RETRY_NONE = 0,
    WEREWOLF_RETRY_SEND = 1,
    WEREWOLF_RETRY_EXHAUSTED = 2,
} werewolf_retry_result_t;

typedef struct {
    uint8_t peer_id;
    uint8_t attempt;
    uint32_t peer_generation;
    uint32_t msg_seq;
    uint16_t wire_len;
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
} werewolf_retry_item_t;

void werewolf_reliable_init(werewolf_reliable_t *reliable,
                            uint8_t local_id,
                            uint64_t session_id,
                            uint32_t epoch,
                            uint32_t first_tx_seq,
                            uint16_t ack_timeout_ms,
                            uint8_t max_retries);
void werewolf_reliable_set_phase(werewolf_reliable_t *reliable,
                                 uint16_t phase_seq);
uint32_t werewolf_reliable_next_seq(werewolf_reliable_t *reliable);
werewolf_rx_result_t werewolf_reliable_observe(
    werewolf_reliable_t *reliable,
    const werewolf_frame_t *frame);
werewolf_net_result_t werewolf_reliable_make_ack(
    werewolf_reliable_t *reliable,
    const werewolf_frame_t *received,
    werewolf_frame_t *ack);
werewolf_net_result_t werewolf_reliable_track(
    werewolf_reliable_t *reliable,
    uint8_t peer_id,
    uint32_t peer_generation,
    const werewolf_frame_t *frame,
    const uint8_t *wire,
    size_t wire_len,
    uint32_t now_ms);
bool werewolf_reliable_ack(werewolf_reliable_t *reliable,
                           uint8_t peer_id,
                           uint32_t ack_seq);
werewolf_delivery_status_t werewolf_reliable_delivery_status(
    const werewolf_reliable_t *reliable,
    uint8_t peer_id,
    uint32_t peer_generation,
    uint32_t msg_seq);
werewolf_retry_result_t werewolf_reliable_poll(
    werewolf_reliable_t *reliable,
    uint32_t now_ms,
    werewolf_retry_item_t *item);
size_t werewolf_reliable_pending_count(const werewolf_reliable_t *reliable);
size_t werewolf_reliable_pending_count_for_peer(
    const werewolf_reliable_t *reliable,
    uint8_t peer_id);
void werewolf_reliable_clear_pending(werewolf_reliable_t *reliable);
void werewolf_reliable_forget_peer(werewolf_reliable_t *reliable,
                                   uint8_t peer_id);

/*
 * Return true only after the frame has been accepted by the upper layer.
 * This callback runs under the transport lock: it must not block or call a
 * werewolf_net_* API. Copy/queue the frame and return immediately.
 */
typedef bool (*werewolf_net_message_cb_t)(const werewolf_frame_t *frame,
                                          const uint8_t source_mac[6],
                                          void *user);
typedef void (*werewolf_net_delivery_failed_cb_t)(uint8_t peer_id,
                                                  uint32_t msg_seq,
                                                  uint64_t session_id,
                                                  uint32_t peer_generation,
                                                  void *user);

typedef struct {
    werewolf_net_role_t role;
    uint8_t local_id;
    uint8_t host_id;
    uint8_t channel;
    uint64_t session_id;
    uint32_t epoch;
    uint16_t ack_timeout_ms;
    uint8_t max_retries;
    const uint8_t *pmk;
    size_t pmk_len;
    werewolf_net_message_cb_t on_message;
    werewolf_net_delivery_failed_cb_t on_delivery_failed;
    void *user;
} werewolf_net_config_t;

typedef struct {
    bool initialized;
    bool game_started;
    bool has_pmk;
    size_t secure_peer_count;
    size_t pending_count;
    bool signal_available;
    int8_t signal_rssi_dbm;
    uint32_t signal_age_ms;
    uint32_t rx_queue_drops;
    uint32_t rx_protocol_errors;
    uint32_t rx_security_drops;
    uint32_t rx_duplicates;
    uint32_t rx_late;
    uint32_t tx_retries;
    uint32_t tx_exhausted;
    uint32_t tx_transport_errors;
} werewolf_net_snapshot_t;

/* ESP-NOW transport. Host builds return WEREWOLF_NET_ERR_UNSUPPORTED. */
werewolf_net_result_t werewolf_net_init(const werewolf_net_config_t *config);
void werewolf_net_deinit(void);
/* Install the non-zero PMK after Wi-Fi/RF has started and before adding any
 * encrypted peer.  A PMK cannot be replaced within an active session. */
werewolf_net_result_t werewolf_net_set_pmk(
    const uint8_t *pmk,
    size_t pmk_len);
werewolf_net_result_t werewolf_net_add_encrypted_peer(
    uint8_t player_id,
    const uint8_t mac[WEREWOLF_NET_MAC_SIZE],
    const uint8_t *lmk,
    size_t lmk_len);
werewolf_net_result_t werewolf_net_remove_peer(uint8_t player_id);
werewolf_net_result_t werewolf_net_get_peer_generation(
    uint8_t player_id,
    uint32_t *peer_generation);
werewolf_net_result_t werewolf_net_begin_game(size_t expected_secure_peers);
void werewolf_net_end_game(void);
void werewolf_net_set_phase(uint16_t phase_seq);
werewolf_net_result_t werewolf_net_broadcast_discovery(
    werewolf_message_type_t type,
    const uint8_t *payload,
    size_t payload_len);
/* OK means the frame was encoded and accepted into the reliable pending
 * queue. A transient failure of the first ESP-NOW send is counted internally
 * and retried; it is not an enqueue failure. */
werewolf_net_result_t werewolf_net_send_unicast(
    uint8_t peer_id,
    werewolf_message_type_t type,
    uint16_t phase_seq,
    uint32_t action_key,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t *out_msg_seq);
werewolf_delivery_status_t werewolf_net_delivery_status(
    uint8_t peer_id,
    uint32_t peer_generation,
    uint32_t msg_seq);
size_t werewolf_net_peer_pending_count(uint8_t peer_id);
void werewolf_net_snapshot(werewolf_net_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
