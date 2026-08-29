#include "werewolf_net.h"

#include <limits.h>
#include <string.h>

/* Volatile stores are used for plaintext frame/key material so whole-program
 * optimization cannot elide cleanup once a retry entry or stack copy dies. */
static void net_secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    while (size-- != 0U) {
        *bytes++ = 0U;
    }
}

static bool seq_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void remember_delivery(werewolf_reliable_t *reliable,
                              uint8_t peer_id,
                              uint32_t peer_generation,
                              uint32_t msg_seq,
                              werewolf_delivery_status_t status)
{
    size_t index;

    if (reliable == NULL || peer_id == WEREWOLF_PLAYER_BROADCAST ||
        peer_generation == 0U || msg_seq == 0U ||
        (status != WEREWOLF_DELIVERY_ACKNOWLEDGED &&
         status != WEREWOLF_DELIVERY_FAILED)) {
        return;
    }
    index = reliable->delivery_cache_cursor++ %
            WEREWOLF_NET_DELIVERY_CACHE_SIZE;
    reliable->delivery_cache[index] = (werewolf_delivery_record_t){
        .used = true,
        .peer_id = peer_id,
        .peer_generation = peer_generation,
        .msg_seq = msg_seq,
        .status = status,
    };
}

static werewolf_rx_window_t *find_rx_window(werewolf_reliable_t *reliable,
                                             uint8_t src)
{
    werewolf_rx_window_t *free_slot = NULL;

    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        if (reliable->rx[i].used && reliable->rx[i].src == src) {
            return &reliable->rx[i];
        }
        if (!reliable->rx[i].used && free_slot == NULL) {
            free_slot = &reliable->rx[i];
        }
    }
    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        free_slot->src = src;
    }
    return free_slot;
}

static bool action_seen(const werewolf_reliable_t *reliable,
                        const werewolf_frame_t *frame)
{
    for (size_t i = 0; i < WEREWOLF_NET_ACTION_CACHE_SIZE; ++i) {
        const werewolf_action_cache_entry_t *entry =
            &reliable->action_cache[i];
        if (entry->used && entry->src == frame->src &&
            entry->phase_seq == frame->phase_seq &&
            entry->action_key == frame->action_key) {
            return true;
        }
    }
    return false;
}

static void remember_action(werewolf_reliable_t *reliable,
                            const werewolf_frame_t *frame)
{
    size_t index = reliable->action_cache_cursor++ %
                   WEREWOLF_NET_ACTION_CACHE_SIZE;
    reliable->action_cache[index] = (werewolf_action_cache_entry_t){
        .used = true,
        .src = frame->src,
        .phase_seq = frame->phase_seq,
        .action_key = frame->action_key,
    };
}

void werewolf_reliable_init(werewolf_reliable_t *reliable,
                            uint8_t local_id,
                            uint64_t session_id,
                            uint32_t epoch,
                            uint32_t first_tx_seq,
                            uint16_t ack_timeout_ms,
                            uint8_t max_retries)
{
    if (reliable == NULL) {
        return;
    }
    net_secure_zero(reliable, sizeof(*reliable));
    reliable->local_id = local_id;
    reliable->session_id = session_id;
    reliable->epoch = epoch;
    reliable->next_tx_seq = first_tx_seq == 0 ? 1 : first_tx_seq;
    reliable->ack_timeout_ms = ack_timeout_ms == 0 ? 150 : ack_timeout_ms;
    reliable->max_retries = max_retries > WEREWOLF_NET_MAX_RETRIES
                                ? WEREWOLF_NET_MAX_RETRIES
                                : max_retries;
}

void werewolf_reliable_set_phase(werewolf_reliable_t *reliable,
                                 uint16_t phase_seq)
{
    if (reliable == NULL) {
        return;
    }
    reliable->current_phase_seq = phase_seq;
}

uint32_t werewolf_reliable_next_seq(werewolf_reliable_t *reliable)
{
    uint32_t result;

    if (reliable == NULL) {
        return 0;
    }
    result = reliable->next_tx_seq++;
    if (result == 0) {
        result = reliable->next_tx_seq++;
    }
    if (reliable->next_tx_seq == 0) {
        reliable->next_tx_seq = 1;
    }
    return result;
}

werewolf_rx_result_t werewolf_reliable_observe(
    werewolf_reliable_t *reliable,
    const werewolf_frame_t *frame)
{
    werewolf_rx_window_t *window;
    werewolf_rx_result_t accepted = WEREWOLF_RX_ACCEPT;
    uint32_t distance;

    if (reliable == NULL || frame == NULL || frame->msg_seq == 0 ||
        !werewolf_protocol_type_valid(frame->type)) {
        return WEREWOLF_RX_INVALID;
    }
    if (frame->session_id != reliable->session_id) {
        return WEREWOLF_RX_WRONG_SESSION;
    }
    if (frame->epoch != reliable->epoch) {
        return WEREWOLF_RX_WRONG_EPOCH;
    }
    if (frame->dst != reliable->local_id &&
        frame->dst != WEREWOLF_PLAYER_BROADCAST) {
        return WEREWOLF_RX_WRONG_DESTINATION;
    }
    if (werewolf_protocol_type_is_phase_bound(frame->type) &&
        frame->phase_seq < reliable->current_phase_seq) {
        return WEREWOLF_RX_LATE;
    }

    window = find_rx_window(reliable, frame->src);
    if (window == NULL) {
        return WEREWOLF_RX_NO_SLOT;
    }
    if (window->highest_seq == 0) {
        window->highest_seq = frame->msg_seq;
        window->seen_bitmap = 1U;
    } else if (seq_newer(frame->msg_seq, window->highest_seq)) {
        distance = frame->msg_seq - window->highest_seq;
        window->seen_bitmap = distance >= WEREWOLF_NET_RX_WINDOW_BITS
                                  ? 1U
                                  : (window->seen_bitmap << distance) | 1U;
        window->highest_seq = frame->msg_seq;
    } else {
        distance = window->highest_seq - frame->msg_seq;
        if (distance >= WEREWOLF_NET_RX_WINDOW_BITS) {
            return WEREWOLF_RX_LATE;
        }
        if ((window->seen_bitmap & (UINT32_C(1) << distance)) != 0U) {
            return WEREWOLF_RX_DUPLICATE;
        }
        window->seen_bitmap |= UINT32_C(1) << distance;
        accepted = WEREWOLF_RX_ACCEPT_REORDERED;
    }

    if (frame->type == WEREWOLF_MSG_ACTION) {
        if (action_seen(reliable, frame)) {
            return WEREWOLF_RX_ACTION_DUPLICATE;
        }
        remember_action(reliable, frame);
    }
    return accepted;
}

werewolf_net_result_t werewolf_reliable_make_ack(
    werewolf_reliable_t *reliable,
    const werewolf_frame_t *received,
    werewolf_frame_t *ack)
{
    if (reliable == NULL || received == NULL || ack == NULL ||
        received->msg_seq == 0 ||
        received->src == WEREWOLF_PLAYER_BROADCAST) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    memset(ack, 0, sizeof(*ack));
    ack->version = WEREWOLF_PROTOCOL_VERSION;
    ack->type = WEREWOLF_MSG_ACK;
    ack->flags = WEREWOLF_FLAG_IS_ACK;
    ack->src = reliable->local_id;
    ack->dst = received->src;
    ack->session_id = reliable->session_id;
    ack->epoch = reliable->epoch;
    ack->msg_seq = werewolf_reliable_next_seq(reliable);
    ack->ack_seq = received->msg_seq;
    ack->phase_seq = received->phase_seq;
    return WEREWOLF_NET_OK;
}

werewolf_net_result_t werewolf_reliable_track(
    werewolf_reliable_t *reliable,
    uint8_t peer_id,
    uint32_t peer_generation,
    const werewolf_frame_t *frame,
    const uint8_t *wire,
    size_t wire_len,
    uint32_t now_ms)
{
    werewolf_pending_tx_t *free_slot = NULL;

    if (reliable == NULL || frame == NULL || wire == NULL ||
        wire_len == 0 || wire_len > WEREWOLF_PROTOCOL_MAX_FRAME_SIZE ||
        peer_id == WEREWOLF_PLAYER_BROADCAST || peer_generation == 0 ||
        frame->msg_seq == 0 ||
        (frame->flags & WEREWOLF_FLAG_ACK_REQUIRED) == 0) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        werewolf_pending_tx_t *pending = &reliable->pending[i];
        if (pending->used && pending->peer_id == peer_id &&
            pending->msg_seq == frame->msg_seq) {
            return WEREWOLF_NET_ERR_STATE;
        }
        if (!pending->used && free_slot == NULL) {
            free_slot = pending;
        }
    }
    if (free_slot == NULL) {
        return WEREWOLF_NET_ERR_CAPACITY;
    }
    for (size_t i = 0; i < WEREWOLF_NET_DELIVERY_CACHE_SIZE; ++i) {
        werewolf_delivery_record_t *record =
            &reliable->delivery_cache[i];
        if (record->used && record->peer_id == peer_id &&
            record->peer_generation == peer_generation &&
            record->msg_seq == frame->msg_seq) {
            memset(record, 0, sizeof(*record));
        }
    }
    net_secure_zero(free_slot, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->peer_id = peer_id;
    free_slot->attempts = 1;
    free_slot->peer_generation = peer_generation;
    free_slot->msg_seq = frame->msg_seq;
    free_slot->deadline_ms = now_ms + reliable->ack_timeout_ms;
    free_slot->wire_len = (uint16_t)wire_len;
    memcpy(free_slot->wire, wire, wire_len);
    return WEREWOLF_NET_OK;
}

bool werewolf_reliable_ack(werewolf_reliable_t *reliable,
                           uint8_t peer_id,
                           uint32_t ack_seq)
{
    if (reliable == NULL || ack_seq == 0) {
        return false;
    }
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        werewolf_pending_tx_t *pending = &reliable->pending[i];
        if (pending->used && pending->peer_id == peer_id &&
            pending->msg_seq == ack_seq) {
            remember_delivery(reliable, pending->peer_id,
                              pending->peer_generation,
                              pending->msg_seq,
                              WEREWOLF_DELIVERY_ACKNOWLEDGED);
            net_secure_zero(pending, sizeof(*pending));
            return true;
        }
    }
    return false;
}

werewolf_delivery_status_t werewolf_reliable_delivery_status(
    const werewolf_reliable_t *reliable,
    uint8_t peer_id,
    uint32_t peer_generation,
    uint32_t msg_seq)
{
    if (reliable == NULL || peer_id == WEREWOLF_PLAYER_BROADCAST ||
        peer_generation == 0U || msg_seq == 0U) {
        return WEREWOLF_DELIVERY_UNKNOWN;
    }
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        const werewolf_pending_tx_t *pending = &reliable->pending[i];
        if (pending->used && pending->peer_id == peer_id &&
            pending->peer_generation == peer_generation &&
            pending->msg_seq == msg_seq) {
            return WEREWOLF_DELIVERY_PENDING;
        }
    }
    for (size_t offset = 0; offset < WEREWOLF_NET_DELIVERY_CACHE_SIZE;
         ++offset) {
        size_t index = (size_t)(reliable->delivery_cache_cursor - 1U -
                                (uint8_t)offset) %
                       WEREWOLF_NET_DELIVERY_CACHE_SIZE;
        const werewolf_delivery_record_t *record =
            &reliable->delivery_cache[index];
        if (record->used && record->peer_id == peer_id &&
            record->peer_generation == peer_generation &&
            record->msg_seq == msg_seq) {
            return record->status;
        }
    }
    return WEREWOLF_DELIVERY_UNKNOWN;
}

werewolf_retry_result_t werewolf_reliable_poll(
    werewolf_reliable_t *reliable,
    uint32_t now_ms,
    werewolf_retry_item_t *item)
{
    if (reliable == NULL || item == NULL) {
        return WEREWOLF_RETRY_NONE;
    }
    net_secure_zero(item, sizeof(*item));
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        werewolf_pending_tx_t *pending = &reliable->pending[i];
        uint32_t backoff;
        unsigned shift;

        if (!pending->used || !time_reached(now_ms, pending->deadline_ms)) {
            continue;
        }
        item->peer_id = pending->peer_id;
        item->attempt = pending->attempts;
        item->peer_generation = pending->peer_generation;
        item->msg_seq = pending->msg_seq;
        if (pending->attempts >= (uint8_t)(1U + reliable->max_retries)) {
            remember_delivery(reliable, pending->peer_id,
                              pending->peer_generation,
                              pending->msg_seq,
                              WEREWOLF_DELIVERY_FAILED);
            net_secure_zero(pending, sizeof(*pending));
            return WEREWOLF_RETRY_EXHAUSTED;
        }

        ++pending->attempts;
        item->attempt = pending->attempts;
        item->wire_len = pending->wire_len;
        memcpy(item->wire, pending->wire, pending->wire_len);
        shift = pending->attempts > 5U ? 4U : (unsigned)pending->attempts - 1U;
        backoff = (uint32_t)reliable->ack_timeout_ms << shift;
        pending->deadline_ms = now_ms + backoff;
        return WEREWOLF_RETRY_SEND;
    }
    return WEREWOLF_RETRY_NONE;
}

size_t werewolf_reliable_pending_count(const werewolf_reliable_t *reliable)
{
    size_t count = 0;

    if (reliable == NULL) {
        return 0;
    }
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        if (reliable->pending[i].used) {
            ++count;
        }
    }
    return count;
}

size_t werewolf_reliable_pending_count_for_peer(
    const werewolf_reliable_t *reliable,
    uint8_t peer_id)
{
    size_t count = 0U;

    if (reliable == NULL || peer_id == WEREWOLF_PLAYER_BROADCAST) {
        return 0U;
    }
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        if (reliable->pending[i].used &&
            reliable->pending[i].peer_id == peer_id) {
            ++count;
        }
    }
    return count;
}

void werewolf_reliable_clear_pending(werewolf_reliable_t *reliable)
{
    if (reliable != NULL) {
        net_secure_zero(reliable->pending, sizeof(reliable->pending));
    }
}

void werewolf_reliable_forget_peer(werewolf_reliable_t *reliable,
                                   uint8_t peer_id)
{
    if (reliable == NULL || peer_id == WEREWOLF_PLAYER_BROADCAST) {
        return;
    }
    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        if (reliable->rx[i].used && reliable->rx[i].src == peer_id) {
            memset(&reliable->rx[i], 0, sizeof(reliable->rx[i]));
        }
    }
    for (size_t i = 0; i < WEREWOLF_NET_ACTION_CACHE_SIZE; ++i) {
        if (reliable->action_cache[i].used &&
            reliable->action_cache[i].src == peer_id) {
            memset(&reliable->action_cache[i], 0,
                   sizeof(reliable->action_cache[i]));
        }
    }
    for (size_t i = 0; i < WEREWOLF_NET_PENDING_MAX; ++i) {
        if (reliable->pending[i].used &&
            reliable->pending[i].peer_id == peer_id) {
            net_secure_zero(&reliable->pending[i],
                            sizeof(reliable->pending[i]));
        }
    }
    for (size_t i = 0; i < WEREWOLF_NET_DELIVERY_CACHE_SIZE; ++i) {
        if (reliable->delivery_cache[i].used &&
            reliable->delivery_cache[i].peer_id == peer_id) {
            memset(&reliable->delivery_cache[i], 0,
                   sizeof(reliable->delivery_cache[i]));
        }
    }
}

#ifdef ESP_PLATFORM

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define NET_EVENT_QUEUE_DEPTH 12U
#define NET_TASK_STACK_BYTES  4096U
#define NET_TASK_PRIORITY     5U
#define NET_POLL_MS           20U

typedef enum {
    NET_EVENT_RX,
    NET_EVENT_TX_STATUS,
} net_event_kind_t;

typedef struct {
    net_event_kind_t kind;
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    bool broadcast;
    bool rssi_available;
    int8_t rssi_dbm;
    uint16_t wire_len;
    esp_now_send_status_t tx_status;
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
} net_event_t;

typedef struct {
    bool used;
    bool encrypted;
    uint8_t player_id;
    uint32_t generation;
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    bool rssi_available;
    int8_t rssi_dbm;
    uint32_t rssi_updated_ms;
} net_peer_t;

static const char *TAG = "werewolf_net";
static const uint8_t s_broadcast_mac[WEREWOLF_NET_MAC_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static werewolf_net_config_t s_config;
static werewolf_reliable_t s_reliable;
static net_peer_t s_peers[WEREWOLF_NET_MAX_PEERS];
static werewolf_net_snapshot_t s_stats;
static uint8_t s_pmk[WEREWOLF_NET_KEY_SIZE];
static bool s_running;
static uint32_t s_next_peer_generation;
static bool s_wifi_initialized;
static bool s_espnow_initialized;
static bool s_send_cb_registered;
static bool s_recv_cb_registered;
static bool s_callbacks_enabled;
static QueueHandle_t s_event_queue;
static StaticQueue_t s_event_queue_buffer;
static uint8_t s_event_queue_storage[NET_EVENT_QUEUE_DEPTH *
                                     sizeof(net_event_t)];
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_buffer;
static TaskHandle_t s_task;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[NET_TASK_STACK_BYTES / sizeof(StackType_t)];
static bool s_task_stopped;
static volatile uint32_t s_callback_queue_drops;

static bool net_task_running(void)
{
    return __atomic_load_n(&s_running, __ATOMIC_ACQUIRE);
}

static void net_task_set_running(bool running)
{
    __atomic_store_n(&s_running, running, __ATOMIC_RELEASE);
}

static TaskHandle_t net_task_handle(void)
{
    return __atomic_load_n(&s_task, __ATOMIC_ACQUIRE);
}

static void net_task_set_handle(TaskHandle_t task)
{
    __atomic_store_n(&s_task, task, __ATOMIC_RELEASE);
}

static bool net_task_stopped(void)
{
    return __atomic_load_n(&s_task_stopped, __ATOMIC_ACQUIRE);
}

static void net_task_set_stopped(bool stopped)
{
    __atomic_store_n(&s_task_stopped, stopped, __ATOMIC_RELEASE);
}

static void lock_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock_state(void)
{
    xSemaphoreGive(s_mutex);
}

static bool key_is_nonzero(const uint8_t *key, size_t len)
{
    uint8_t aggregate = 0;

    if (key == NULL) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        aggregate |= key[i];
    }
    return aggregate != 0;
}

static bool mac_is_broadcast(const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    return mac != NULL &&
           memcmp(mac, s_broadcast_mac, WEREWOLF_NET_MAC_SIZE) == 0;
}

static net_peer_t *peer_by_id(uint8_t player_id)
{
    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        if (s_peers[i].used && s_peers[i].player_id == player_id) {
            return &s_peers[i];
        }
    }
    return NULL;
}

static net_peer_t *peer_by_mac_and_id(
    const uint8_t mac[WEREWOLF_NET_MAC_SIZE], uint8_t player_id)
{
    net_peer_t *peer = peer_by_id(player_id);
    if (peer != NULL &&
        memcmp(peer->mac, mac, WEREWOLF_NET_MAC_SIZE) == 0) {
        return peer;
    }
    return NULL;
}

static size_t secure_peer_count_locked(void)
{
    size_t count = 0;
    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        if (s_peers[i].used && s_peers[i].encrypted) {
            ++count;
        }
    }
    return count;
}

static uint32_t next_peer_generation_locked(void)
{
    ++s_next_peer_generation;
    if (s_next_peer_generation == 0U) {
        ++s_next_peer_generation;
    }
    return s_next_peer_generation;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void update_error_counter(uint32_t *counter)
{
    lock_state();
    ++*counter;
    unlock_state();
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data,
                           int data_len)
{
    net_event_t event = {0};

    if (!__atomic_load_n(&s_callbacks_enabled, __ATOMIC_ACQUIRE) ||
        s_event_queue == NULL || info == NULL || info->src_addr == NULL ||
        info->des_addr == NULL || data == NULL || data_len <= 0 ||
        data_len > (int)WEREWOLF_PROTOCOL_MAX_FRAME_SIZE) {
        __atomic_add_fetch(&s_callback_queue_drops, 1U, __ATOMIC_RELAXED);
        return;
    }
    event.kind = NET_EVENT_RX;
    event.broadcast = mac_is_broadcast(info->des_addr);
    event.rssi_available = info->rx_ctrl != NULL;
    if (event.rssi_available) {
        event.rssi_dbm = info->rx_ctrl->rssi;
    }
    event.wire_len = (uint16_t)data_len;
    memcpy(event.mac, info->src_addr, WEREWOLF_NET_MAC_SIZE);
    memcpy(event.wire, data, (size_t)data_len);
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        __atomic_add_fetch(&s_callback_queue_drops, 1U, __ATOMIC_RELAXED);
    }
    net_secure_zero(&event, sizeof(event));
}

static void espnow_send_cb(const esp_now_send_info_t *info,
                           esp_now_send_status_t status)
{
    net_event_t event = {0};

    if (!__atomic_load_n(&s_callbacks_enabled, __ATOMIC_ACQUIRE) ||
        s_event_queue == NULL || info == NULL || info->des_addr == NULL) {
        return;
    }
    event.kind = NET_EVENT_TX_STATUS;
    event.tx_status = status;
    memcpy(event.mac, info->des_addr, WEREWOLF_NET_MAC_SIZE);
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        __atomic_add_fetch(&s_callback_queue_drops, 1U, __ATOMIC_RELAXED);
    }
    net_secure_zero(&event, sizeof(event));
}

static void send_ack_for(const werewolf_frame_t *received,
                         const net_peer_t *expected_peer)
{
    werewolf_frame_t ack;
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    size_t wire_len = 0;
    net_peer_t *current_peer;

    lock_state();
    current_peer = peer_by_mac_and_id(expected_peer->mac,
                                      expected_peer->player_id);
    if (current_peer != NULL && current_peer->encrypted &&
        current_peer->generation == expected_peer->generation &&
        werewolf_reliable_make_ack(&s_reliable, received, &ack) ==
            WEREWOLF_NET_OK &&
        werewolf_protocol_encode(&ack, wire, sizeof(wire), &wire_len) ==
            WEREWOLF_PROTOCOL_OK) {
        if (esp_now_send(current_peer->mac, wire, wire_len) != ESP_OK) {
            ++s_stats.tx_transport_errors;
        }
    }
    unlock_state();
}

static bool rx_result_needs_ack(werewolf_rx_result_t result)
{
    return result == WEREWOLF_RX_ACCEPT ||
           result == WEREWOLF_RX_ACCEPT_REORDERED ||
           result == WEREWOLF_RX_DUPLICATE ||
           result == WEREWOLF_RX_ACTION_DUPLICATE;
}

static bool rx_result_deliverable(werewolf_rx_result_t result)
{
    return result == WEREWOLF_RX_ACCEPT ||
           result == WEREWOLF_RX_ACCEPT_REORDERED;
}

static void process_rx_event(const net_event_t *event)
{
    werewolf_frame_t frame = {0};
    werewolf_protocol_result_t protocol_result;
    werewolf_rx_result_t rx_result;
    net_peer_t *peer = NULL;
    net_peer_t peer_copy = {0};
    werewolf_rx_window_t rx_before = {0};
    werewolf_action_cache_entry_t action_before = {0};
    size_t rx_index = WEREWOLF_NET_MAX_PEERS;
    size_t action_index = 0U;
    uint8_t action_cursor_before = 0U;
    bool discovery;
    bool deliver = false;
    bool upper_accepted = false;
    bool state_locked = false;

    protocol_result = werewolf_protocol_decode(event->wire, event->wire_len,
                                                &frame);
    if (protocol_result != WEREWOLF_PROTOCOL_OK) {
        update_error_counter(&s_stats.rx_protocol_errors);
        goto cleanup;
    }
    discovery = werewolf_protocol_type_is_discovery(frame.type);
    if (discovery) {
        if (!event->broadcast) {
            update_error_counter(&s_stats.rx_security_drops);
            goto cleanup;
        }
        if (s_config.on_message != NULL) {
            upper_accepted = s_config.on_message(&frame, event->mac,
                                                  s_config.user);
        }
        if (!upper_accepted) {
            update_error_counter(&s_stats.rx_queue_drops);
        }
        goto cleanup;
    }
    if (event->broadcast) {
        update_error_counter(&s_stats.rx_security_drops);
        goto cleanup;
    }

    lock_state();
    state_locked = true;
    peer = peer_by_mac_and_id(event->mac, frame.src);
    if (peer == NULL || !peer->encrypted) {
        ++s_stats.rx_security_drops;
        goto cleanup;
    }
    peer_copy = *peer;
    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        if (s_reliable.rx[i].used && s_reliable.rx[i].src == frame.src) {
            rx_index = i;
            break;
        }
        if (!s_reliable.rx[i].used && rx_index == WEREWOLF_NET_MAX_PEERS) {
            rx_index = i;
        }
    }
    if (rx_index != WEREWOLF_NET_MAX_PEERS) {
        rx_before = s_reliable.rx[rx_index];
    }
    action_cursor_before = s_reliable.action_cache_cursor;
    action_index = action_cursor_before % WEREWOLF_NET_ACTION_CACHE_SIZE;
    action_before = s_reliable.action_cache[action_index];
    rx_result = werewolf_reliable_observe(&s_reliable, &frame);
    if (event->rssi_available && rx_result_needs_ack(rx_result)) {
        peer->rssi_available = true;
        peer->rssi_dbm = event->rssi_dbm;
        peer->rssi_updated_ms = now_ms();
    }
    if (rx_result == WEREWOLF_RX_DUPLICATE ||
        rx_result == WEREWOLF_RX_ACTION_DUPLICATE) {
        ++s_stats.rx_duplicates;
    } else if (rx_result == WEREWOLF_RX_LATE) {
        ++s_stats.rx_late;
    }
    if (rx_result_deliverable(rx_result) && frame.type == WEREWOLF_MSG_ACK) {
        (void)werewolf_reliable_ack(&s_reliable, frame.src, frame.ack_seq);
    }
    if (rx_result_deliverable(rx_result) && frame.type != WEREWOLF_MSG_ACK) {
        deliver = true;
    }

    if (!deliver) {
        unlock_state();
        state_locked = false;
        if ((frame.flags & WEREWOLF_FLAG_ACK_REQUIRED) != 0 &&
            rx_result_needs_ack(rx_result)) {
            send_ack_for(&frame, &peer_copy);
        }
        goto cleanup;
    }

    if (s_config.on_message != NULL) {
        upper_accepted = s_config.on_message(&frame, event->mac, s_config.user);
    }
    if (!upper_accepted) {
        if (rx_index != WEREWOLF_NET_MAX_PEERS) {
            s_reliable.rx[rx_index] = rx_before;
        }
        if (frame.type == WEREWOLF_MSG_ACTION) {
            s_reliable.action_cache[action_index] = action_before;
            s_reliable.action_cache_cursor = action_cursor_before;
        }
        ++s_stats.rx_queue_drops;
        goto cleanup;
    }
    if (frame.ack_seq != 0) {
        (void)werewolf_reliable_ack(&s_reliable, frame.src, frame.ack_seq);
    }
    if (frame.type == WEREWOLF_MSG_START) {
        s_stats.game_started = true;
    } else if (frame.type == WEREWOLF_MSG_ABORT) {
        s_stats.game_started = false;
    }
    unlock_state();
    state_locked = false;

    if ((frame.flags & WEREWOLF_FLAG_ACK_REQUIRED) != 0) {
        send_ack_for(&frame, &peer_copy);
    }

cleanup:
    if (state_locked) {
        unlock_state();
    }
    net_secure_zero(&frame, sizeof(frame));
    net_secure_zero(&peer_copy, sizeof(peer_copy));
    net_secure_zero(&rx_before, sizeof(rx_before));
    net_secure_zero(&action_before, sizeof(action_before));
}

static void process_retry(void)
{
    werewolf_retry_item_t item = {0};
    werewolf_retry_result_t result;

    lock_state();
    result = werewolf_reliable_poll(&s_reliable, now_ms(), &item);
    if (result == WEREWOLF_RETRY_SEND) {
        net_peer_t *peer = peer_by_id(item.peer_id);
        if (peer == NULL || !peer->encrypted ||
            peer->generation != item.peer_generation ||
            esp_now_send(peer->mac, item.wire, item.wire_len) != ESP_OK) {
            ++s_stats.tx_transport_errors;
        }
        ++s_stats.tx_retries;
    } else if (result == WEREWOLF_RETRY_EXHAUSTED) {
        ++s_stats.tx_exhausted;
    }
    unlock_state();

    if (result == WEREWOLF_RETRY_EXHAUSTED &&
               s_config.on_delivery_failed != NULL) {
        s_config.on_delivery_failed(item.peer_id, item.msg_seq,
                                    s_config.session_id,
                                    item.peer_generation, s_config.user);
    }
    net_secure_zero(&item, sizeof(item));
}

static void net_task(void *arg)
{
    net_event_t event = {0};
    (void)arg;

    while (net_task_running()) {
        if (xQueueReceive(s_event_queue, &event,
                          pdMS_TO_TICKS(NET_POLL_MS)) == pdTRUE) {
            if (event.kind == NET_EVENT_RX) {
                process_rx_event(&event);
            } else if (event.kind == NET_EVENT_TX_STATUS &&
                       event.tx_status != ESP_NOW_SEND_SUCCESS) {
                update_error_counter(&s_stats.tx_transport_errors);
            }
        }
        net_secure_zero(&event, sizeof(event));
        process_retry();
    }
    /* A self-deleting static task remains on FreeRTOS' termination list until
     * Idle removes it.  Publishing a NULL handle before that cleanup allowed
     * werewolf_net_init() to reuse s_task_buffer/s_task_stack too early and
     * corrupt the kernel list during rapid scan/cancel/retry cycles.  Suspend
     * here, then let werewolf_net_deinit() delete this non-running task
     * synchronously before the static storage can be reused. */
    net_task_set_stopped(true);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void radio_disable_callbacks(void)
{
    __atomic_store_n(&s_callbacks_enabled, false, __ATOMIC_RELEASE);
    if (s_recv_cb_registered) {
        (void)esp_now_unregister_recv_cb();
        s_recv_cb_registered = false;
    }
    if (s_send_cb_registered) {
        (void)esp_now_unregister_send_cb();
        s_send_cb_registered = false;
    }
}

static void radio_cleanup(void)
{
    radio_disable_callbacks();
    if (s_espnow_initialized) {
        (void)esp_now_deinit();
        s_espnow_initialized = false;
    }
    if (s_wifi_initialized) {
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
}

static werewolf_net_result_t radio_init(uint8_t channel)
{
    esp_err_t err;
    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_now_peer_info_t broadcast_peer = {0};

    err = nvs_flash_init();
    if (err != ESP_OK) {
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) {
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    s_wifi_initialized = true;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK ||
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        radio_cleanup();
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    if (esp_now_init() != ESP_OK) {
        radio_cleanup();
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    s_espnow_initialized = true;
    if (esp_now_register_send_cb(espnow_send_cb) != ESP_OK) {
        radio_cleanup();
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    s_send_cb_registered = true;
    if (esp_now_register_recv_cb(espnow_recv_cb) != ESP_OK) {
        radio_cleanup();
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    s_recv_cb_registered = true;
    if (s_stats.has_pmk && esp_now_set_pmk(s_pmk) != ESP_OK) {
        radio_cleanup();
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    memcpy(broadcast_peer.peer_addr, s_broadcast_mac,
           WEREWOLF_NET_MAC_SIZE);
    broadcast_peer.channel = channel;
    broadcast_peer.ifidx = WIFI_IF_STA;
    broadcast_peer.encrypt = false;
    if (esp_now_add_peer(&broadcast_peer) != ESP_OK) {
        radio_cleanup();
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    __atomic_store_n(&s_callbacks_enabled, true, __ATOMIC_RELEASE);
    return WEREWOLF_NET_OK;
}

static void reset_runtime_state(void)
{
    if (s_event_queue != NULL) {
        (void)xQueueReset(s_event_queue);
    }
    net_secure_zero(s_event_queue_storage, sizeof(s_event_queue_storage));
    net_secure_zero(s_pmk, sizeof(s_pmk));
    memset(s_peers, 0, sizeof(s_peers));
    net_secure_zero(&s_reliable, sizeof(s_reliable));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_config, 0, sizeof(s_config));
    __atomic_store_n(&s_callback_queue_drops, 0U, __ATOMIC_RELAXED);
}

static void rollback_init(void)
{
    __atomic_store_n(&s_callbacks_enabled, false, __ATOMIC_RELEASE);
    net_task_set_running(false);
    radio_cleanup();
    reset_runtime_state();
}

werewolf_net_result_t werewolf_net_init(const werewolf_net_config_t *config)
{
    uint32_t first_seq;
    werewolf_net_result_t result;

    if (config == NULL ||
        (config->role != WEREWOLF_NET_ROLE_HOST &&
         config->role != WEREWOLF_NET_ROLE_CLIENT) ||
        config->channel == 0 || config->channel > 14 ||
        config->session_id == 0 || config->epoch == 0 ||
        config->local_id == WEREWOLF_PLAYER_BROADCAST ||
        config->host_id == WEREWOLF_PLAYER_BROADCAST ||
        config->max_retries > WEREWOLF_NET_MAX_RETRIES) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    if ((config->pmk == NULL) != (config->pmk_len == 0)) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    if (config->pmk != NULL &&
        (config->pmk_len != WEREWOLF_NET_KEY_SIZE ||
         !key_is_nonzero(config->pmk, config->pmk_len))) {
        return WEREWOLF_NET_ERR_SECURITY;
    }
    if (s_stats.initialized) {
        return WEREWOLF_NET_ERR_STATE;
    }

    memset(&s_config, 0, sizeof(s_config));
    memset(&s_reliable, 0, sizeof(s_reliable));
    memset(s_peers, 0, sizeof(s_peers));
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_pmk, 0, sizeof(s_pmk));
    __atomic_store_n(&s_callback_queue_drops, 0U, __ATOMIC_RELAXED);
    s_config = *config;
    s_config.pmk = NULL;
    s_config.pmk_len = 0;
    if (config->pmk != NULL) {
        memcpy(s_pmk, config->pmk, WEREWOLF_NET_KEY_SIZE);
        s_stats.has_pmk = true;
    }
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buffer);
    }
    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreateStatic(
            NET_EVENT_QUEUE_DEPTH, sizeof(net_event_t), s_event_queue_storage,
            &s_event_queue_buffer);
    } else {
        (void)xQueueReset(s_event_queue);
    }
    if (s_mutex == NULL || s_event_queue == NULL) {
        reset_runtime_state();
        return WEREWOLF_NET_ERR_CAPACITY;
    }
    first_seq = esp_random();
    werewolf_reliable_init(&s_reliable, config->local_id, config->session_id,
                            config->epoch, first_seq,
                            config->ack_timeout_ms, config->max_retries);
    result = radio_init(config->channel);
    if (result != WEREWOLF_NET_OK) {
        rollback_init();
        return result;
    }
    net_task_set_running(true);
    net_task_set_stopped(false);
    TaskHandle_t task = xTaskCreateStatic(
        net_task, "werewolf_net",
        sizeof(s_task_stack), NULL,
        NET_TASK_PRIORITY, s_task_stack, &s_task_buffer);
    net_task_set_handle(task);
    if (task == NULL) {
        rollback_init();
        return WEREWOLF_NET_ERR_CAPACITY;
    }
    s_stats.initialized = true;
    ESP_LOGI(TAG, "ESP-NOW ready on channel %u as %s", config->channel,
             config->role == WEREWOLF_NET_ROLE_HOST ? "host" : "client");
    return WEREWOLF_NET_OK;
}

void werewolf_net_deinit(void)
{
    if (!s_stats.initialized) {
        return;
    }
    /* Stop new callback queue writes before the worker or queue is touched. */
    radio_disable_callbacks();
    net_task_set_running(false);
    /*
     * Join the worker instead of force-deleting it. Force deletion can strand
     * s_mutex in the taken state if deinit races a receive/retry critical
     * section. The worker has only a bounded queue wait and callbacks are
     * contractually non-blocking.
     */
    while (!net_task_stopped()) {
        vTaskDelay(pdMS_TO_TICKS(NET_POLL_MS));
    }
    TaskHandle_t task = net_task_handle();
    if (task != NULL) {
        vTaskDelete(task);
        net_task_set_handle(NULL);
    }
    radio_cleanup();
    /* Static queue/mutex storage stays allocated, avoiding callback UAF. */
    reset_runtime_state();
}

werewolf_net_result_t werewolf_net_set_pmk(const uint8_t *pmk,
                                           size_t pmk_len)
{
    esp_err_t err;

    if (pmk == NULL || pmk_len != WEREWOLF_NET_KEY_SIZE) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    if (!key_is_nonzero(pmk, pmk_len)) {
        return WEREWOLF_NET_ERR_SECURITY;
    }
    if (!s_stats.initialized) {
        return WEREWOLF_NET_ERR_STATE;
    }

    lock_state();
    if (s_stats.has_pmk || s_stats.game_started ||
        secure_peer_count_locked() != 0U) {
        unlock_state();
        return WEREWOLF_NET_ERR_STATE;
    }
    err = esp_now_set_pmk(pmk);
    if (err == ESP_OK) {
        memcpy(s_pmk, pmk, sizeof(s_pmk));
        s_stats.has_pmk = true;
    }
    unlock_state();
    return err == ESP_OK ? WEREWOLF_NET_OK : WEREWOLF_NET_ERR_TRANSPORT;
}

werewolf_net_result_t werewolf_net_add_encrypted_peer(
    uint8_t player_id,
    const uint8_t mac[WEREWOLF_NET_MAC_SIZE],
    const uint8_t *lmk,
    size_t lmk_len)
{
    net_peer_t *slot = NULL;
    esp_now_peer_info_t peer = {0};
    esp_err_t err;

    if (!s_stats.initialized) {
        return WEREWOLF_NET_ERR_STATE;
    }
    if (player_id == WEREWOLF_PLAYER_BROADCAST ||
        player_id == s_config.local_id || mac == NULL || lmk == NULL ||
        mac_is_broadcast(mac) || lmk_len != WEREWOLF_NET_KEY_SIZE ||
        !key_is_nonzero(lmk, lmk_len)) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    if (!s_stats.has_pmk) {
        return WEREWOLF_NET_ERR_SECURITY;
    }
    if (s_config.role == WEREWOLF_NET_ROLE_CLIENT &&
        player_id != s_config.host_id) {
        return WEREWOLF_NET_ERR_SECURITY;
    }

    lock_state();
    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        if (s_peers[i].used &&
            (s_peers[i].player_id == player_id ||
             memcmp(s_peers[i].mac, mac, WEREWOLF_NET_MAC_SIZE) == 0)) {
            if (s_peers[i].player_id != player_id ||
                memcmp(s_peers[i].mac, mac, WEREWOLF_NET_MAC_SIZE) != 0) {
                unlock_state();
                return WEREWOLF_NET_ERR_STATE;
            }
            slot = &s_peers[i];
            break;
        }
        if (!s_peers[i].used && slot == NULL) {
            slot = &s_peers[i];
        }
    }
    if (slot == NULL) {
        unlock_state();
        return WEREWOLF_NET_ERR_CAPACITY;
    }
    if (s_config.role == WEREWOLF_NET_ROLE_CLIENT &&
        secure_peer_count_locked() != 0 && !slot->used) {
        unlock_state();
        return WEREWOLF_NET_ERR_CAPACITY;
    }

    memcpy(peer.peer_addr, mac, WEREWOLF_NET_MAC_SIZE);
    memcpy(peer.lmk, lmk, WEREWOLF_NET_KEY_SIZE);
    peer.channel = s_config.channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    err = esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&peer)
                                     : esp_now_add_peer(&peer);
    if (err == ESP_OK) {
        werewolf_reliable_forget_peer(&s_reliable, player_id);
        slot->used = true;
        slot->encrypted = true;
        slot->player_id = player_id;
        slot->generation = next_peer_generation_locked();
        memcpy(slot->mac, mac, WEREWOLF_NET_MAC_SIZE);
        slot->rssi_available = false;
        slot->rssi_dbm = 0;
        slot->rssi_updated_ms = 0U;
    }
    net_secure_zero(&peer, sizeof(peer));
    unlock_state();
    return err == ESP_OK ? WEREWOLF_NET_OK : WEREWOLF_NET_ERR_TRANSPORT;
}

werewolf_net_result_t werewolf_net_remove_peer(uint8_t player_id)
{
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    net_peer_t *peer;

    if (!s_stats.initialized) {
        return WEREWOLF_NET_ERR_STATE;
    }
    lock_state();
    peer = peer_by_id(player_id);
    if (peer == NULL) {
        unlock_state();
        return WEREWOLF_NET_ERR_NOT_FOUND;
    }
    memcpy(mac, peer->mac, sizeof(mac));
    werewolf_reliable_forget_peer(&s_reliable, player_id);
    memset(peer, 0, sizeof(*peer));
    unlock_state();
    return esp_now_del_peer(mac) == ESP_OK ? WEREWOLF_NET_OK
                                           : WEREWOLF_NET_ERR_TRANSPORT;
}

werewolf_net_result_t werewolf_net_get_peer_generation(
    uint8_t player_id,
    uint32_t *peer_generation)
{
    net_peer_t *peer;

    if (peer_generation == NULL) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    *peer_generation = 0U;
    if (!s_stats.initialized) {
        return WEREWOLF_NET_ERR_STATE;
    }
    lock_state();
    peer = peer_by_id(player_id);
    if (peer == NULL || !peer->encrypted) {
        unlock_state();
        return WEREWOLF_NET_ERR_NOT_FOUND;
    }
    *peer_generation = peer->generation;
    unlock_state();
    return WEREWOLF_NET_OK;
}

werewolf_net_result_t werewolf_net_begin_game(size_t expected_secure_peers)
{
    size_t count;
    bool client_has_host;

    if (!s_stats.initialized || expected_secure_peers == 0 ||
        expected_secure_peers > WEREWOLF_NET_MAX_PEERS) {
        return WEREWOLF_NET_ERR_STATE;
    }
    lock_state();
    count = secure_peer_count_locked();
    client_has_host = s_config.role != WEREWOLF_NET_ROLE_CLIENT ||
                      (peer_by_id(s_config.host_id) != NULL &&
                       peer_by_id(s_config.host_id)->encrypted);
    if (!s_stats.has_pmk || count < expected_secure_peers || !client_has_host) {
        unlock_state();
        return WEREWOLF_NET_ERR_SECURITY;
    }
    s_stats.game_started = true;
    unlock_state();
    return WEREWOLF_NET_OK;
}

void werewolf_net_end_game(void)
{
    if (!s_stats.initialized) {
        return;
    }
    lock_state();
    s_stats.game_started = false;
    werewolf_reliable_clear_pending(&s_reliable);
    unlock_state();
}

void werewolf_net_set_phase(uint16_t phase_seq)
{
    if (!s_stats.initialized) {
        return;
    }
    lock_state();
    werewolf_reliable_set_phase(&s_reliable, phase_seq);
    unlock_state();
}

static bool type_requires_started_game(werewolf_message_type_t type)
{
    return type == WEREWOLF_MSG_START ||
           type == WEREWOLF_MSG_PRIVATE_ROLE ||
           type == WEREWOLF_MSG_PHASE ||
           type == WEREWOLF_MSG_ACTION;
}

werewolf_net_result_t werewolf_net_broadcast_discovery(
    werewolf_message_type_t type,
    const uint8_t *payload,
    size_t payload_len)
{
    werewolf_frame_t frame = {0};
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE];
    size_t wire_len = 0;

    if (!s_stats.initialized) {
        return WEREWOLF_NET_ERR_STATE;
    }
    if (!werewolf_protocol_type_is_discovery(type) ||
        payload_len > WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE ||
        (payload == NULL && payload_len != 0)) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    lock_state();
    frame.version = WEREWOLF_PROTOCOL_VERSION;
    frame.type = type;
    frame.src = s_config.local_id;
    frame.dst = WEREWOLF_PLAYER_BROADCAST;
    frame.session_id = s_config.session_id;
    frame.epoch = s_config.epoch;
    frame.msg_seq = werewolf_reliable_next_seq(&s_reliable);
    frame.payload_len = (uint16_t)payload_len;
    if (payload_len != 0) {
        memcpy(frame.payload, payload, payload_len);
    }
    bool encoded = werewolf_protocol_encode(&frame, wire, sizeof(wire),
                                             &wire_len) ==
                   WEREWOLF_PROTOCOL_OK;
    unlock_state();
    if (!encoded) {
        return WEREWOLF_NET_ERR_PROTOCOL;
    }
    if (esp_now_send(s_broadcast_mac, wire, wire_len) != ESP_OK) {
        update_error_counter(&s_stats.tx_transport_errors);
        return WEREWOLF_NET_ERR_TRANSPORT;
    }
    return WEREWOLF_NET_OK;
}

werewolf_net_result_t werewolf_net_send_unicast(
    uint8_t peer_id,
    werewolf_message_type_t type,
    uint16_t phase_seq,
    uint32_t action_key,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t *out_msg_seq)
{
    werewolf_frame_t frame = {0};
    uint8_t wire[WEREWOLF_PROTOCOL_MAX_FRAME_SIZE] = {0};
    size_t wire_len = 0;
    net_peer_t *peer = NULL;
    werewolf_net_result_t track_result;
    esp_err_t send_result;
    werewolf_net_result_t result;
    bool state_locked = false;

    if (out_msg_seq != NULL) {
        *out_msg_seq = 0U;
    }

    if (!s_stats.initialized) {
        result = WEREWOLF_NET_ERR_STATE;
        goto cleanup;
    }
    if (!werewolf_protocol_type_requires_encryption(type) ||
        type == WEREWOLF_MSG_ACK ||
        payload_len > WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE ||
        (payload == NULL && payload_len != 0)) {
        result = WEREWOLF_NET_ERR_ARGUMENT;
        goto cleanup;
    }
    lock_state();
    state_locked = true;
    peer = peer_by_id(peer_id);
    if (peer == NULL || !peer->encrypted ||
        (type_requires_started_game(type) && !s_stats.game_started)) {
        result = peer == NULL ? WEREWOLF_NET_ERR_NOT_FOUND
                              : WEREWOLF_NET_ERR_SECURITY;
        goto cleanup;
    }
    frame.version = WEREWOLF_PROTOCOL_VERSION;
    frame.type = type;
    frame.flags = WEREWOLF_FLAG_ACK_REQUIRED;
    frame.src = s_config.local_id;
    frame.dst = peer_id;
    frame.session_id = s_config.session_id;
    frame.epoch = s_config.epoch;
    frame.msg_seq = werewolf_reliable_next_seq(&s_reliable);
    frame.phase_seq = phase_seq;
    frame.payload_len = (uint16_t)payload_len;
    frame.action_key = action_key;
    if (payload_len != 0) {
        memcpy(frame.payload, payload, payload_len);
    }
    if (werewolf_protocol_encode(&frame, wire, sizeof(wire), &wire_len) !=
        WEREWOLF_PROTOCOL_OK) {
        result = WEREWOLF_NET_ERR_PROTOCOL;
        goto cleanup;
    }
    track_result = werewolf_reliable_track(
        &s_reliable, peer_id, peer->generation, &frame, wire, wire_len,
        now_ms());
    if (track_result != WEREWOLF_NET_OK) {
        result = track_result;
        goto cleanup;
    }
    send_result = esp_now_send(peer->mac, wire, wire_len);
    if (send_result != ESP_OK) {
        ++s_stats.tx_transport_errors;
    }
    /* Tracking transfers delivery ownership to the retry worker.  A transient
     * first-send failure (notably ESP_ERR_ESPNOW_NO_MEM during a full
     * authoritative burst) must therefore still be reported to the caller as
     * accepted; the pending entry remains live and will be retried or will
     * eventually produce on_delivery_failed. */
    result = WEREWOLF_NET_OK;
    if (out_msg_seq != NULL) {
        *out_msg_seq = frame.msg_seq;
    }

cleanup:
    if (state_locked) {
        unlock_state();
    }
    net_secure_zero(&frame, sizeof(frame));
    net_secure_zero(wire, sizeof(wire));
    return result;
}

werewolf_delivery_status_t werewolf_net_delivery_status(
    uint8_t peer_id,
    uint32_t peer_generation,
    uint32_t msg_seq)
{
    werewolf_delivery_status_t status;

    if (!s_stats.initialized || s_mutex == NULL) {
        return WEREWOLF_DELIVERY_UNKNOWN;
    }
    lock_state();
    status = werewolf_reliable_delivery_status(
        &s_reliable, peer_id, peer_generation, msg_seq);
    unlock_state();
    return status;
}

size_t werewolf_net_peer_pending_count(uint8_t peer_id)
{
    size_t count;

    if (!s_stats.initialized || s_mutex == NULL ||
        peer_id == WEREWOLF_PLAYER_BROADCAST) {
        return 0U;
    }
    lock_state();
    count = werewolf_reliable_pending_count_for_peer(
        &s_reliable, peer_id);
    unlock_state();
    return count;
}

void werewolf_net_snapshot(werewolf_net_snapshot_t *snapshot)
{
    uint32_t snapshot_ms;
    uint32_t oldest_age_ms = 0U;
    int8_t weakest_rssi_dbm = INT8_MAX;
    bool all_peers_have_signal = true;

    if (snapshot == NULL) {
        return;
    }
    if (s_mutex == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }
    lock_state();
    snapshot_ms = now_ms();
    *snapshot = s_stats;
    snapshot->rx_queue_drops +=
        __atomic_load_n(&s_callback_queue_drops, __ATOMIC_RELAXED);
    snapshot->secure_peer_count = secure_peer_count_locked();
    snapshot->pending_count = werewolf_reliable_pending_count(&s_reliable);
    snapshot->signal_available = false;
    snapshot->signal_rssi_dbm = 0;
    snapshot->signal_age_ms = 0U;
    for (size_t i = 0; i < WEREWOLF_NET_MAX_PEERS; ++i) {
        uint32_t age_ms;

        if (!s_peers[i].used || !s_peers[i].encrypted) {
            continue;
        }
        if (!s_peers[i].rssi_available) {
            all_peers_have_signal = false;
            continue;
        }
        if (s_peers[i].rssi_dbm < weakest_rssi_dbm) {
            weakest_rssi_dbm = s_peers[i].rssi_dbm;
        }
        age_ms = snapshot_ms - s_peers[i].rssi_updated_ms;
        if (age_ms > oldest_age_ms) {
            oldest_age_ms = age_ms;
        }
    }
    if (snapshot->secure_peer_count != 0U && all_peers_have_signal) {
        snapshot->signal_available = true;
        snapshot->signal_rssi_dbm = weakest_rssi_dbm;
        snapshot->signal_age_ms = oldest_age_ms;
    }
    unlock_state();
}

#else

werewolf_net_result_t werewolf_net_init(const werewolf_net_config_t *config)
{
    (void)config;
    return WEREWOLF_NET_ERR_UNSUPPORTED;
}

void werewolf_net_deinit(void)
{
}

werewolf_net_result_t werewolf_net_set_pmk(const uint8_t *pmk,
                                           size_t pmk_len)
{
    uint8_t aggregate = 0U;

    if (pmk == NULL || pmk_len != WEREWOLF_NET_KEY_SIZE) {
        return WEREWOLF_NET_ERR_ARGUMENT;
    }
    for (size_t i = 0U; i < pmk_len; ++i) {
        aggregate |= pmk[i];
    }
    return aggregate != 0U ? WEREWOLF_NET_ERR_UNSUPPORTED
                           : WEREWOLF_NET_ERR_SECURITY;
}

werewolf_net_result_t werewolf_net_add_encrypted_peer(
    uint8_t player_id,
    const uint8_t mac[WEREWOLF_NET_MAC_SIZE],
    const uint8_t *lmk,
    size_t lmk_len)
{
    (void)player_id;
    (void)mac;
    (void)lmk;
    (void)lmk_len;
    return WEREWOLF_NET_ERR_UNSUPPORTED;
}

werewolf_net_result_t werewolf_net_remove_peer(uint8_t player_id)
{
    (void)player_id;
    return WEREWOLF_NET_ERR_UNSUPPORTED;
}

werewolf_net_result_t werewolf_net_get_peer_generation(
    uint8_t player_id,
    uint32_t *peer_generation)
{
    (void)player_id;
    if (peer_generation != NULL) {
        *peer_generation = 0U;
    }
    return peer_generation == NULL ? WEREWOLF_NET_ERR_ARGUMENT
                                   : WEREWOLF_NET_ERR_UNSUPPORTED;
}

werewolf_net_result_t werewolf_net_begin_game(size_t expected_secure_peers)
{
    (void)expected_secure_peers;
    return WEREWOLF_NET_ERR_UNSUPPORTED;
}

void werewolf_net_end_game(void)
{
}

void werewolf_net_set_phase(uint16_t phase_seq)
{
    (void)phase_seq;
}

werewolf_net_result_t werewolf_net_broadcast_discovery(
    werewolf_message_type_t type,
    const uint8_t *payload,
    size_t payload_len)
{
    (void)type;
    (void)payload;
    (void)payload_len;
    return WEREWOLF_NET_ERR_UNSUPPORTED;
}

werewolf_net_result_t werewolf_net_send_unicast(
    uint8_t peer_id,
    werewolf_message_type_t type,
    uint16_t phase_seq,
    uint32_t action_key,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t *out_msg_seq)
{
    (void)peer_id;
    (void)type;
    (void)phase_seq;
    (void)action_key;
    (void)payload;
    (void)payload_len;
    if (out_msg_seq != NULL) {
        *out_msg_seq = 0U;
    }
    return WEREWOLF_NET_ERR_UNSUPPORTED;
}

werewolf_delivery_status_t werewolf_net_delivery_status(
    uint8_t peer_id,
    uint32_t peer_generation,
    uint32_t msg_seq)
{
    (void)peer_id;
    (void)peer_generation;
    (void)msg_seq;
    return WEREWOLF_DELIVERY_UNKNOWN;
}

size_t werewolf_net_peer_pending_count(uint8_t peer_id)
{
    (void)peer_id;
    return 0U;
}

void werewolf_net_snapshot(werewolf_net_snapshot_t *snapshot)
{
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
}

#endif
