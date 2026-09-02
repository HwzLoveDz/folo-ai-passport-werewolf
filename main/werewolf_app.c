#include "werewolf_app.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "werewolf_game.h"
#include "werewolf_identity.h"
#include "werewolf_lobby.h"
#include "werewolf_messages.h"
#include "werewolf_net.h"
#include "werewolf_nickname.h"
#include "werewolf_pairing.h"
#include "werewolf_room_directory.h"
#include "werewolf_sound.h"
#include "werewolf_termination.h"
#include "werewolf_ui.h"

#define APP_ESPNOW_CHANNEL       6U
#define APP_QUEUE_DEPTH         24U
#define APP_DELIVERY_FALLBACK_DEPTH WEREWOLF_NET_PENDING_MAX
#define APP_TASK_STACK_BYTES 12288U
#define APP_TASK_PRIORITY        4U
#define APP_TICK_MS             50U
#define APP_BEACON_MS          500U
#define APP_JOIN_RETRY_MS       700U
#define APP_DISCOVERY_TIMEOUT_MS 10000U
#define APP_JOIN_TIMEOUT_MS      10000U
#define APP_PAIR_OFFER_TIMEOUT_MS 10000U
#define APP_HEARTBEAT_MS        1500U
#define APP_GAME_OVER_HEARTBEAT_MS 5000U
#define APP_ACTION_SETTLE_MS    2000U
#define APP_RECONNECT_MS        3500U
#define APP_HOST_LOST_MS        9000U
#define APP_SIGNAL_REFRESH_MS   1000U
#define APP_SIGNAL_STALE_MARGIN_MS 500U
#define APP_ACK_TIMEOUT_MS       180U
#define APP_MAX_RETRIES            5U
#define APP_PROFILE_RETRY_MS    1200U
#define APP_PROFILE_TIMEOUT_MS 10000U
/* The reliable layer sends immediately, then waits 180 + 360 + 720 + 1440 +
 * 2880 + 2880 ms before declaring five retries exhausted.  Keep the radio
 * alive through that complete window, plus a controller-tick margin. */
#define APP_ROOM_CLOSE_TIMEOUT_MS 9000U

_Static_assert(APP_MAX_RETRIES == 5U,
               "recalculate the room-close retry window");
_Static_assert(APP_ROOM_CLOSE_TIMEOUT_MS >
                   APP_ACK_TIMEOUT_MS * 47U + APP_TICK_MS,
               "room close must outlive reliable retry exhaustion");

static const char *TAG = "werewolf_app";

typedef enum {
    APP_EVENT_UI,
    APP_EVENT_NET,
    APP_EVENT_DELIVERY_FAILED,
    APP_EVENT_INPUT_FAILURE,
    APP_EVENT_BATTERY,
    APP_EVENT_AUDIO,
} app_event_kind_t;

typedef struct {
    app_event_kind_t kind;
    union {
        werewolf_ui_action_t ui;
        struct {
            werewolf_frame_t frame;
            uint8_t mac[WEREWOLF_NET_MAC_SIZE];
        } net;
        struct {
            uint8_t peer_id;
            uint32_t msg_seq;
            uint64_t session_id;
            uint32_t peer_generation;
        } delivery;
        struct {
            bool available;
            bool stale;
            uint8_t percent;
        } battery;
        struct {
            bool checked;
            bool available;
        } audio;
    } body;
} app_event_t;

typedef struct {
    uint32_t revision;
    werewolf_ui_public_phase_t public_phase;
    werewolf_ui_connection_t connection;
    werewolf_ui_error_t error;
} app_ui_status_snapshot_t;

typedef enum {
    APP_STATE_MODE,
    APP_STATE_HOST_LOBBY,
    APP_STATE_HOST_KICKING,
    APP_STATE_CLIENT_SCANNING,
    APP_STATE_CLIENT_JOINING,
    APP_STATE_CLIENT_LOBBY,
    APP_STATE_TERMINATING,
    APP_STATE_ROOM_CLOSED,
    APP_STATE_GAME,
    APP_STATE_ERROR,
} app_state_t;

typedef enum {
    APP_TERMINATION_TO_MODE = 0,
    APP_TERMINATION_TO_ERROR,
} app_termination_destination_t;

typedef enum {
    APP_PAIR_NONE,
    APP_PAIR_HOST_OFFER,
    APP_PAIR_HOST_LOCKED,
    APP_PAIR_CLIENT_COMMITTED,
    APP_PAIR_CLIENT_REVEALED,
} app_pair_state_t;

typedef enum {
    APP_GATE_NONE = WEREWOLF_GATE_NONE,
    APP_GATE_ROLE = WEREWOLF_GATE_ROLE,
    APP_GATE_PRIVATE_RESULT = WEREWOLF_GATE_PRIVATE_RESULT,
    APP_GATE_DAWN = WEREWOLF_GATE_DAWN,
    APP_GATE_EXILE = WEREWOLF_GATE_EXILE,
} app_gate_t;

typedef enum {
    APP_ACTION_TAG_ROLE = 1,
    APP_ACTION_TAG_NIGHT,
    APP_ACTION_TAG_SPEECH,
    APP_ACTION_TAG_VOTE,
    APP_ACTION_TAG_PRIVATE_ACK,
    APP_ACTION_TAG_DAWN_ACK,
    APP_ACTION_TAG_EXILE_ACK,
    APP_ACTION_TAG_LEAVE,
} app_action_tag_t;

typedef struct {
    bool paired;
    bool ready;
    uint8_t mac[WEREWOLF_NET_MAC_SIZE];
    uint8_t public_key[WW_PAIRING_PUBLIC_KEY_SIZE];
    uint32_t verification_code;
    uint32_t first_delivery_failure_ms;
    uint32_t last_delivery_failure_ms;
} app_peer_t;

typedef struct {
    bool active;
    bool queued;
    bool delivery_event_lost;
    uint8_t seat;
    uint32_t peer_generation;
    uint32_t msg_seq;
    uint32_t started_ms;
} app_host_kick_t;

typedef struct {
    bool active;
    bool commits_guard_target;
    werewolf_action_kind_t kind;
    uint8_t target;
    uint8_t speaker_seat;
    uint32_t phase_epoch;
    uint32_t sent_ms;
    uint32_t snapshot_revision;
} app_inflight_action_t;

typedef struct {
    app_state_t state;
    app_pair_state_t pair_state;
    app_gate_t gate;
    bool is_host;
    bool net_active;
    bool pairing_active;
    bool game_started;
    bool have_public;
    bool have_private;
    bool have_pending_role;
    bool have_pending_private_result;
    bool role_confirmed;
    bool private_result_ready;
    bool private_result_confirmed;
    bool gate_acknowledged;
    bool have_role_reveal;
    bool have_pending_role_reveal;
    bool host_review_exit_unlocked;
    bool local_ready;
    bool reconnecting;
    bool profile_confirmed;
    bool battery_available;
    bool battery_stale;
    bool audio_checked;
    bool audio_available;
    uint8_t battery_percent;
    uint8_t local_seat;
    uint8_t ready_mask;
    uint8_t role_seen_mask;
    uint8_t result_ack_mask;
    uint8_t lobby_occupied_mask;
    uint8_t visible_alive_mask;
    uint8_t seer_result_seat;
    uint8_t guard_previous_target;
    uint8_t offered_seat;
    uint32_t client_verification_code;
    uint64_t session_id;
    uint32_t protocol_epoch;
    uint32_t gate_epoch;
    uint32_t last_beacon_ms;
    uint32_t last_join_ms;
    uint32_t operation_started_ms;
    uint32_t pair_started_ms;
    uint32_t last_snapshot_ms;
    uint32_t last_host_rx_ms;
    uint32_t last_signal_refresh_ms;
    uint32_t game_snapshot_revision;
    uint32_t last_profile_ms;
    uint32_t profile_started_ms;
    uint8_t local_mac[WEREWOLF_NET_MAC_SIZE];
    uint8_t host_mac[WEREWOLF_NET_MAC_SIZE];
    uint8_t locked_client_mac[WEREWOLF_NET_MAC_SIZE];
    uint8_t room_fingerprint[WEREWOLF_MESSAGES_ROOM_FINGERPRINT_SIZE];
    uint8_t peer_public_key[WW_PAIRING_PUBLIC_KEY_SIZE];
    uint8_t host_nonce[WW_PAIRING_NONCE_SIZE];
    uint8_t client_nonce[WW_PAIRING_NONCE_SIZE];
    uint8_t host_commitment[WW_PAIRING_COMMITMENT_SIZE];
    uint8_t client_commitment[WW_PAIRING_COMMITMENT_SIZE];
    ww_camp_t seer_result_faction;
    ww_pairing_t pairing;
    ww_game_t game;
    werewolf_public_state_message_t public_state;
    werewolf_private_role_message_t private_state;
    werewolf_private_role_message_t pending_role;
    werewolf_private_role_message_t pending_private_result;
    app_inflight_action_t inflight_action;
    werewolf_nickname_t local_nickname;
    werewolf_roster_message_t roster;
    werewolf_room_directory_t room_directory;
    werewolf_termination_t termination;
    app_host_kick_t host_kick;
    werewolf_abort_reason_t termination_reason;
    app_termination_destination_t termination_destination;
    werewolf_ui_error_t termination_error;
    werewolf_ui_connection_t termination_connection;
    bool termination_recoverable;
    char termination_detail[WEREWOLF_UI_DETAIL_MAX];
    ww_role_t revealed_roles[WW_PLAYER_COUNT];
    ww_role_t pending_revealed_roles[WW_PLAYER_COUNT];
    app_peer_t peers[WW_PLAYER_COUNT];
    werewolf_ui_model_t ui;
} app_context_t;

static app_context_t s_app;
static QueueHandle_t s_queue;
static StaticQueue_t s_queue_buffer;
static uint8_t s_queue_storage[APP_QUEUE_DEPTH * sizeof(app_event_t)];
static QueueHandle_t s_delivery_fallback_queue;
static StaticQueue_t s_delivery_fallback_queue_buffer;
static uint8_t s_delivery_fallback_queue_storage[
    APP_DELIVERY_FALLBACK_DEPTH * sizeof(app_event_t)];
static TaskHandle_t s_task;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[APP_TASK_STACK_BYTES / sizeof(StackType_t)];
static bool s_delivery_event_lost;
static bool s_input_event_lost;
static werewolf_ui_deferred_release_t s_deferred_private_release;
static werewolf_ui_deferred_release_t s_deferred_ui_rollback;
static werewolf_ui_deferred_release_t s_deferred_ui_model;
static StaticSemaphore_t s_ui_snapshot_mutex_buffer;
static SemaphoreHandle_t s_ui_snapshot_mutex;
static werewolf_ui_model_t s_ui_snapshot;
static bool s_sound_snapshot_valid;
static uint32_t s_sound_snapshot_revision;
static werewolf_ui_public_phase_t s_sound_public_phase;
static werewolf_ui_connection_t s_sound_connection;
static werewolf_ui_error_t s_sound_error;

static void publish_ui(void);
static void reset_to_mode(bool notify_peers);
static void show_error(werewolf_ui_error_t error,
                       werewolf_ui_connection_t connection,
                       bool recoverable,
                       const char *detail);
static void client_apply_public(
    const werewolf_public_state_message_t *state, bool phase_signal,
    bool gate_ack_known, bool gate_acknowledged);
static bool host_publish_phase(void);
static void host_enter_game_over(void);
static bool host_clear_lobby_peer(uint8_t seat);
static void host_remove_lobby_peer(uint8_t seat);
static void host_retire_finished_peer(uint8_t seat);
static bool host_prepare_next_offer(void);
static void begin_join_scan(void);
static bool private_message_for(uint8_t seat, werewolf_message_t *message);
static bool host_send_private_to(uint8_t seat);
static bool host_send_role_reveal_to(uint8_t seat);

static uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / UINT64_C(1000));
}

static bool elapsed_ms(uint32_t now, uint32_t then, uint32_t interval)
{
    return (uint32_t)(now - then) >= interval;
}

static uint8_t seat_bit(uint8_t seat)
{
    return seat < WW_PLAYER_COUNT
               ? (uint8_t)(UINT8_C(1) << seat)
               : 0U;
}

static uint8_t count_mask(uint8_t mask)
{
    uint8_t count = 0U;

    while (mask != 0U) {
        count = (uint8_t)(count + (mask & UINT8_C(1)));
        mask = (uint8_t)(mask >> 1);
    }
    return count;
}

static void bump_lobby_revision(void)
{
    ++s_app.roster.lobby_revision;
    if (s_app.roster.lobby_revision == 0U) {
        s_app.roster.lobby_revision = 1U;
    }
}

static bool roster_complete(uint8_t occupied_mask)
{
    return s_app.roster.lobby_revision != 0U &&
           s_app.roster.profile_mask == occupied_mask;
}

static bool host_client_links_complete(uint8_t occupied_mask)
{
    if (!s_app.is_host || occupied_mask != WW_ALL_PLAYERS_MASK) {
        return false;
    }
    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if (!s_app.peers[seat].paired) {
            return false;
        }
    }
    return true;
}

static bool local_profile_echoed(void)
{
    return s_app.local_seat < WW_PLAYER_COUNT &&
           (s_app.roster.profile_mask & seat_bit(s_app.local_seat)) != 0U &&
           strcmp(s_app.roster.names[s_app.local_seat],
                  s_app.local_nickname) == 0;
}

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    while (size-- != 0U) {
        *bytes++ = 0U;
    }
}

static bool bytes_equal_constant_time(const uint8_t *left,
                                      const uint8_t *right, size_t size)
{
    volatile uint8_t difference = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (size_t i = 0U; i < size; ++i) {
        difference = (uint8_t)(difference |
                               (uint8_t)(left[i] ^ right[i]));
    }
    return difference == 0U;
}

static void random_nonzero(void *data, size_t size)
{
    uint8_t *bytes = (uint8_t *)data;
    uint8_t aggregate = 0U;

    do {
        esp_fill_random(bytes, size);
        aggregate = 0U;
        for (size_t i = 0U; i < size; ++i) {
            aggregate |= bytes[i];
        }
    } while (aggregate == 0U);
}

static uint64_t random_u64_nonzero(void)
{
    uint64_t value;

    do {
        value = ((uint64_t)esp_random() << 32) | esp_random();
    } while (value == 0U);
    return value;
}

static uint32_t random_u32_nonzero(void)
{
    uint32_t value;

    do {
        value = esp_random();
    } while (value == 0U);
    return value;
}

static void advance_gate(app_gate_t gate)
{
    s_app.gate = gate;
    ++s_app.gate_epoch;
    if (s_app.gate_epoch == 0U) {
        s_app.gate_epoch = 1U;
    }
    s_app.gate_acknowledged = false;
}

static bool verification_code_to_number(
    const char code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE], uint32_t *value)
{
    uint32_t parsed = 0U;

    if (code == NULL || value == NULL ||
        code[WW_PAIRING_VERIFY_CODE_DIGITS] != '\0') {
        return false;
    }
    for (size_t i = 0U; i < WW_PAIRING_VERIFY_CODE_DIGITS; ++i) {
        if (code[i] < '0' || code[i] > '9') {
            return false;
        }
        parsed = parsed * UINT32_C(10) + (uint32_t)(code[i] - '0');
    }
    *value = parsed;
    return true;
}

static void clear_lobby_verification_codes(void)
{
    s_app.client_verification_code = 0U;
    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        s_app.peers[seat].verification_code = 0U;
    }
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
}

static werewolf_ui_role_t ui_role(ww_role_t role)
{
    switch (role) {
    case WW_ROLE_WOLF: return WEREWOLF_UI_ROLE_WOLF;
    case WW_ROLE_SEER: return WEREWOLF_UI_ROLE_SEER;
    case WW_ROLE_GUARD: return WEREWOLF_UI_ROLE_GUARD;
    case WW_ROLE_VILLAGER: return WEREWOLF_UI_ROLE_VILLAGER;
    default: return WEREWOLF_UI_ROLE_UNKNOWN;
    }
}

static werewolf_ui_faction_t ui_faction(ww_camp_t camp)
{
    if (camp == WW_CAMP_WOLF) {
        return WEREWOLF_UI_FACTION_WOLVES;
    }
    if (camp == WW_CAMP_GOOD) {
        return WEREWOLF_UI_FACTION_GOOD;
    }
    return WEREWOLF_UI_FACTION_UNKNOWN;
}

static werewolf_ui_winner_t ui_winner(ww_winner_t winner)
{
    if (winner == WW_WINNER_GOOD) {
        return WEREWOLF_UI_WINNER_GOOD;
    }
    if (winner == WW_WINNER_WOLVES) {
        return WEREWOLF_UI_WINNER_WOLVES;
    }
    return WEREWOLF_UI_WINNER_NONE;
}

static uint8_t occupied_mask(void)
{
    if (s_app.is_host) {
        return s_app.game.joined_mask;
    }
    if (s_app.have_public) {
        return s_app.public_state.occupied_mask;
    }
    return s_app.lobby_occupied_mask;
}

static uint8_t alive_mask(void)
{
    if (!s_app.game_started) {
        return occupied_mask();
    }
    return s_app.is_host ? s_app.game.alive_mask
                         : s_app.public_state.alive_mask;
}

static ww_role_t local_role(void)
{
    if (!s_app.have_private || s_app.local_seat >= WW_PLAYER_COUNT) {
        return WW_ROLE_NONE;
    }
    return s_app.private_state.role;
}

static bool target_eligible(uint8_t target)
{
    uint8_t alive = alive_mask();
    ww_phase_t phase = s_app.is_host ? s_app.game.phase
                                     : s_app.public_state.phase;
    ww_role_t role = local_role();

    if ((alive & seat_bit(target)) == 0U) {
        return false;
    }
    if (phase == WW_PHASE_VOTE || phase == WW_PHASE_REVOTE) {
        uint8_t tied = s_app.is_host ? s_app.game.tie_mask
                                     : s_app.public_state.tie_mask;
        if (target == s_app.local_seat) {
            return false;
        }
        return phase != WW_PHASE_REVOTE || (tied & seat_bit(target)) != 0U;
    }
    if (phase != WW_PHASE_NIGHT && phase != WW_PHASE_WOLF_REVOTE) {
        return false;
    }
    if (phase == WW_PHASE_WOLF_REVOTE && role != WW_ROLE_WOLF) {
        return true;
    }
    if (role == WW_ROLE_WOLF) {
        return target != s_app.local_seat &&
               (s_app.private_state.wolf_teammate_mask & seat_bit(target)) == 0U;
    }
    if (role == WW_ROLE_SEER) {
        return target != s_app.local_seat;
    }
    if (role == WW_ROLE_GUARD) {
        return target != s_app.guard_previous_target;
    }
    return role == WW_ROLE_VILLAGER;
}

static werewolf_ui_public_phase_t ui_public_phase(void)
{
    if (s_app.ui.page == WEREWOLF_UI_PAGE_MODE) {
        return WEREWOLF_UI_PUBLIC_PHASE_MODE;
    }
    if (s_app.ui.page == WEREWOLF_UI_PAGE_LOBBY ||
        s_app.ui.page == WEREWOLF_UI_PAGE_PLAYER_ACTION) {
        return WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
    }
    if (s_app.ui.page == WEREWOLF_UI_PAGE_ROLE) {
        return WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK;
    }
    if (s_app.ui.page == WEREWOLF_UI_PAGE_ERROR) {
        return WEREWOLF_UI_PUBLIC_PHASE_ERROR;
    }
    if (s_app.gate == APP_GATE_PRIVATE_RESULT ||
        s_app.ui.page == WEREWOLF_UI_PAGE_PRIVATE_RESULT) {
        /* Night resolution may already have advanced the authoritative game
         * object to DAWN while the all-device private-result gate is still
         * open. Presenting DAWN here would disclose gate progress/timing. */
        return WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
    }
    if (!s_app.game_started) {
        return WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
    }

    ww_phase_t phase = s_app.is_host ? s_app.game.phase
                                     : s_app.public_state.phase;
    switch (phase) {
    case WW_PHASE_NIGHT:
    case WW_PHASE_WOLF_REVOTE:
        /* Wolf re-selection is intentionally still presented as NIGHT on
         * every device; the status bar must not identify a role-specific
         * sub-phase. */
        return WEREWOLF_UI_PUBLIC_PHASE_NIGHT;
    case WW_PHASE_DAWN_RESULT:
        return WEREWOLF_UI_PUBLIC_PHASE_DAWN;
    case WW_PHASE_DISCUSSION:
        return WEREWOLF_UI_PUBLIC_PHASE_DISCUSSION;
    case WW_PHASE_VOTE:
        return WEREWOLF_UI_PUBLIC_PHASE_VOTE;
    case WW_PHASE_TIE_DEFENSE:
        return WEREWOLF_UI_PUBLIC_PHASE_DEFENCE;
    case WW_PHASE_REVOTE:
        return WEREWOLF_UI_PUBLIC_PHASE_REVOTE;
    case WW_PHASE_EXILE_RESULT:
        return WEREWOLF_UI_PUBLIC_PHASE_EXILE;
    case WW_PHASE_GAME_OVER:
        return WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER;
    case WW_PHASE_LOBBY:
    default:
        return WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
    }
}

static void announce_ui_status(const app_ui_status_snapshot_t *status)
{
    werewolf_sound_cue_t cue = WEREWOLF_SOUND_COUNT;

    if (status == NULL || s_ui_snapshot_mutex == NULL ||
        xSemaphoreTake(s_ui_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (s_sound_snapshot_valid &&
        (int32_t)(status->revision - s_sound_snapshot_revision) < 0) {
        (void)xSemaphoreGive(s_ui_snapshot_mutex);
        return;
    }
    if (s_sound_snapshot_valid) {
        bool phase_changed =
            s_sound_public_phase != status->public_phase;
        bool connection_changed =
            s_sound_connection != status->connection;

        if ((phase_changed &&
             status->public_phase == WEREWOLF_UI_PUBLIC_PHASE_ERROR) ||
            (status->error != WEREWOLF_UI_ERROR_NONE &&
             s_sound_error != status->error)) {
            cue = WEREWOLF_SOUND_ERROR;
        } else if (phase_changed &&
                   status->public_phase ==
                       WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER) {
            cue = WEREWOLF_SOUND_GAME_OVER;
        } else if (connection_changed &&
                   status->connection != WEREWOLF_UI_CONNECTION_ONLINE &&
                   s_sound_connection == WEREWOLF_UI_CONNECTION_ONLINE) {
            cue = WEREWOLF_SOUND_DISCONNECTED;
        } else if (connection_changed &&
                   status->connection == WEREWOLF_UI_CONNECTION_ONLINE &&
                   s_sound_connection != WEREWOLF_UI_CONNECTION_ONLINE) {
            cue = WEREWOLF_SOUND_CONNECTED;
        } else if (phase_changed &&
                   (status->public_phase ==
                        WEREWOLF_UI_PUBLIC_PHASE_DAWN ||
                    status->public_phase ==
                        WEREWOLF_UI_PUBLIC_PHASE_EXILE)) {
            cue = WEREWOLF_SOUND_RESULT;
        } else if (phase_changed &&
                   status->public_phase != WEREWOLF_UI_PUBLIC_PHASE_MODE &&
                   status->public_phase != WEREWOLF_UI_PUBLIC_PHASE_LOBBY) {
            cue = WEREWOLF_SOUND_PHASE;
        }
    }

    s_sound_snapshot_valid = true;
    s_sound_snapshot_revision = status->revision;
    s_sound_public_phase = status->public_phase;
    s_sound_connection = status->connection;
    s_sound_error = status->error;
    (void)xSemaphoreGive(s_ui_snapshot_mutex);
    if (cue != WEREWOLF_SOUND_COUNT) {
        werewolf_sound_play(cue);
    }
}

static void play_ui_feedback(werewolf_ui_feedback_t feedback)
{
    switch (feedback) {
    case WEREWOLF_UI_FEEDBACK_MOVE:
        werewolf_sound_play(WEREWOLF_SOUND_MOVE);
        break;
    case WEREWOLF_UI_FEEDBACK_SELECT:
        werewolf_sound_play(WEREWOLF_SOUND_SELECT);
        break;
    case WEREWOLF_UI_FEEDBACK_READY_ON:
        werewolf_sound_play(WEREWOLF_SOUND_READY_ON);
        break;
    case WEREWOLF_UI_FEEDBACK_READY_OFF:
        werewolf_sound_play(WEREWOLF_SOUND_READY_OFF);
        break;
    case WEREWOLF_UI_FEEDBACK_PRIVATE_REVEAL:
        werewolf_sound_play(WEREWOLF_SOUND_PRIVATE_REVEAL);
        break;
    case WEREWOLF_UI_FEEDBACK_PRIVATE_SEAL:
        werewolf_sound_play(WEREWOLF_SOUND_PRIVATE_SEAL);
        break;
    case WEREWOLF_UI_FEEDBACK_CONFIRM_ARMED:
        werewolf_sound_play(WEREWOLF_SOUND_CONFIRM_ARMED);
        break;
    case WEREWOLF_UI_FEEDBACK_CONFIRMED:
        werewolf_sound_play(WEREWOLF_SOUND_CONFIRMED);
        break;
    case WEREWOLF_UI_FEEDBACK_NONE:
    default:
        break;
    }
}

static void fill_player_models(void)
{
    uint8_t occupied = occupied_mask();
    uint8_t alive = alive_mask();

    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        werewolf_ui_player_t *player = &s_app.ui.players[seat];
        bool present = (occupied & seat_bit(seat)) != 0U;

        player->seat = seat;
        player->occupied = present;
        player->ready = (s_app.ready_mask & seat_bit(seat)) != 0U;
        player->alive = !s_app.game_started ||
                        (alive & seat_bit(seat)) != 0U;
        player->publicly_alive = !s_app.game_started ||
                                 (s_app.visible_alive_mask &
                                  seat_bit(seat)) != 0U;
        player->eligible = present && player->alive && target_eligible(seat);
        player->role = s_app.have_role_reveal
                           ? ui_role(s_app.revealed_roles[seat])
                           : WEREWOLF_UI_ROLE_UNKNOWN;
        if (!present) {
            player->name[0] = '\0';
        } else if ((s_app.roster.profile_mask & seat_bit(seat)) != 0U) {
            (void)snprintf(player->name, sizeof(player->name), "%s",
                           s_app.roster.names[seat]);
        } else {
            (void)snprintf(player->name, sizeof(player->name), "SYNC");
        }
    }
}

static bool stage_ui_snapshot(bool request_apply)
{
    if (s_ui_snapshot_mutex == NULL ||
        xSemaphoreTake(s_ui_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    /* Publish intent before copying while the mutex is held. A button task
     * that preempts here claims the sticky flag, then blocks on this mutex
     * until the complete immutable snapshot is ready. */
    if (request_apply) {
        werewolf_ui_deferred_release_request(&s_deferred_ui_model);
    }
    memcpy(&s_ui_snapshot, &s_app.ui, sizeof(s_ui_snapshot));
    (void)xSemaphoreGive(s_ui_snapshot_mutex);
    return true;
}

/* Caller owns the LVGL lock. The snapshot mutex is never held while the
 * controller waits for LVGL, so this lock order cannot deadlock. */
static bool apply_ui_snapshot_locked(app_ui_status_snapshot_t *status)
{
    if (s_ui_snapshot_mutex == NULL ||
        xSemaphoreTake(s_ui_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    werewolf_ui_set_model(&s_ui_snapshot);
    if (status != NULL) {
        status->revision = s_ui_snapshot.revision;
        status->public_phase = s_ui_snapshot.public_phase;
        status->connection = s_ui_snapshot.connection;
        status->error = s_ui_snapshot.error;
    }
    (void)xSemaphoreGive(s_ui_snapshot_mutex);
    return true;
}

static void publish_ui_snapshot(bool advance_revision)
{
    bool model_applied = false;
    app_ui_status_snapshot_t status = { 0 };

    s_app.ui.public_phase = ui_public_phase();
    s_app.visible_alive_mask = werewolf_ui_update_visible_alive_mask(
        s_app.visible_alive_mask, occupied_mask(), alive_mask(),
        s_app.game_started, s_app.ui.public_phase);
    s_app.ui.battery_state = !s_app.battery_available
                                 ? WEREWOLF_UI_BATTERY_UNAVAILABLE
                                 : (s_app.battery_stale
                                        ? WEREWOLF_UI_BATTERY_STALE
                                        : WEREWOLF_UI_BATTERY_FRESH);
    s_app.ui.game_started = s_app.game_started;
    s_app.ui.battery_soc = s_app.battery_percent;
    (void)snprintf(s_app.ui.local_name, sizeof(s_app.ui.local_name), "%s",
                   s_app.local_nickname);
    fill_player_models();
    if (advance_revision) {
        ++s_app.ui.revision;
        if (s_app.ui.revision == 0U) {
            s_app.ui.revision = 1U;
        }
    }
    if (!stage_ui_snapshot(true)) {
        ESP_LOGE(TAG, "unable to stage UI snapshot");
        return;
    }
    if (bsp_lvgl_lock(100)) {
        if (werewolf_ui_deferred_release_claim(&s_deferred_ui_model, true)) {
            model_applied = apply_ui_snapshot_locked(&status);
            if (!model_applied) {
                werewolf_ui_deferred_release_request(&s_deferred_ui_model);
            }
        }
        bsp_lvgl_unlock();
    }
    if (model_applied) {
        announce_ui_status(&status);
    }
}

static void publish_ui(void)
{
    publish_ui_snapshot(true);
}

static void publish_telemetry_ui(void)
{
    /* Battery and quantized link health are not interaction transitions.
     * Keeping the rendered revision stable prevents a concurrent valid button
     * action from being rejected solely because telemetry changed. */
    publish_ui_snapshot(false);
}

static void set_room_code_from_fingerprint(void)
{
    (void)snprintf(s_app.ui.room_code, sizeof(s_app.ui.room_code),
                   "R%02X%02X%02X", s_app.room_fingerprint[0],
                   s_app.room_fingerprint[1], s_app.room_fingerprint[2]);
}

static void set_private_detail(void)
{
    ww_role_t role = local_role();

    s_app.ui.private_detail[0] = '\0';
    if (role == WW_ROLE_WOLF) {
        for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
            if ((s_app.private_state.wolf_teammate_mask & seat_bit(seat)) != 0U) {
                (void)snprintf(s_app.ui.private_detail,
                               sizeof(s_app.ui.private_detail),
                               "YOUR TEAMMATE IS SEAT %u", (unsigned)seat + 1U);
                break;
            }
        }
    } else if (role == WW_ROLE_SEER) {
        (void)snprintf(s_app.ui.private_detail,
                       sizeof(s_app.ui.private_detail),
                       "CHECK ONE PLAYER EACH NIGHT");
    } else if (role == WW_ROLE_GUARD) {
        (void)snprintf(s_app.ui.private_detail,
                       sizeof(s_app.ui.private_detail),
                       "DO NOT GUARD THE SAME SEAT TWICE");
    } else if (role == WW_ROLE_VILLAGER) {
        (void)snprintf(s_app.ui.private_detail,
                       sizeof(s_app.ui.private_detail),
                       "FIND THE WOLVES WITH THE TABLE");
    }
}

static void show_lobby(void)
{
    uint8_t occupied = occupied_mask();
    uint8_t selected = s_app.ui.selected_seat;
    bool selected_guest = s_app.is_host && selected > 0U &&
        selected < WW_PLAYER_COUNT && s_app.peers[selected].paired &&
        (occupied & seat_bit(selected)) != 0U;
    bool show_player = selected_guest &&
        s_app.ui.page == WEREWOLF_UI_PAGE_PLAYER_ACTION;

    if (!s_app.is_host) {
        s_app.profile_confirmed = local_profile_echoed();
    }

    if (s_app.is_host && !selected_guest) {
        s_app.ui.selected_seat = WEREWOLF_UI_NO_SEAT;
    }
    s_app.ui.page = show_player ? WEREWOLF_UI_PAGE_PLAYER_ACTION
                                : WEREWOLF_UI_PAGE_LOBBY;
    s_app.ui.mode = s_app.is_host ? WEREWOLF_UI_MODE_CREATE
                                  : WEREWOLF_UI_MODE_JOIN;
    s_app.ui.connection = WEREWOLF_UI_CONNECTION_ONLINE;
    s_app.ui.error = WEREWOLF_UI_ERROR_NONE;
    s_app.ui.is_host = s_app.is_host;
    s_app.ui.private_epoch = s_app.gate_epoch;
    s_app.ui.local_seat = s_app.local_seat;
    s_app.ui.local_ready = s_app.local_ready;
    s_app.ui.can_start = s_app.is_host &&
        host_client_links_complete(occupied) &&
        werewolf_lobby_can_start(occupied, s_app.roster.profile_mask,
                                 s_app.ready_mask);
    s_app.ui.input_enabled = s_app.is_host || s_app.profile_confirmed;
    s_app.ui.recoverable = false;
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
    if (show_player) {
        s_app.ui.has_verify_code = true;
        s_app.ui.verify_code = s_app.peers[selected].verification_code;
    } else if (!s_app.is_host && s_app.state == APP_STATE_CLIENT_LOBBY) {
        s_app.ui.has_verify_code = true;
        s_app.ui.verify_code = s_app.client_verification_code;
    }
    set_room_code_from_fingerprint();
    publish_ui();
}

static void show_scanning(void)
{
    s_app.ui.page = WEREWOLF_UI_PAGE_LOBBY;
    s_app.ui.mode = WEREWOLF_UI_MODE_JOIN;
    s_app.ui.connection = s_app.state == APP_STATE_CLIENT_SCANNING
                              ? WEREWOLF_UI_CONNECTION_SCANNING
                              : WEREWOLF_UI_CONNECTION_PAIRING;
    s_app.ui.is_host = false;
    s_app.ui.local_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.input_enabled = false;
    s_app.ui.local_ready = false;
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
    (void)snprintf(s_app.ui.room_code, sizeof(s_app.ui.room_code), "PAIR");
    publish_ui();
}

static void show_room_list(void)
{
    werewolf_room_candidate_t candidates[WEREWOLF_ROOM_DIRECTORY_CAPACITY];
    size_t count = werewolf_room_directory_snapshot_sorted(
        &s_app.room_directory, candidates,
        WEREWOLF_ROOM_DIRECTORY_CAPACITY);
    bool selection_found = false;

    memset(s_app.ui.rooms, 0, sizeof(s_app.ui.rooms));
    for (size_t i = 0U; i < count; ++i) {
        werewolf_ui_room_t *room = &s_app.ui.rooms[i];

        room->visible = true;
        room->token = candidates[i].token;
        room->occupied_count = count_mask(
            candidates[i].beacon.occupied_mask);
        (void)snprintf(room->code, sizeof(room->code), "R%02X%02X%02X",
                       candidates[i].beacon.room_fingerprint[0],
                       candidates[i].beacon.room_fingerprint[1],
                       candidates[i].beacon.room_fingerprint[2]);
        if (room->token == s_app.ui.selected_room_token) {
            selection_found = true;
        }
    }
    if (!selection_found) {
        s_app.ui.selected_room_token = count == 0U
                                           ? WEREWOLF_ROOM_TOKEN_NONE
                                           : candidates[0].token;
    }
    secure_zero(candidates, sizeof(candidates));
    s_app.ui.page = WEREWOLF_UI_PAGE_ROOM_LIST;
    s_app.ui.mode = WEREWOLF_UI_MODE_JOIN;
    s_app.ui.connection = WEREWOLF_UI_CONNECTION_SCANNING;
    s_app.ui.error = WEREWOLF_UI_ERROR_NONE;
    s_app.ui.is_host = false;
    s_app.ui.local_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.input_enabled = true;
    s_app.ui.local_ready = false;
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
    publish_ui();
}

static void show_error(werewolf_ui_error_t error,
                       werewolf_ui_connection_t connection,
                       bool recoverable,
                       const char *detail)
{
    s_app.state = APP_STATE_ERROR;
    s_app.ui.room_close_prompt = false;
    s_app.ui.room_closing = false;
    s_app.ui.page = WEREWOLF_UI_PAGE_ERROR;
    s_app.ui.error = error;
    s_app.ui.connection = connection;
    s_app.ui.recoverable = recoverable;
    s_app.ui.input_enabled = false;
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
    s_app.ui.winner = connection == WEREWOLF_UI_CONNECTION_HOST_LOST
                          ? WEREWOLF_UI_WINNER_ABORTED
                          : WEREWOLF_UI_WINNER_NONE;
    (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail), "%s",
                   detail != NULL ? detail : "SESSION STOPPED");
    publish_ui();
}

static bool app_net_message_cb(const werewolf_frame_t *frame,
                               const uint8_t source_mac[6], void *user)
{
    app_event_t event = { .kind = APP_EVENT_NET };
    bool accepted;
    (void)user;

    if (s_queue == NULL || frame == NULL || source_mac == NULL) {
        return false;
    }
    event.body.net.frame = *frame;
    memcpy(event.body.net.mac, source_mac, sizeof(event.body.net.mac));
    accepted = xQueueSend(s_queue, &event, 0U) == pdTRUE;
    secure_zero(&event, sizeof(event));
    return accepted;
}

static void app_delivery_failed_cb(uint8_t peer_id, uint32_t msg_seq,
                                   uint64_t session_id,
                                   uint32_t peer_generation,
                                   void *user)
{
    app_event_t event = { .kind = APP_EVENT_DELIVERY_FAILED };
    bool queued = false;
    (void)user;

    event.body.delivery.peer_id = peer_id;
    event.body.delivery.msg_seq = msg_seq;
    event.body.delivery.session_id = session_id;
    event.body.delivery.peer_generation = peer_generation;
    if (s_queue != NULL) {
        queued = xQueueSend(s_queue, &event, 0U) == pdTRUE;
    }
    if (!queued && s_delivery_fallback_queue != NULL) {
        queued = xQueueSend(s_delivery_fallback_queue, &event, 0U) ==
                 pdTRUE;
    }
    if (!queued) {
        /* Both queues saturated, so the exact failed transaction is unknown.
         * The controller must retain its existing fail-closed policy. */
        __atomic_store_n(&s_delivery_event_lost, true, __ATOMIC_RELEASE);
    }
    secure_zero(&event, sizeof(event));
}

static bool start_network(werewolf_net_role_t role, uint8_t local_id,
                          uint64_t session_id, uint32_t epoch,
                          const uint8_t *room_pmk)
{
    werewolf_net_config_t config = {
        .role = role,
        .local_id = local_id,
        .host_id = 0U,
        .channel = APP_ESPNOW_CHANNEL,
        .session_id = session_id,
        .epoch = epoch,
        .ack_timeout_ms = APP_ACK_TIMEOUT_MS,
        .max_retries = APP_MAX_RETRIES,
        .pmk = room_pmk,
        .pmk_len = room_pmk != NULL ? WEREWOLF_NET_KEY_SIZE : 0U,
        .on_message = app_net_message_cb,
        .on_delivery_failed = app_delivery_failed_cb,
        .user = NULL,
    };
    werewolf_net_result_t result;

    if (s_app.net_active) {
        werewolf_net_deinit();
        s_app.net_active = false;
    }
    __atomic_store_n(&s_delivery_event_lost, false, __ATOMIC_RELEASE);
    result = werewolf_net_init(&config);
    if (result != WEREWOLF_NET_OK) {
        return false;
    }
    s_app.net_active = true;
    return true;
}

static bool encode_and_broadcast(const werewolf_message_t *message)
{
    uint8_t payload[WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE];
    size_t payload_len = 0U;
    bool ok = werewolf_messages_encode(message, payload, sizeof(payload),
                                        &payload_len) == WEREWOLF_MESSAGES_OK &&
              werewolf_net_broadcast_discovery(message->type, payload,
                                                payload_len) == WEREWOLF_NET_OK;

    secure_zero(payload, sizeof(payload));
    return ok;
}

static bool encode_and_send_tracked(uint8_t peer,
                                    const werewolf_message_t *message,
                                    uint32_t phase_epoch,
                                    uint32_t action_key,
                                    uint32_t *out_msg_seq)
{
    uint8_t payload[WEREWOLF_PROTOCOL_MAX_PAYLOAD_SIZE];
    size_t payload_len = 0U;
    bool ok;

    if (out_msg_seq != NULL) {
        *out_msg_seq = 0U;
    }
    ok = werewolf_messages_encode(message, payload, sizeof(payload),
                                  &payload_len) == WEREWOLF_MESSAGES_OK &&
         werewolf_net_send_unicast(peer, message->type,
                                   (uint16_t)phase_epoch, action_key,
                                   payload, payload_len,
                                   out_msg_seq) == WEREWOLF_NET_OK;

    secure_zero(payload, sizeof(payload));
    return ok;
}

static bool encode_and_send(uint8_t peer, const werewolf_message_t *message,
                            uint32_t phase_epoch, uint32_t action_key)
{
    return encode_and_send_tracked(peer, message, phase_epoch, action_key,
                                   NULL);
}

static uint32_t action_key(uint8_t seat, uint32_t phase_epoch,
                           werewolf_gate_kind_t gate_kind,
                           uint32_t gate_epoch,
                           werewolf_action_kind_t kind,
                           app_action_tag_t tag)
{
    uint32_t key = UINT32_C(0x9e3779b9) ^
                   (phase_epoch * UINT32_C(0x85ebca6b)) ^
                   (gate_epoch * UINT32_C(0xc2b2ae35)) ^
                   ((uint32_t)seat << 24) ^ ((uint32_t)kind << 16) ^
                   ((uint32_t)gate_kind << 8) ^ (uint32_t)tag;

    key ^= key >> 16;
    return key == 0U ? 1U : key;
}

static bool send_action(uint8_t peer, uint8_t actor,
                        werewolf_action_kind_t kind, uint8_t target,
                        uint32_t phase_epoch,
                        werewolf_gate_kind_t gate_kind,
                        uint32_t gate_epoch, app_action_tag_t tag)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_ACTION,
        .body.action = {
            .kind = kind,
            .target = target,
            .expected_phase_epoch = phase_epoch,
            .expected_gate_kind = gate_kind,
            .expected_gate_epoch = gate_epoch,
        },
    };

    return encode_and_send(peer, &message, phase_epoch,
                           action_key(actor, phase_epoch, gate_kind,
                                      gate_epoch, kind, tag));
}

static bool game_public_state(werewolf_public_state_message_t *state)
{
    ww_public_view_t view;

    if (state == NULL || ww_game_get_public_view(&s_app.game, &view) != WW_OK ||
        view.phase == WW_PHASE_LOBBY) {
        return false;
    }
    *state = (werewolf_public_state_message_t){
        .phase = view.phase,
        .winner = view.winner,
        .phase_epoch = view.phase_epoch,
        .round_number = view.round_number,
        .occupied_mask = view.joined_mask,
        .alive_mask = view.alive_mask,
        .submitted_mask = view.submitted_mask,
        .tie_mask = view.tie_mask,
        .current_speaker = view.current_speaker,
        .dawn_victim = view.dawn_victim,
        .exiled_player = view.exiled_player,
        .gate_kind = (werewolf_gate_kind_t)s_app.gate,
        .gate_epoch = s_app.gate_epoch,
    };
    return true;
}

static bool send_to_clients(const werewolf_message_t *message,
                            uint32_t phase_epoch)
{
    bool ok = true;

    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if (s_app.peers[seat].paired) {
            ok = encode_and_send(seat, message, phase_epoch, 0U) && ok;
        }
    }
    return ok;
}

static void send_beacon(void)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_BEACON,
        .body.beacon = {
            .protocol_version = WEREWOLF_PROTOCOL_VERSION,
            .rules_version = WEREWOLF_MESSAGES_RULES_VERSION,
            .occupied_mask = s_app.game.joined_mask,
            .offered_seat = s_app.offered_seat,
        },
    };

    if (!s_app.is_host || s_app.pair_state != APP_PAIR_HOST_OFFER ||
        !s_app.pairing_active || s_app.offered_seat == WW_NO_PLAYER) {
        return;
    }
    memcpy(message.body.beacon.room_fingerprint, s_app.room_fingerprint,
           sizeof(message.body.beacon.room_fingerprint));
    memcpy(message.body.beacon.host_commitment, s_app.host_commitment,
           sizeof(message.body.beacon.host_commitment));
    (void)encode_and_broadcast(&message);
    secure_zero(&message, sizeof(message));
}

static void send_join(void)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_JOIN,
        .body.join.candidate_seat = s_app.local_seat,
    };

    if (s_app.is_host || s_app.pair_state != APP_PAIR_CLIENT_COMMITTED) {
        return;
    }
    memcpy(message.body.join.client_commitment, s_app.client_commitment,
           sizeof(message.body.join.client_commitment));
    (void)encode_and_broadcast(&message);
    secure_zero(&message, sizeof(message));
}

static void send_pair_host_reveal(void)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_PAIR_HOST_REVEAL,
        .body.pair_host_reveal.offered_seat = s_app.offered_seat,
    };

    if (!s_app.is_host || s_app.pair_state != APP_PAIR_HOST_LOCKED ||
        !s_app.pairing_active ||
        ww_pairing_get_public_key(
            &s_app.pairing,
            message.body.pair_host_reveal.host_public_key) != WW_PAIRING_OK) {
        return;
    }
    memcpy(message.body.pair_host_reveal.host_nonce, s_app.host_nonce,
           sizeof(message.body.pair_host_reveal.host_nonce));
    memcpy(message.body.pair_host_reveal.locked_client_commitment,
           s_app.client_commitment,
           sizeof(message.body.pair_host_reveal.locked_client_commitment));
    (void)encode_and_broadcast(&message);
    secure_zero(&message, sizeof(message));
}

static void send_pair_client_reveal(void)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_PAIR_CLIENT_REVEAL,
        .body.pair_client_reveal.candidate_seat = s_app.local_seat,
    };

    if (s_app.is_host || s_app.pair_state != APP_PAIR_CLIENT_REVEALED ||
        !s_app.pairing_active ||
        ww_pairing_get_public_key(
            &s_app.pairing,
            message.body.pair_client_reveal.client_public_key) !=
            WW_PAIRING_OK) {
        return;
    }
    memcpy(message.body.pair_client_reveal.client_nonce, s_app.client_nonce,
           sizeof(message.body.pair_client_reveal.client_nonce));
    memcpy(message.body.pair_client_reveal.echoed_host_commitment,
           s_app.host_commitment,
           sizeof(message.body.pair_client_reveal.echoed_host_commitment));
    (void)encode_and_broadcast(&message);
    secure_zero(&message, sizeof(message));
}

static bool send_lobby_snapshot_to(uint8_t seat)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_SNAPSHOT,
        .body.snapshot = {
            .kind = WEREWOLF_SNAPSHOT_LOBBY,
            .body.lobby = {
                .phase_epoch = s_app.game.phase_epoch,
                .occupied_mask = s_app.game.joined_mask,
                .ready_mask = s_app.ready_mask,
                .roster = s_app.roster,
            },
        },
    };

    return encode_and_send(seat, &message, s_app.game.phase_epoch, 0U);
}

static void send_game_snapshot_to(uint8_t seat)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_SNAPSHOT,
        .body.snapshot.kind = WEREWOLF_SNAPSHOT_GAME,
    };

    if (game_public_state(
            &message.body.snapshot.body.game.public_state)) {
        /* Submission timing is private.  A reconnecting client only needs to
         * know whether its own action was accepted. */
        message.body.snapshot.body.game.public_state.submitted_mask &=
            seat_bit(seat);
        message.body.snapshot.body.game.local_gate_acknowledged =
            s_app.gate != APP_GATE_NONE &&
            (s_app.result_ack_mask & seat_bit(seat)) != 0U;
        (void)encode_and_send(seat, &message,
                              message.body.snapshot.body.game.public_state
                                  .phase_epoch,
                              0U);
        secure_zero(&message, sizeof(message));
    }
}

static void send_heartbeat_snapshots_excluding(uint8_t excluded_mask)
{
    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if (!s_app.peers[seat].paired ||
            (excluded_mask & seat_bit(seat)) != 0U ||
            werewolf_net_peer_pending_count(seat) != 0U) {
            continue;
        }
        if (s_app.game_started) {
            send_game_snapshot_to(seat);
            if (s_app.game.phase == WW_PHASE_GAME_OVER &&
                s_app.gate == APP_GATE_NONE && s_app.have_role_reveal) {
                /* ROLE_REVEAL is secret until the final gate closes.  It is
                 * repeated only as encrypted unicast so retry exhaustion of
                 * the first burst cannot strand this client forever. */
                (void)host_send_role_reveal_to(seat);
            } else if ((s_app.result_ack_mask & seat_bit(seat)) == 0U) {
                if (s_app.gate == APP_GATE_ROLE) {
                    (void)host_send_private_to(seat);
                } else if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
                    (void)host_send_private_to(seat);
                }
            }
        } else {
            (void)send_lobby_snapshot_to(seat);
        }
    }
}

static void send_heartbeat_snapshots(void)
{
    send_heartbeat_snapshots_excluding(0U);
}

static bool send_ready(bool ready)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_READY,
        .body.ready.ready = ready,
    };

    return encode_and_send(0U, &message, 0U, 0U);
}

static bool send_profile(void)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_PROFILE,
    };

    memcpy(message.body.profile.nickname, s_app.local_nickname,
           sizeof(message.body.profile.nickname));
    return encode_and_send(0U, &message, 0U, 0U);
}

static bool send_abort_to(uint8_t peer, werewolf_abort_reason_t reason)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_ABORT,
        .body.abort.reason = reason,
    };
    uint32_t phase = s_app.game_started
                         ? (s_app.is_host ? s_app.game.phase_epoch
                                          : s_app.public_state.phase_epoch)
                         : 0U;

    return encode_and_send(peer, &message, phase, 0U);
}

static bool send_abort_to_tracked(uint8_t peer,
                                  werewolf_abort_reason_t reason,
                                  uint32_t *out_msg_seq)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_ABORT,
        .body.abort.reason = reason,
    };

    return encode_and_send_tracked(peer, &message, 0U, 0U,
                                   out_msg_seq);
}

static uint8_t paired_client_mask(void)
{
    uint8_t mask = 0U;

    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if (s_app.peers[seat].paired) {
            mask |= seat_bit(seat);
        }
    }
    return mask;
}

static uint8_t queue_termination_for_mask(uint8_t target_mask,
                                          werewolf_abort_reason_t reason)
{
    uint8_t queued_mask = 0U;

    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        uint8_t bit = seat_bit(seat);
        bool valid_peer = s_app.is_host ?
                              (seat > 0U && s_app.peers[seat].paired) :
                              seat == 0U;

        if ((target_mask & bit) != 0U && valid_peer &&
            send_abort_to(seat, reason)) {
            queued_mask |= bit;
        }
    }
    return queued_mask;
}

static void destroy_pairing(void)
{
    if (s_app.pairing_active) {
        (void)ww_pairing_deinit(&s_app.pairing);
        s_app.pairing_active = false;
    }
    secure_zero(s_app.peer_public_key, sizeof(s_app.peer_public_key));
    secure_zero(s_app.host_nonce, sizeof(s_app.host_nonce));
    secure_zero(s_app.client_nonce, sizeof(s_app.client_nonce));
    secure_zero(s_app.host_commitment, sizeof(s_app.host_commitment));
    secure_zero(s_app.client_commitment, sizeof(s_app.client_commitment));
    secure_zero(s_app.locked_client_mac, sizeof(s_app.locked_client_mac));
    s_app.offered_seat = WW_NO_PLAYER;
    s_app.pair_started_ms = 0U;
    s_app.pair_state = APP_PAIR_NONE;
}

static void clear_session(void)
{
    if (s_app.net_active) {
        werewolf_net_end_game();
        werewolf_net_deinit();
        s_app.net_active = false;
    }
    destroy_pairing();
    secure_zero(s_app.host_mac, sizeof(s_app.host_mac));
    secure_zero(s_app.room_fingerprint, sizeof(s_app.room_fingerprint));
    secure_zero(s_app.peers, sizeof(s_app.peers));
    secure_zero(s_app.revealed_roles, sizeof(s_app.revealed_roles));
    secure_zero(s_app.pending_revealed_roles,
                sizeof(s_app.pending_revealed_roles));
    secure_zero(&s_app.private_state, sizeof(s_app.private_state));
    secure_zero(&s_app.pending_role, sizeof(s_app.pending_role));
    secure_zero(&s_app.pending_private_result,
                sizeof(s_app.pending_private_result));
    secure_zero(&s_app.inflight_action, sizeof(s_app.inflight_action));
    secure_zero(&s_app.roster, sizeof(s_app.roster));
    werewolf_room_directory_clear(&s_app.room_directory);
    secure_zero(&s_app.game, sizeof(s_app.game));
    secure_zero(&s_app.public_state, sizeof(s_app.public_state));
    s_app.session_id = 0U;
    s_app.protocol_epoch = 0U;
    s_app.client_verification_code = 0U;
    s_app.gate_epoch = 0U;
    s_app.game_snapshot_revision = 0U;
    s_app.game_started = false;
    s_app.have_public = false;
    s_app.have_private = false;
    s_app.have_pending_role = false;
    s_app.have_pending_private_result = false;
    s_app.have_role_reveal = false;
    s_app.have_pending_role_reveal = false;
    s_app.host_review_exit_unlocked = false;
    s_app.role_confirmed = false;
    s_app.private_result_ready = false;
    s_app.private_result_confirmed = false;
    s_app.gate_acknowledged = false;
    s_app.local_ready = false;
    s_app.ready_mask = 0U;
    s_app.role_seen_mask = 0U;
    s_app.result_ack_mask = 0U;
    s_app.lobby_occupied_mask = 0U;
    secure_zero(&s_app.termination, sizeof(s_app.termination));
    secure_zero(&s_app.host_kick, sizeof(s_app.host_kick));
    s_app.host_kick.seat = WEREWOLF_UI_NO_SEAT;
    secure_zero(s_app.termination_detail, sizeof(s_app.termination_detail));
    s_app.termination_reason = WEREWOLF_ABORT_INTERNAL_ERROR;
    s_app.termination_destination = APP_TERMINATION_TO_MODE;
    s_app.termination_error = WEREWOLF_UI_ERROR_NONE;
    s_app.termination_connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
    s_app.termination_recoverable = false;
    s_app.visible_alive_mask = 0U;
    s_app.seer_result_seat = WW_NO_PLAYER;
    s_app.seer_result_faction = WW_CAMP_UNKNOWN;
    s_app.gate = APP_GATE_NONE;
    s_app.reconnecting = false;
    s_app.profile_confirmed = false;
    s_app.last_profile_ms = 0U;
    s_app.profile_started_ms = 0U;
    s_app.ui.room_close_prompt = false;
    s_app.ui.room_closing = false;
    s_app.ui.leaving_room = false;
    s_app.ui.player_kicking = false;
    s_app.ui.kick_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
    s_app.ui.waiting_for_players = false;
    s_app.ui.selected_room_token = WEREWOLF_ROOM_TOKEN_NONE;
    memset(s_app.ui.rooms, 0, sizeof(s_app.ui.rooms));
    s_app.ui.signal = WEREWOLF_UI_SIGNAL_NO_SAMPLE;
    s_app.last_signal_refresh_ms = app_now_ms();
}

static bool completed_review_active(void)
{
    if (!s_app.game_started || !s_app.have_role_reveal) {
        return false;
    }
    if (s_app.is_host) {
        return s_app.game.phase == WW_PHASE_GAME_OVER &&
               s_app.gate == APP_GATE_NONE;
    }
    return s_app.have_public &&
           werewolf_messages_public_allows_role_reveal(
               &s_app.public_state);
}

static uint32_t signal_stale_interval_ms(void)
{
    uint32_t heartbeat = completed_review_active()
                             ? APP_GAME_OVER_HEARTBEAT_MS
                             : APP_HEARTBEAT_MS;

    return heartbeat * 2U + APP_SIGNAL_STALE_MARGIN_MS;
}

static werewolf_ui_signal_t current_signal_state(void)
{
    werewolf_net_snapshot_t net_state = { 0 };

    switch (s_app.ui.connection) {
    case WEREWOLF_UI_CONNECTION_RECONNECTING:
        return WEREWOLF_UI_SIGNAL_STALE;
    case WEREWOLF_UI_CONNECTION_DISCONNECTED:
    case WEREWOLF_UI_CONNECTION_HOST_LOST:
        return WEREWOLF_UI_SIGNAL_DISCONNECTED;
    case WEREWOLF_UI_CONNECTION_RADIO_OFF:
    case WEREWOLF_UI_CONNECTION_SCANNING:
    case WEREWOLF_UI_CONNECTION_PAIRING:
        return WEREWOLF_UI_SIGNAL_NO_SAMPLE;
    case WEREWOLF_UI_CONNECTION_ONLINE:
    default:
        break;
    }
    if (!s_app.net_active) {
        return WEREWOLF_UI_SIGNAL_NO_SAMPLE;
    }
    werewolf_net_snapshot(&net_state);
    if (net_state.secure_peer_count == 0U ||
        !net_state.signal_available) {
        return WEREWOLF_UI_SIGNAL_NO_SAMPLE;
    }
    if (net_state.signal_age_ms >= signal_stale_interval_ms()) {
        return WEREWOLF_UI_SIGNAL_STALE;
    }
    return werewolf_ui_signal_from_rssi(net_state.signal_rssi_dbm);
}

static void refresh_signal_ui(uint32_t now)
{
    werewolf_ui_signal_t signal;

    /* Sampling on a fixed cadence prevents the header from exposing the exact
     * arrival time of a private night/action packet. */
    if (!elapsed_ms(now, s_app.last_signal_refresh_ms,
                    APP_SIGNAL_REFRESH_MS)) {
        return;
    }
    s_app.last_signal_refresh_ms = now;
    signal = current_signal_state();
    if (signal != s_app.ui.signal) {
        s_app.ui.signal = signal;
        publish_telemetry_ui();
    }
}

static void reset_to_mode(bool notify_peers)
{
    if (notify_peers && s_app.net_active) {
        if (completed_review_active()) {
            if (!s_app.is_host) {
                /* Final review leave is an optional best-effort retirement
                 * hint, never ABORT: other screens keep their static deck. */
                (void)send_action(
                    0U, s_app.local_seat, WEREWOLF_ACTION_LEAVE_GAME,
                    WW_NO_PLAYER, s_app.public_state.phase_epoch,
                    s_app.public_state.gate_kind,
                    s_app.public_state.gate_epoch, APP_ACTION_TAG_LEAVE);
            }
        }
    }
    clear_session();
    s_app.state = APP_STATE_MODE;
    s_app.is_host = false;
    s_app.local_seat = WEREWOLF_UI_NO_SEAT;
    werewolf_ui_model_init(&s_app.ui);
    s_app.ui.page = WEREWOLF_UI_PAGE_MODE;
    s_app.ui.input_enabled = true;
    publish_ui();
}

static void show_room_closed_notice(void)
{
    clear_session();
    s_app.state = APP_STATE_ROOM_CLOSED;
    s_app.is_host = false;
    s_app.local_seat = WEREWOLF_UI_NO_SEAT;
    werewolf_ui_model_init(&s_app.ui);
    s_app.ui.page = WEREWOLF_UI_PAGE_ROOM_CLOSED;
    s_app.ui.public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
    s_app.ui.connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
    s_app.ui.input_enabled = true;
    (void)snprintf(s_app.ui.headline, sizeof(s_app.ui.headline),
                   "ROOM CLOSED");
    (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                   "THE HOST CLOSED THIS ROOM.");
    publish_ui();
}

static void show_kicked_notice(void)
{
    clear_session();
    s_app.state = APP_STATE_ROOM_CLOSED;
    s_app.is_host = false;
    s_app.local_seat = WEREWOLF_UI_NO_SEAT;
    werewolf_ui_model_init(&s_app.ui);
    s_app.ui.page = WEREWOLF_UI_PAGE_ROOM_CLOSED;
    s_app.ui.public_phase = WEREWOLF_UI_PUBLIC_PHASE_LOBBY;
    s_app.ui.connection = WEREWOLF_UI_CONNECTION_DISCONNECTED;
    s_app.ui.input_enabled = true;
    (void)snprintf(s_app.ui.headline, sizeof(s_app.ui.headline),
                   "REMOVED FROM ROOM");
    (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                   "THE HOST REMOVED YOU FROM THIS ROOM.");
    publish_ui();
}

static void finish_termination(void)
{
    app_termination_destination_t destination =
        s_app.termination_destination;
    werewolf_ui_error_t error = s_app.termination_error;
    werewolf_ui_connection_t connection = s_app.termination_connection;
    bool recoverable = s_app.termination_recoverable;
    char detail[WEREWOLF_UI_DETAIL_MAX];

    (void)snprintf(detail, sizeof(detail), "%s", s_app.termination_detail);
    if (destination == APP_TERMINATION_TO_MODE) {
        reset_to_mode(false);
    } else {
        clear_session();
        s_app.is_host = false;
        s_app.local_seat = WEREWOLF_UI_NO_SEAT;
        s_app.ui.is_host = false;
        show_error(error, connection, recoverable, detail);
    }
    secure_zero(detail, sizeof(detail));
}

static void begin_termination(uint8_t target_mask,
                              werewolf_abort_reason_t reason,
                              app_termination_destination_t destination,
                              werewolf_ui_error_t error,
                              werewolf_ui_connection_t connection,
                              bool recoverable,
                              const char *detail,
                              bool closing_room,
                              bool leaving_room)
{
    werewolf_net_snapshot_t net_state = { 0 };
    uint32_t now;
    uint8_t queued;

    if (s_app.state == APP_STATE_TERMINATING) {
        return;
    }
    destroy_pairing();
    werewolf_net_end_game();
    werewolf_net_snapshot(&net_state);
    now = app_now_ms();
    s_app.state = APP_STATE_TERMINATING;
    s_app.termination_reason = reason;
    s_app.termination_destination = destination;
    s_app.termination_error = error;
    s_app.termination_connection = connection;
    s_app.termination_recoverable = recoverable;
    (void)snprintf(s_app.termination_detail,
                   sizeof(s_app.termination_detail), "%s",
                   detail != NULL ? detail : "SESSION STOPPED");
    werewolf_termination_begin(&s_app.termination, target_mask, now,
                               net_state.tx_exhausted);
    queued = queue_termination_for_mask(target_mask, reason);
    werewolf_termination_mark_queued(&s_app.termination, queued);
    s_app.ui.room_close_prompt = false;
    s_app.ui.room_closing = closing_room;
    s_app.ui.leaving_room = leaving_room;
    s_app.ui.input_enabled = false;
    ESP_LOGI(TAG,
             "terminating session; target=0x%02x queued=0x%02x reason=%u",
             s_app.termination.target_mask,
             s_app.termination.queued_mask, (unsigned)reason);
    publish_ui();
}

static void begin_host_fatal_excluding(uint8_t excluded_mask,
                                       werewolf_abort_reason_t reason,
                                       werewolf_ui_error_t error,
                                       werewolf_ui_connection_t connection,
                                       bool recoverable,
                                       const char *detail)
{
    uint8_t target = (uint8_t)(paired_client_mask() &
                               (uint8_t)~excluded_mask);

    begin_termination(target, reason, APP_TERMINATION_TO_ERROR, error,
                      connection, recoverable, detail, true, false);
}

static void begin_host_fatal(werewolf_abort_reason_t reason,
                             werewolf_ui_error_t error,
                             werewolf_ui_connection_t connection,
                             bool recoverable,
                             const char *detail)
{
    begin_host_fatal_excluding(0U, reason, error, connection, recoverable,
                               detail);
}

static void begin_client_leave(void)
{
    begin_termination(seat_bit(0U), WEREWOLF_ABORT_USER_CANCELLED,
                      APP_TERMINATION_TO_MODE, WEREWOLF_UI_ERROR_NONE,
                      WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                      "LEFT ROOM", false, true);
}

static void begin_host_room_close(void)
{
    if (s_app.state != APP_STATE_HOST_LOBBY || !s_app.is_host ||
        !s_app.ui.room_close_prompt || !s_app.net_active) {
        werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        return;
    }

    begin_termination(paired_client_mask(),
                      WEREWOLF_ABORT_HOST_CLOSED_ROOM,
                      APP_TERMINATION_TO_MODE, WEREWOLF_UI_ERROR_NONE,
                      WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                      "ROOM CLOSED", true, false);
}

static bool host_lobby_state_active(void)
{
    return s_app.state == APP_STATE_HOST_LOBBY ||
           s_app.state == APP_STATE_HOST_KICKING;
}

static void finish_host_kick(void)
{
    uint8_t seat;
    bool delivery_event_lost;

    if (!s_app.host_kick.active) {
        return;
    }
    seat = s_app.host_kick.seat;
    delivery_event_lost = s_app.host_kick.delivery_event_lost;
    secure_zero(&s_app.host_kick, sizeof(s_app.host_kick));
    s_app.host_kick.seat = WEREWOLF_UI_NO_SEAT;
    s_app.state = APP_STATE_HOST_LOBBY;
    s_app.ui.page = WEREWOLF_UI_PAGE_LOBBY;
    s_app.ui.player_kicking = false;
    s_app.ui.kick_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.selected_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.input_enabled = true;
    (void)host_clear_lobby_peer(seat);
    if (!host_prepare_next_offer()) {
        begin_host_fatal(WEREWOLF_ABORT_SECURITY_FAILURE,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "PAIR OFFER ROTATION FAILED");
        return;
    }
    show_lobby();
    send_heartbeat_snapshots();
    s_app.last_snapshot_ms = app_now_ms();
    send_beacon();
    s_app.last_beacon_ms = app_now_ms();
    if (delivery_event_lost) {
        /* Preserve the pre-existing fail-closed lobby policy when a different
         * delivery failure was hidden by queue overflow. The exact kick
         * result itself is read from the reliable completion cache. */
        __atomic_store_n(&s_delivery_event_lost, true, __ATOMIC_RELEASE);
    }
}

static void begin_host_kick(uint8_t seat)
{
    uint32_t peer_generation = 0U;

    if (s_app.state != APP_STATE_HOST_LOBBY || !s_app.is_host ||
        !s_app.net_active ||
        s_app.ui.page != WEREWOLF_UI_PAGE_PLAYER_ACTION ||
        s_app.ui.player_kicking ||
        seat == 0U || seat >= WW_PLAYER_COUNT ||
        seat != s_app.ui.selected_seat || !s_app.peers[seat].paired ||
        (s_app.game.joined_mask & seat_bit(seat)) == 0U ||
        werewolf_net_get_peer_generation(seat, &peer_generation) !=
            WEREWOLF_NET_OK) {
        werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        return;
    }

    secure_zero(&s_app.host_kick, sizeof(s_app.host_kick));
    s_app.host_kick.active = true;
    s_app.host_kick.seat = seat;
    s_app.host_kick.peer_generation = peer_generation;
    s_app.host_kick.started_ms = app_now_ms();
    s_app.state = APP_STATE_HOST_KICKING;
    s_app.ui.player_kicking = true;
    s_app.ui.kick_seat = seat;
    s_app.ui.input_enabled = false;
    publish_ui();
}

static void poll_host_kick(uint32_t now)
{
    werewolf_delivery_status_t status = WEREWOLF_DELIVERY_UNKNOWN;

    if (!s_app.host_kick.active) {
        return;
    }
    if (!s_app.host_kick.queued) {
        uint32_t msg_seq = 0U;

        if (send_abort_to_tracked(
                s_app.host_kick.seat,
                WEREWOLF_ABORT_KICKED_BY_HOST, &msg_seq)) {
            s_app.host_kick.queued = true;
            s_app.host_kick.msg_seq = msg_seq;
        }
    }
    if (s_app.host_kick.queued) {
        status = werewolf_net_delivery_status(
            s_app.host_kick.seat,
            s_app.host_kick.peer_generation,
            s_app.host_kick.msg_seq);
        if (status == WEREWOLF_DELIVERY_ACKNOWLEDGED ||
            status == WEREWOLF_DELIVERY_FAILED) {
            ESP_LOGI(TAG,
                     "host kick settled: seat=%u seq=%" PRIu32
                     " status=%u",
                     s_app.host_kick.seat, s_app.host_kick.msg_seq,
                     (unsigned)status);
            finish_host_kick();
            return;
        }
    }
    if (elapsed_ms(now, s_app.last_snapshot_ms, APP_HEARTBEAT_MS)) {
        /* Keep every non-target guest online while the target consumes the
         * full reliable retry window. Per-peer pending checks prevent these
         * liveness snapshots from stacking behind an unacknowledged frame. */
        send_heartbeat_snapshots_excluding(
            seat_bit(s_app.host_kick.seat));
        s_app.last_snapshot_ms = now;
    }
    if (elapsed_ms(now, s_app.host_kick.started_ms,
                   APP_ROOM_CLOSE_TIMEOUT_MS)) {
        ESP_LOGW(TAG,
                 "host kick deadline: seat=%u queued=%s seq=%" PRIu32
                 " status=%u",
                 s_app.host_kick.seat,
                 s_app.host_kick.queued ? "yes" : "no",
                 s_app.host_kick.msg_seq, (unsigned)status);
        finish_host_kick();
    }
}

static bool start_pairing_context(void)
{
    destroy_pairing();
    s_app.pairing = (ww_pairing_t)WW_PAIRING_CONTEXT_INIT;
    s_app.pairing_active = ww_pairing_init(&s_app.pairing) == WW_PAIRING_OK;
    return s_app.pairing_active;
}

static void create_room(void)
{
    uint8_t room_pmk[WEREWOLF_NET_KEY_SIZE] = { 0 };

    clear_session();
    s_app.is_host = true;
    s_app.local_seat = 0U;
    s_app.session_id = random_u64_nonzero();
    s_app.protocol_epoch = random_u32_nonzero();
    random_nonzero(s_app.room_fingerprint, sizeof(s_app.room_fingerprint));
    bool room_pmk_ok = ww_pairing_derive_room_pmk(
                           s_app.session_id, s_app.protocol_epoch,
                           s_app.room_fingerprint,
                           sizeof(s_app.room_fingerprint), room_pmk) ==
                       WW_PAIRING_OK;
    if (!room_pmk_ok ||
        esp_read_mac(s_app.local_mac, ESP_MAC_WIFI_STA) != ESP_OK ||
        !start_network(WEREWOLF_NET_ROLE_HOST, 0U, s_app.session_id,
                       s_app.protocol_epoch, room_pmk) ||
        ww_game_init(&s_app.game) != WW_OK ||
        ww_game_join(&s_app.game, 0U) != WW_OK ||
        !host_prepare_next_offer()) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "ROOM SETUP FAILED");
        secure_zero(room_pmk, sizeof(room_pmk));
        return;
    }
    secure_zero(room_pmk, sizeof(room_pmk));
    s_app.state = APP_STATE_HOST_LOBBY;
    s_app.lobby_occupied_mask = seat_bit(0U);
    s_app.roster.lobby_revision = 1U;
    s_app.roster.profile_mask = seat_bit(0U);
    memcpy(s_app.roster.names[0], s_app.local_nickname,
           sizeof(s_app.roster.names[0]));
    s_app.operation_started_ms = app_now_ms();
    s_app.last_beacon_ms = 0U;
    werewolf_net_set_phase((uint16_t)s_app.game.phase_epoch);
    show_lobby();
    send_beacon();
    s_app.last_beacon_ms = app_now_ms();
}

static void begin_join_scan(void)
{
    clear_session();
    s_app.is_host = false;
    s_app.local_seat = WEREWOLF_UI_NO_SEAT;
    s_app.session_id = random_u64_nonzero();
    s_app.protocol_epoch = random_u32_nonzero();
    if (esp_read_mac(s_app.local_mac, ESP_MAC_WIFI_STA) != ESP_OK ||
        !start_network(WEREWOLF_NET_ROLE_CLIENT, 1U, s_app.session_id,
                       s_app.protocol_epoch, NULL) ||
        !start_pairing_context()) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "SCAN SETUP FAILED");
        return;
    }
    s_app.state = APP_STATE_CLIENT_SCANNING;
    s_app.operation_started_ms = app_now_ms();
    werewolf_room_directory_init(&s_app.room_directory);
    show_room_list();
}

static uint8_t lowest_open_seat(uint8_t occupied)
{
    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if ((occupied & seat_bit(seat)) == 0U) {
            return seat;
        }
    }
    return WW_NO_PLAYER;
}

static bool host_prepare_next_offer(void)
{
    uint8_t public_key[WW_PAIRING_PUBLIC_KEY_SIZE] = { 0 };
    uint8_t seat;
    bool ok;

    destroy_pairing();
    seat = lowest_open_seat(s_app.game.joined_mask);
    if (seat == WW_NO_PLAYER) {
        return true;
    }
    ok = start_pairing_context() &&
         ww_pairing_generate_nonce(s_app.host_nonce) == WW_PAIRING_OK &&
         ww_pairing_get_public_key(&s_app.pairing, public_key) ==
             WW_PAIRING_OK &&
         ww_pairing_make_host_commitment(
             public_key, s_app.host_nonce, s_app.session_id,
             s_app.local_mac, seat, s_app.host_commitment) == WW_PAIRING_OK;
    secure_zero(public_key, sizeof(public_key));
    if (!ok) {
        destroy_pairing();
        return false;
    }
    s_app.offered_seat = seat;
    s_app.pair_state = APP_PAIR_HOST_OFFER;
    s_app.pair_started_ms = app_now_ms();
    s_app.last_beacon_ms = 0U;
    return true;
}

static bool derive_client_link(uint32_t *verification_code_value)
{
    uint8_t lmk[WW_PAIRING_ESPNOW_KEY_SIZE] = { 0 };
    char verification_code[WW_PAIRING_VERIFY_CODE_TEXT_SIZE] = { 0 };
    uint32_t parsed_code = 0U;
    bool ok = false;

    if (verification_code_value != NULL) {
        *verification_code_value = 0U;
    }
    if (verification_code_value != NULL &&
        ww_pairing_derive(&s_app.pairing, WW_PAIRING_ROLE_CLIENT,
                          s_app.peer_public_key, s_app.session_id,
                          s_app.local_mac, s_app.host_mac,
                          s_app.local_seat,
                          s_app.host_nonce, s_app.client_nonce,
                          s_app.host_commitment, s_app.client_commitment,
                          lmk, verification_code) == WW_PAIRING_OK &&
        verification_code_to_number(verification_code, &parsed_code)) {
        ok = werewolf_net_add_encrypted_peer(0U, s_app.host_mac, lmk,
                                              sizeof(lmk)) ==
             WEREWOLF_NET_OK;
    }
    if (ok) {
        *verification_code_value = parsed_code;
        ESP_LOGI(TAG, "client secure peer ready: seat=%u",
                 s_app.local_seat);
    } else {
        ESP_LOGW(TAG, "client secure peer setup failed: seat=%u",
                 s_app.local_seat);
    }
    secure_zero(lmk, sizeof(lmk));
    secure_zero(verification_code, sizeof(verification_code));
    return ok;
}

static void adopt_beacon(const werewolf_frame_t *frame,
                         const werewolf_beacon_message_t *beacon,
                         const uint8_t host_mac[WEREWOLF_NET_MAC_SIZE])
{
    uint8_t client_public_key[WW_PAIRING_PUBLIC_KEY_SIZE] = { 0 };
    uint8_t room_pmk[WEREWOLF_NET_KEY_SIZE] = { 0 };
    uint8_t candidate = beacon->offered_seat;
    bool committed;

    if (candidate == 0U || candidate >= WW_PLAYER_COUNT ||
        (beacon->occupied_mask & seat_bit(candidate)) != 0U) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_ROOM_FULL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "WAIT FOR AN OPEN SEAT");
        return;
    }
    s_app.session_id = frame->session_id;
    s_app.protocol_epoch = frame->epoch;
    s_app.local_seat = candidate;
    s_app.lobby_occupied_mask = beacon->occupied_mask;
    memcpy(s_app.host_mac, host_mac, sizeof(s_app.host_mac));
    memcpy(s_app.room_fingerprint, beacon->room_fingerprint,
           sizeof(s_app.room_fingerprint));
    memcpy(s_app.host_commitment, beacon->host_commitment,
           sizeof(s_app.host_commitment));
    if (ww_pairing_derive_room_pmk(
            s_app.session_id, s_app.protocol_epoch,
            s_app.room_fingerprint, sizeof(s_app.room_fingerprint),
            room_pmk) != WW_PAIRING_OK) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "ROOM KEY SETUP FAILED");
        return;
    }
    if (s_app.net_active) {
        werewolf_net_deinit();
        s_app.net_active = false;
    }
    if (!start_network(WEREWOLF_NET_ROLE_CLIENT, candidate,
                       s_app.session_id, s_app.protocol_epoch, room_pmk)) {
        secure_zero(room_pmk, sizeof(room_pmk));
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "SECURE PAIRING FAILED");
        return;
    }
    secure_zero(room_pmk, sizeof(room_pmk));
    committed = ww_pairing_generate_nonce(s_app.client_nonce) ==
                    WW_PAIRING_OK &&
                ww_pairing_get_public_key(&s_app.pairing,
                                          client_public_key) ==
                    WW_PAIRING_OK &&
                ww_pairing_make_client_commitment(
                    client_public_key, s_app.client_nonce,
                    s_app.session_id, s_app.host_mac, s_app.local_mac,
                    candidate, s_app.host_commitment,
                    s_app.client_commitment) == WW_PAIRING_OK;
    secure_zero(client_public_key, sizeof(client_public_key));
    if (!committed) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "PAIR COMMIT FAILED");
        return;
    }
    s_app.pair_state = APP_PAIR_CLIENT_COMMITTED;
    s_app.state = APP_STATE_CLIENT_JOINING;
    werewolf_room_directory_clear(&s_app.room_directory);
    s_app.ui.selected_room_token = WEREWOLF_ROOM_TOKEN_NONE;
    memset(s_app.ui.rooms, 0, sizeof(s_app.ui.rooms));
    s_app.operation_started_ms = app_now_ms();
    s_app.last_join_ms = 0U;
    show_scanning();
    send_join();
    s_app.last_join_ms = app_now_ms();
}

static void update_game_ui_common(void)
{
    const werewolf_public_state_message_t *state = &s_app.public_state;

    if (s_app.is_host) {
        (void)game_public_state(&s_app.public_state);
    }
    s_app.ui.is_host = s_app.is_host;
    s_app.ui.private_epoch = s_app.gate_epoch;
    s_app.ui.local_seat = s_app.local_seat;
    s_app.ui.local_role = ui_role(local_role());
    s_app.ui.round = state->round_number > UINT8_MAX
                         ? UINT8_MAX
                         : (uint8_t)state->round_number;
    s_app.ui.speaker_seat = state->current_speaker;
    s_app.ui.affected_seat = state->exiled_player;
    s_app.ui.votes_received = count_mask(state->submitted_mask);
    s_app.ui.votes_expected = count_mask(state->alive_mask);
    s_app.ui.connection = s_app.reconnecting
                              ? WEREWOLF_UI_CONNECTION_RECONNECTING
                              : WEREWOLF_UI_CONNECTION_ONLINE;
    s_app.ui.error = WEREWOLF_UI_ERROR_NONE;
    s_app.ui.winner = ui_winner(state->winner);
    s_app.ui.can_start = false;
    s_app.ui.recoverable = false;
    s_app.ui.waiting_for_players = false;
    s_app.ui.has_verify_code = false;
    s_app.ui.verify_code = 0U;
}

static void show_role_page(void)
{
    bool material_ready;

    update_game_ui_common();
    material_ready = s_app.have_private &&
        werewolf_messages_private_matches_gate(
            &s_app.private_state, WEREWOLF_GATE_ROLE,
            s_app.gate_epoch);
    s_app.ui.page = WEREWOLF_UI_PAGE_ROLE;
    s_app.ui.waiting_for_players = s_app.gate == APP_GATE_ROLE &&
                                   s_app.gate_acknowledged;
    s_app.ui.input_enabled = material_ready && !s_app.reconnecting &&
                             s_app.gate == APP_GATE_ROLE &&
                             !s_app.gate_acknowledged;
    if (!material_ready) {
        s_app.ui.connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
    }
    set_private_detail();
    publish_ui();
}

static void show_night_page(void)
{
    update_game_ui_common();
    s_app.ui.page = WEREWOLF_UI_PAGE_NIGHT_SELECT;
    s_app.ui.selected_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.input_enabled =
        !s_app.inflight_action.active && !s_app.reconnecting &&
        (alive_mask() & seat_bit(s_app.local_seat)) != 0U &&
        (s_app.public_state.submitted_mask & seat_bit(s_app.local_seat)) == 0U;
    s_app.ui.guide_seconds = 60U;
    publish_ui();
}

static void show_private_result_page(bool ready)
{
    bool material_ready;

    update_game_ui_common();
    material_ready = ready && s_app.private_result_ready &&
        s_app.have_private &&
        werewolf_messages_private_matches_gate(
            &s_app.private_state, WEREWOLF_GATE_PRIVATE_RESULT,
            s_app.gate_epoch);
    s_app.ui.page = WEREWOLF_UI_PAGE_PRIVATE_RESULT;
    s_app.ui.waiting_for_players =
        s_app.gate == APP_GATE_PRIVATE_RESULT &&
        s_app.gate_acknowledged;
    s_app.ui.input_enabled = material_ready && !s_app.reconnecting &&
                             s_app.gate == APP_GATE_PRIVATE_RESULT &&
                             !s_app.gate_acknowledged;
    if (!material_ready) {
        s_app.ui.connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
    }
    s_app.ui.private_seat = s_app.seer_result_seat;
    s_app.ui.private_faction = ui_faction(s_app.seer_result_faction);
    publish_ui();
}

static void show_dawn_page(void)
{
    update_game_ui_common();
    s_app.ui.page = WEREWOLF_UI_PAGE_DAY_RESULT;
    s_app.ui.input_enabled = !s_app.reconnecting &&
                             s_app.gate == APP_GATE_DAWN &&
                             !s_app.gate_acknowledged;
    (void)snprintf(s_app.ui.headline, sizeof(s_app.ui.headline), "DAWN RESULT");
    if (s_app.public_state.dawn_victim == WW_NO_PLAYER) {
        (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                       "NO ONE WAS ELIMINATED LAST NIGHT");
    } else {
        (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                       "SEAT %u WAS ELIMINATED. ROLE STAYS HIDDEN.",
                       (unsigned)s_app.public_state.dawn_victim + 1U);
    }
    publish_ui();
}

static void show_speaking_page(void)
{
    update_game_ui_common();
    s_app.ui.page = WEREWOLF_UI_PAGE_SPEAKING;
    s_app.ui.input_enabled =
        !s_app.inflight_action.active && !s_app.reconnecting &&
        s_app.public_state.current_speaker == s_app.local_seat &&
        (alive_mask() & seat_bit(s_app.local_seat)) != 0U;
    s_app.ui.guide_seconds = 60U;
    (void)snprintf(s_app.ui.headline, sizeof(s_app.ui.headline), "%s",
                   s_app.public_state.phase == WW_PHASE_TIE_DEFENSE
                       ? "TIED PLAYER DEFENCE"
                       : "SPEAK IN ORDER");
    (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                   "SPEECH IS LOCAL AND IS NOT RECORDED");
    publish_ui();
}

static void show_vote_page(void)
{
    update_game_ui_common();
    s_app.ui.page = WEREWOLF_UI_PAGE_VOTE_SELECT;
    s_app.ui.selected_seat = WEREWOLF_UI_NO_SEAT;
    s_app.ui.input_enabled =
        !s_app.inflight_action.active && !s_app.reconnecting &&
        (alive_mask() & seat_bit(s_app.local_seat)) != 0U &&
        (s_app.public_state.submitted_mask & seat_bit(s_app.local_seat)) == 0U;
    s_app.ui.guide_seconds = 45U;
    publish_ui();
}

static void show_exile_page(void)
{
    update_game_ui_common();
    s_app.ui.page = WEREWOLF_UI_PAGE_ELIMINATED;
    s_app.ui.input_enabled = !s_app.reconnecting &&
                             s_app.gate == APP_GATE_EXILE &&
                             !s_app.gate_acknowledged;
    s_app.ui.affected_seat = s_app.public_state.exiled_player;
    (void)snprintf(s_app.ui.headline, sizeof(s_app.ui.headline), "EXILE RESULT");
    if (s_app.public_state.exiled_player == WW_NO_PLAYER) {
        (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                       "REVOTE TIED. NO PLAYER WAS EXILED.");
    } else {
        (void)snprintf(s_app.ui.detail, sizeof(s_app.ui.detail),
                       "ROLE STAYS HIDDEN UNTIL GAME OVER");
    }
    publish_ui();
}

static void show_game_over_page(void)
{
    update_game_ui_common();
    s_app.ui.page = WEREWOLF_UI_PAGE_GAME_OVER;
    s_app.ui.input_enabled = !s_app.is_host ||
                             s_app.host_review_exit_unlocked;
    s_app.ui.winner = ui_winner(s_app.public_state.winner);
    publish_ui();
}

static void host_refresh_local_private(void)
{
    ww_private_view_t view;

    if (ww_game_get_private_view(&s_app.game, 0U, &view) != WW_OK) {
        return;
    }
    s_app.private_state = (werewolf_private_role_message_t){
        .role = view.role,
        .wolf_teammate_mask = view.wolf_teammates_mask,
        .seer_result_seat = view.alive && view.role == WW_ROLE_SEER
                                ? s_app.seer_result_seat
                                : WW_NO_PLAYER,
        .seer_result_faction = view.alive && view.role == WW_ROLE_SEER
                                   ? s_app.seer_result_faction
                                   : WW_CAMP_UNKNOWN,
        .gate_kind = (werewolf_gate_kind_t)s_app.gate,
        .gate_epoch = s_app.gate_epoch,
    };
    s_app.have_private = true;
    s_app.guard_previous_target = view.guard_previous_target;
}

static bool private_message_for(uint8_t seat, werewolf_message_t *message)
{
    ww_private_view_t view;

    if (message == NULL ||
        (s_app.gate != APP_GATE_ROLE &&
         s_app.gate != APP_GATE_PRIVATE_RESULT) ||
        ww_game_get_private_view(&s_app.game, seat, &view) != WW_OK) {
        return false;
    }
    memset(message, 0, sizeof(*message));
    message->type = WEREWOLF_MSG_PRIVATE_ROLE;
    message->body.private_role.role = view.role;
    message->body.private_role.wolf_teammate_mask = view.wolf_teammates_mask;
    message->body.private_role.seer_result_seat = WW_NO_PLAYER;
    message->body.private_role.seer_result_faction = WW_CAMP_UNKNOWN;
    message->body.private_role.gate_kind =
        (werewolf_gate_kind_t)s_app.gate;
    message->body.private_role.gate_epoch = s_app.gate_epoch;
    if (s_app.gate == APP_GATE_PRIVATE_RESULT && view.alive &&
        view.role == WW_ROLE_SEER &&
        s_app.seer_result_seat != WW_NO_PLAYER) {
        message->body.private_role.seer_result_seat = s_app.seer_result_seat;
        message->body.private_role.seer_result_faction =
            s_app.seer_result_faction;
    }
    return true;
}

static bool host_send_private_to(uint8_t seat)
{
    werewolf_message_t message;
    bool ok;

    if (seat == 0U || seat >= WW_PLAYER_COUNT ||
        !s_app.peers[seat].paired ||
        !private_message_for(seat, &message)) {
        return false;
    }
    ok = encode_and_send(seat, &message, s_app.game.phase_epoch, 0U);
    secure_zero(&message, sizeof(message));
    return ok;
}

static bool host_send_private_messages(void)
{
    bool ok = true;

    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if (s_app.peers[seat].paired) {
            ok = host_send_private_to(seat) && ok;
        }
    }
    host_refresh_local_private();
    return ok;
}

static bool host_send_role_reveal_to(uint8_t seat)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_SNAPSHOT,
        .body.snapshot.kind = WEREWOLF_SNAPSHOT_ROLE_REVEAL,
    };
    bool ok;

    if (seat == 0U || seat >= WW_PLAYER_COUNT ||
        !s_app.peers[seat].paired || !s_app.have_role_reveal ||
        s_app.game.phase != WW_PHASE_GAME_OVER ||
        s_app.gate != APP_GATE_NONE) {
        return false;
    }
    memcpy(message.body.snapshot.body.roles, s_app.revealed_roles,
           sizeof(message.body.snapshot.body.roles));
    ok = encode_and_send(seat, &message, s_app.game.phase_epoch, 0U);
    secure_zero(&message, sizeof(message));
    return ok;
}

static bool host_send_role_reveal(void)
{
    bool ok = true;

    if (ww_game_get_role_reveal(&s_app.game, s_app.revealed_roles) != WW_OK) {
        return false;
    }
    s_app.have_role_reveal = true;
    for (uint8_t seat = 1U; seat < WW_PLAYER_COUNT; ++seat) {
        if (s_app.peers[seat].paired) {
            ok = host_send_role_reveal_to(seat) && ok;
        }
    }
    return ok;
}

static void host_transport_abort(void)
{
    begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                     WEREWOLF_UI_ERROR_PROTOCOL,
                     WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                     "SECURE DELIVERY FAILED");
}

static bool host_prepare_authoritative_burst(void)
{
    werewolf_net_end_game();
    return werewolf_net_begin_game(WW_PLAYER_COUNT - 1U) == WEREWOLF_NET_OK;
}

static bool host_publish_phase(void)
{
    werewolf_message_t message = {
        .type = WEREWOLF_MSG_PHASE,
    };

    if (!game_public_state(&message.body.phase.public_state)) {
        begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "PUBLIC STATE BUILD FAILED");
        return false;
    }
    s_app.public_state = message.body.phase.public_state;
    s_app.have_public = true;
    werewolf_net_set_phase((uint16_t)s_app.game.phase_epoch);
    if (!host_prepare_authoritative_burst() ||
        !send_to_clients(&message, s_app.game.phase_epoch)) {
        host_transport_abort();
        return false;
    }
    return true;
}

static void host_open_night(void)
{
    advance_gate(APP_GATE_NONE);
    s_app.private_result_ready = false;
    s_app.private_result_confirmed = false;
    s_app.result_ack_mask = 0U;
    s_app.seer_result_seat = WW_NO_PLAYER;
    s_app.seer_result_faction = WW_CAMP_UNKNOWN;
    host_refresh_local_private();
    if (host_publish_phase()) {
        show_night_page();
    }
}

static void host_begin_private_result(void)
{
    advance_gate(APP_GATE_PRIVATE_RESULT);
    s_app.result_ack_mask = 0U;
    s_app.private_result_ready = true;
    s_app.private_result_confirmed = false;
    if (!host_publish_phase()) {
        return;
    }
    if (!host_send_private_messages()) {
        host_transport_abort();
        return;
    }
    show_private_result_page(true);
}

static void host_begin_dawn_gate(void)
{
    advance_gate(APP_GATE_DAWN);
    s_app.result_ack_mask = 0U;
    if (host_publish_phase()) {
        show_dawn_page();
    }
}

static void host_begin_exile_gate(void)
{
    advance_gate(APP_GATE_EXILE);
    s_app.result_ack_mask = 0U;
    if (host_publish_phase()) {
        show_exile_page();
    }
}

static void host_enter_game_over(void)
{
    advance_gate(APP_GATE_NONE);
    if (host_publish_phase()) {
        if (host_send_role_reveal()) {
            show_game_over_page();
        } else {
            host_transport_abort();
        }
    }
}

static bool all_gate_confirmed(void)
{
    return (s_app.result_ack_mask & s_app.game.joined_mask) ==
           s_app.game.joined_mask;
}

static void host_finish_gate(void)
{
    ww_status_t status;

    if (!all_gate_confirmed()) {
        return;
    }
    if (s_app.gate == APP_GATE_ROLE) {
        host_open_night();
        return;
    }
    if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
        host_begin_dawn_gate();
        return;
    }
    if (s_app.gate == APP_GATE_DAWN) {
        if (s_app.game.phase == WW_PHASE_GAME_OVER) {
            host_enter_game_over();
            return;
        }
        status = ww_game_begin_discussion(&s_app.game,
                                           s_app.game.phase_epoch);
        if (status == WW_OK) {
            advance_gate(APP_GATE_NONE);
            if (host_publish_phase()) {
                show_speaking_page();
            }
        }
        return;
    }
    if (s_app.gate == APP_GATE_EXILE) {
        if (s_app.game.phase == WW_PHASE_GAME_OVER) {
            host_enter_game_over();
            return;
        }
        status = ww_game_begin_next_night(&s_app.game,
                                           s_app.game.phase_epoch);
        if (status == WW_OK) {
            host_open_night();
        }
    }
}

static void host_capture_seer_result(void)
{
    s_app.seer_result_seat = WW_NO_PLAYER;
    s_app.seer_result_faction = WW_CAMP_UNKNOWN;
    for (uint8_t seat = 0U; seat < WW_PLAYER_COUNT; ++seat) {
        if ((s_app.game.alive_mask & seat_bit(seat)) != 0U &&
            s_app.game.roles[seat] == WW_ROLE_SEER &&
            s_app.game.action_targets[seat] < WW_PLAYER_COUNT) {
            uint8_t target = s_app.game.action_targets[seat];
            s_app.seer_result_seat = target;
            s_app.seer_result_faction = ww_role_camp(s_app.game.roles[target]);
            break;
        }
    }
}

static void host_after_night_action(void)
{
    ww_status_t status;

    if ((s_app.game.submitted_mask & s_app.game.alive_mask) !=
        s_app.game.alive_mask) {
        (void)game_public_state(&s_app.public_state);
        show_night_page();
        return;
    }
    if (s_app.game.phase == WW_PHASE_NIGHT) {
        host_capture_seer_result();
    }
    status = ww_game_finalize_night(&s_app.game, s_app.game.phase_epoch);
    if (status != WW_OK) {
        begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "NIGHT RESOLUTION FAILED");
        return;
    }
    if (s_app.game.dawn_victim < WW_PLAYER_COUNT &&
        s_app.game.roles[s_app.game.dawn_victim] == WW_ROLE_SEER) {
        /* The normative MVP rule gives dead players no newly learned secret,
         * including a Seer killed in the same night resolution. */
        s_app.seer_result_seat = WW_NO_PLAYER;
        s_app.seer_result_faction = WW_CAMP_UNKNOWN;
    }
    if (s_app.game.phase == WW_PHASE_WOLF_REVOTE) {
        if (host_publish_phase()) {
            show_night_page();
        }
    } else {
        host_begin_private_result();
    }
}

static void host_after_vote(void)
{
    ww_status_t status;

    if ((s_app.game.submitted_mask & s_app.game.alive_mask) !=
        s_app.game.alive_mask) {
        (void)game_public_state(&s_app.public_state);
        show_vote_page();
        return;
    }
    status = ww_game_finalize_vote(&s_app.game, s_app.game.phase_epoch);
    if (status != WW_OK) {
        begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "VOTE RESOLUTION FAILED");
        return;
    }
    if (s_app.game.phase == WW_PHASE_TIE_DEFENSE) {
        if (host_publish_phase()) {
            show_speaking_page();
        }
    } else {
        host_begin_exile_gate();
    }
}

static void host_process_action(uint8_t actor,
                                const werewolf_action_message_t *action)
{
    ww_status_t status = WW_ERR_INVALID_PHASE;

    if (actor >= WW_PLAYER_COUNT || action == NULL ||
        action->expected_phase_epoch != s_app.game.phase_epoch ||
        !werewolf_messages_action_matches_gate(
            action, (werewolf_gate_kind_t)s_app.gate,
            s_app.gate_epoch)) {
        return;
    }
    if (action->kind == WEREWOLF_ACTION_ROLE_SEEN &&
        s_app.gate == APP_GATE_ROLE) {
        s_app.role_seen_mask |= seat_bit(actor);
        s_app.result_ack_mask = s_app.role_seen_mask;
        if (actor == 0U) {
            s_app.role_confirmed = true;
            s_app.gate_acknowledged = true;
        }
        show_role_page();
        host_finish_gate();
        return;
    }
    if (action->kind == WEREWOLF_ACTION_ACK_RESULT &&
        s_app.gate != APP_GATE_NONE && s_app.gate != APP_GATE_ROLE) {
        s_app.result_ack_mask |= seat_bit(actor);
        if (actor == 0U) {
            s_app.gate_acknowledged = true;
            if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
                s_app.private_result_confirmed = true;
            }
        }
        if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
            show_private_result_page(true);
        } else if (s_app.gate == APP_GATE_DAWN) {
            show_dawn_page();
        } else if (s_app.gate == APP_GATE_EXILE) {
            show_exile_page();
        }
        host_finish_gate();
        return;
    }
    if (s_app.gate != APP_GATE_NONE) {
        return;
    }
    if (action->kind == WEREWOLF_ACTION_NIGHT_TARGET) {
        status = ww_game_submit_night_action(&s_app.game,
                                              action->expected_phase_epoch,
                                              actor, action->target);
        if (status == WW_OK) {
            if (actor == 0U && local_role() == WW_ROLE_GUARD &&
                s_app.game.phase == WW_PHASE_NIGHT) {
                s_app.guard_previous_target = action->target;
            }
            host_after_night_action();
        }
        return;
    }
    if (action->kind == WEREWOLF_ACTION_PASS_SPEECH) {
        status = ww_game_pass_speaker(&s_app.game,
                                      action->expected_phase_epoch, actor);
        if (status == WW_OK) {
            if (s_app.game.current_speaker == WW_NO_PLAYER) {
                status = s_app.game.phase == WW_PHASE_TIE_DEFENSE
                             ? ww_game_begin_revote(
                                   &s_app.game, s_app.game.phase_epoch)
                             : ww_game_begin_vote(
                                   &s_app.game, s_app.game.phase_epoch);
            }
            if (status == WW_OK) {
                if (host_publish_phase()) {
                    if (s_app.game.phase == WW_PHASE_VOTE ||
                        s_app.game.phase == WW_PHASE_REVOTE) {
                        show_vote_page();
                    } else {
                        show_speaking_page();
                    }
                }
            }
        }
        return;
    }
    if (action->kind == WEREWOLF_ACTION_VOTE_TARGET) {
        status = ww_game_submit_vote(&s_app.game,
                                     action->expected_phase_epoch,
                                     actor, action->target);
        if (status == WW_OK) {
            host_after_vote();
        }
    }
}

static void host_begin_game(void)
{
    uint64_t shuffle_seed;
    werewolf_message_t start = {
        .type = WEREWOLF_MSG_START,
    };

    if (!roster_complete(s_app.game.joined_mask) ||
        !host_client_links_complete(s_app.game.joined_mask) ||
        !werewolf_lobby_can_start(s_app.game.joined_mask,
                                  s_app.roster.profile_mask,
                                  s_app.ready_mask)) {
        werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        show_lobby();
        return;
    }
    /* Lobby heartbeats are obsolete once START is committed. Clearing them
     * guarantees exactly twelve slots for six START and six private frames. */
    werewolf_net_end_game();
    if (werewolf_net_begin_game(WW_PLAYER_COUNT - 1U) != WEREWOLF_NET_OK) {
        werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        show_lobby();
        return;
    }
    shuffle_seed = random_u64_nonzero();
    if (ww_game_start(&s_app.game, shuffle_seed) != WW_OK) {
        begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "GAME START FAILED");
        return;
    }
    advance_gate(APP_GATE_ROLE);
    if (!game_public_state(&start.body.start.public_state)) {
        begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "GAME START FAILED");
        return;
    }
    start.body.start.roster = s_app.roster;
    s_app.game_started = true;
    s_app.state = APP_STATE_GAME;
    clear_lobby_verification_codes();
    s_app.role_seen_mask = 0U;
    s_app.result_ack_mask = 0U;
    s_app.role_confirmed = false;
    s_app.seer_result_seat = WW_NO_PLAYER;
    s_app.seer_result_faction = WW_CAMP_UNKNOWN;
    s_app.public_state = start.body.start.public_state;
    s_app.have_public = true;
    werewolf_net_set_phase((uint16_t)s_app.game.phase_epoch);
    if (!send_to_clients(&start, s_app.game.phase_epoch) ||
        !host_send_private_messages()) {
        host_transport_abort();
        destroy_pairing();
        return;
    }
    destroy_pairing();
    show_role_page();
}

static bool host_send_accept(uint8_t seat)
{
    werewolf_message_t accept = {
        .type = WEREWOLF_MSG_ACCEPT,
        .body.accept = {
            .seat = seat,
        },
    };

    bool accepted = encode_and_send(seat, &accept,
                                    s_app.game.phase_epoch, 0U);
    ESP_LOGI(TAG, "host response queued: seat=%u accept=%s",
             seat, accepted ? "yes" : "no");
    return accepted;
}

static void host_process_join(const werewolf_frame_t *frame,
                              const werewolf_join_message_t *join,
                              const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    uint8_t seat = join->candidate_seat;

    if (s_app.state != APP_STATE_HOST_LOBBY ||
        frame->session_id != s_app.session_id ||
        frame->epoch != s_app.protocol_epoch || frame->src != seat ||
        seat != s_app.offered_seat || !s_app.pairing_active ||
        (s_app.pair_state != APP_PAIR_HOST_OFFER &&
         s_app.pair_state != APP_PAIR_HOST_LOCKED) ||
        (s_app.game.joined_mask & seat_bit(seat)) != 0U) {
        return;
    }
    for (uint8_t other = 1U; other < WW_PLAYER_COUNT; ++other) {
        if (s_app.peers[other].paired &&
            memcmp(s_app.peers[other].mac, mac, WEREWOLF_NET_MAC_SIZE) == 0) {
            return;
        }
    }

    if (s_app.pair_state == APP_PAIR_HOST_OFFER) {
        memcpy(s_app.locked_client_mac, mac,
               sizeof(s_app.locked_client_mac));
        memcpy(s_app.client_commitment, join->client_commitment,
               sizeof(s_app.client_commitment));
        s_app.pair_state = APP_PAIR_HOST_LOCKED;
        s_app.pair_started_ms = app_now_ms();
    }
    /* Always echo the first locked commitment.  Contending clients detect a
     * mismatch and return to scanning without revealing their key. */
    send_pair_host_reveal();
    s_app.last_beacon_ms = app_now_ms();
}

static void host_process_pair_client_reveal(
    const werewolf_frame_t *frame,
    const werewolf_pair_client_reveal_message_t *reveal,
    const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    uint8_t lmk[WW_PAIRING_ESPNOW_KEY_SIZE] = { 0 };
    char verification_code_text[WW_PAIRING_VERIFY_CODE_TEXT_SIZE] = { 0 };
    uint32_t verification_code = 0U;
    uint8_t seat = reveal->candidate_seat;
    app_peer_t *peer;
    bool ok;

    if (s_app.state != APP_STATE_HOST_LOBBY ||
        frame->session_id != s_app.session_id ||
        frame->epoch != s_app.protocol_epoch || frame->src != seat ||
        seat == 0U || seat >= WW_PLAYER_COUNT) {
        return;
    }
    peer = &s_app.peers[seat];
    if (peer->paired) {
        /* The original ACCEPT is already tracked by the reliable layer.
         * Minting a new sequence here can make two ACCEPTs reorder across the
         * client's JOINING -> LOBBY transition. */
        return;
    }
    if (s_app.pair_state != APP_PAIR_HOST_LOCKED ||
        seat != s_app.offered_seat || !s_app.pairing_active ||
        memcmp(s_app.locked_client_mac, mac, WEREWOLF_NET_MAC_SIZE) != 0 ||
        (s_app.game.joined_mask & seat_bit(seat)) != 0U ||
        !bytes_equal_constant_time(
            reveal->echoed_host_commitment, s_app.host_commitment,
            sizeof(s_app.host_commitment)) ||
        ww_pairing_verify_client_commitment(
            s_app.client_commitment, reveal->client_public_key,
            reveal->client_nonce, s_app.session_id, s_app.local_mac, mac,
            seat, s_app.host_commitment) != WW_PAIRING_OK) {
        return;
    }
    ok = ww_pairing_derive(
             &s_app.pairing, WW_PAIRING_ROLE_HOST,
             reveal->client_public_key, s_app.session_id,
             s_app.local_mac, mac, seat,
             s_app.host_nonce, reveal->client_nonce,
             s_app.host_commitment, s_app.client_commitment,
             lmk, verification_code_text) == WW_PAIRING_OK &&
         verification_code_to_number(verification_code_text,
                                     &verification_code) &&
         werewolf_net_add_encrypted_peer(seat, mac, lmk, sizeof(lmk)) ==
             WEREWOLF_NET_OK &&
         ww_game_join(&s_app.game, seat) == WW_OK;
    secure_zero(lmk, sizeof(lmk));
    secure_zero(verification_code_text, sizeof(verification_code_text));
    if (!ok) {
        ESP_LOGW(TAG, "host secure peer setup failed: seat=%u", seat);
        return;
    }

    ESP_LOGI(TAG, "host secure peer ready: seat=%u", seat);

    peer->paired = true;
    peer->ready = false;
    peer->verification_code = verification_code;
    memcpy(peer->mac, mac, sizeof(peer->mac));
    memcpy(peer->public_key, reveal->client_public_key,
           sizeof(peer->public_key));
    s_app.lobby_occupied_mask = s_app.game.joined_mask;
    bump_lobby_revision();
    werewolf_net_set_phase((uint16_t)s_app.game.phase_epoch);
    if (!host_send_accept(seat)) {
        host_remove_lobby_peer(seat);
        return;
    }
    if (!host_prepare_next_offer()) {
        begin_host_fatal(WEREWOLF_ABORT_SECURITY_FAILURE,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "NEXT PAIR OFFER FAILED");
        return;
    }
    show_lobby();
}

static bool host_clear_lobby_peer(uint8_t seat)
{
    if (seat == 0U || seat >= WW_PLAYER_COUNT ||
        !s_app.peers[seat].paired) {
        return false;
    }
    (void)werewolf_net_remove_peer(seat);
    (void)ww_game_leave_lobby(&s_app.game, seat);
    secure_zero(&s_app.peers[seat], sizeof(s_app.peers[seat]));
    secure_zero(s_app.roster.names[seat],
                sizeof(s_app.roster.names[seat]));
    s_app.roster.profile_mask &= (uint8_t)~seat_bit(seat);
    s_app.ready_mask &= (uint8_t)~seat_bit(seat);
    if (s_app.ui.kick_seat == seat) {
        s_app.ui.kick_seat = WEREWOLF_UI_NO_SEAT;
    }
    if (s_app.ui.selected_seat == seat) {
        s_app.ui.selected_seat = WEREWOLF_UI_NO_SEAT;
    }
    s_app.lobby_occupied_mask = s_app.game.joined_mask;
    bump_lobby_revision();
    werewolf_net_set_phase((uint16_t)s_app.game.phase_epoch);
    return true;
}

static void host_remove_lobby_peer(uint8_t seat)
{
    if (!host_clear_lobby_peer(seat)) {
        return;
    }
    if (!host_prepare_next_offer()) {
        begin_host_fatal(WEREWOLF_ABORT_SECURITY_FAILURE,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "PAIR OFFER ROTATION FAILED");
        return;
    }
    show_lobby();
}

static void host_retire_finished_peer(uint8_t seat)
{
    if (seat == 0U || seat >= WW_PLAYER_COUNT ||
        !s_app.peers[seat].paired ||
        s_app.game.phase != WW_PHASE_GAME_OVER ||
        s_app.gate != APP_GATE_NONE) {
        return;
    }
    /* The authoritative game/deck stays immutable for every remaining
     * reviewer; only this transport identity and its retry state are retired. */
    (void)werewolf_net_remove_peer(seat);
    secure_zero(&s_app.peers[seat], sizeof(s_app.peers[seat]));
}

static void host_drop_transport_peer(uint8_t seat)
{
    if (seat == 0U || seat >= WW_PLAYER_COUNT ||
        !s_app.peers[seat].paired) {
        return;
    }
    (void)werewolf_net_remove_peer(seat);
    secure_zero(&s_app.peers[seat], sizeof(s_app.peers[seat]));
}

static void host_process_ready(uint8_t seat,
                               const werewolf_ready_message_t *ready)
{
    if (!host_lobby_state_active() || seat == 0U ||
        seat >= WW_PLAYER_COUNT || !s_app.peers[seat].paired ||
        (s_app.roster.profile_mask & seat_bit(seat)) == 0U) {
        return;
    }
    if (s_app.peers[seat].ready == ready->ready) {
        return;
    }
    s_app.peers[seat].ready = ready->ready;
    if (ready->ready) {
        s_app.ready_mask |= seat_bit(seat);
    } else {
        s_app.ready_mask &= (uint8_t)~seat_bit(seat);
    }
    bump_lobby_revision();
    if (s_app.state == APP_STATE_HOST_LOBBY) {
        show_lobby();
    }
}

static void host_process_profile(uint8_t seat,
                                 const werewolf_profile_message_t *profile)
{
    if (!host_lobby_state_active() || seat == 0U ||
        seat >= WW_PLAYER_COUNT || !s_app.peers[seat].paired) {
        return;
    }
    if ((s_app.roster.profile_mask & seat_bit(seat)) != 0U) {
        if (strcmp(s_app.roster.names[seat], profile->nickname) != 0) {
            ESP_LOGW(TAG,
                     "ignored conflicting nickname update: seat=%u",
                     seat);
        }
        return;
    }
    memcpy(s_app.roster.names[seat], profile->nickname,
           sizeof(s_app.roster.names[seat]));
    s_app.roster.profile_mask |= seat_bit(seat);
    s_app.ready_mask &= (uint8_t)~seat_bit(seat);
    bump_lobby_revision();
    s_app.last_snapshot_ms = 0U;
    if (s_app.state == APP_STATE_HOST_LOBBY) {
        show_lobby();
    }
}

static void client_process_pair_host_reveal(
    const werewolf_pair_host_reveal_message_t *reveal,
    const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    uint32_t verification_code = 0U;

    if (s_app.state != APP_STATE_CLIENT_JOINING ||
        (s_app.pair_state != APP_PAIR_CLIENT_COMMITTED &&
         s_app.pair_state != APP_PAIR_CLIENT_REVEALED) ||
        reveal->offered_seat != s_app.local_seat ||
        memcmp(mac, s_app.host_mac, WEREWOLF_NET_MAC_SIZE) != 0) {
        return;
    }
    if (!bytes_equal_constant_time(
            reveal->locked_client_commitment, s_app.client_commitment,
            sizeof(s_app.client_commitment))) {
        /* Another client won this single-seat offer.  Discard our unrevealed
         * key and scan for the host's next freshly committed offer. */
        begin_join_scan();
        return;
    }
    if (ww_pairing_verify_host_commitment(
            s_app.host_commitment, reveal->host_public_key,
            reveal->host_nonce, s_app.session_id, s_app.host_mac,
            s_app.local_seat) != WW_PAIRING_OK) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                   "HOST COMMITMENT MISMATCH");
        return;
    }
    if (s_app.pair_state == APP_PAIR_CLIENT_REVEALED) {
        if (!bytes_equal_constant_time(
                s_app.peer_public_key, reveal->host_public_key,
                sizeof(s_app.peer_public_key)) ||
            !bytes_equal_constant_time(
                s_app.host_nonce, reveal->host_nonce,
                sizeof(s_app.host_nonce))) {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                       "HOST REVEAL CHANGED");
            return;
        }
        send_pair_client_reveal();
        s_app.last_join_ms = app_now_ms();
        return;
    }

    memcpy(s_app.peer_public_key, reveal->host_public_key,
           sizeof(s_app.peer_public_key));
    memcpy(s_app.host_nonce, reveal->host_nonce,
           sizeof(s_app.host_nonce));
    if (!derive_client_link(&verification_code)) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                   "SECURE PAIRING FAILED");
        return;
    }
    s_app.client_verification_code = verification_code;
    s_app.pair_state = APP_PAIR_CLIENT_REVEALED;
    send_pair_client_reveal();
    s_app.last_join_ms = app_now_ms();
}

static void client_accept(const werewolf_accept_message_t *accept)
{
    if (s_app.state == APP_STATE_CLIENT_LOBBY) {
        if (accept->seat == s_app.local_seat) {
            if (!s_app.profile_confirmed && send_profile()) {
                s_app.last_profile_ms = app_now_ms();
            }
            return;
        }
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                   "CONFLICTING ACCEPT");
        return;
    }
    if (s_app.state != APP_STATE_CLIENT_JOINING ||
        s_app.pair_state != APP_PAIR_CLIENT_REVEALED ||
        accept->seat != s_app.local_seat) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "ACCEPT SEAT MISMATCH");
        return;
    }
    destroy_pairing();
    s_app.state = APP_STATE_CLIENT_LOBBY;
    s_app.lobby_occupied_mask |= seat_bit(0U) | seat_bit(s_app.local_seat);
    s_app.last_host_rx_ms = app_now_ms();
    s_app.local_ready = false;
    s_app.profile_confirmed = false;
    s_app.profile_started_ms = app_now_ms();
    if (send_profile()) {
        s_app.last_profile_ms = app_now_ms();
    }
    ESP_LOGI(TAG, "client accepted into lobby: seat=%u", accept->seat);
    show_lobby();
}

static bool private_identity_matches_base(
    const werewolf_private_role_message_t *private_message)
{
    return private_message != NULL && s_app.have_private &&
           private_message->role == s_app.private_state.role &&
           private_message->wolf_teammate_mask ==
               s_app.private_state.wolf_teammate_mask;
}

static bool client_activate_role(
    const werewolf_private_role_message_t *private_message)
{
    if (private_message == NULL ||
        private_message->gate_kind != WEREWOLF_GATE_ROLE) {
        return false;
    }
    if (s_app.have_private) {
        return private_identity_matches_base(private_message);
    }
    s_app.private_state = *private_message;
    s_app.have_private = true;
    return true;
}

static bool client_activate_private_result(
    const werewolf_private_role_message_t *private_message)
{
    if (private_message == NULL ||
        private_message->gate_kind != WEREWOLF_GATE_PRIVATE_RESULT ||
        !private_identity_matches_base(private_message)) {
        return false;
    }
    s_app.seer_result_seat = private_message->seer_result_seat;
    s_app.seer_result_faction = private_message->seer_result_faction;
    s_app.private_state.seer_result_seat =
        private_message->seer_result_seat;
    s_app.private_state.seer_result_faction =
        private_message->seer_result_faction;
    s_app.private_state.gate_kind = private_message->gate_kind;
    s_app.private_state.gate_epoch = private_message->gate_epoch;
    s_app.private_result_ready = true;
    return true;
}

static void clear_pending_private(
    werewolf_private_role_message_t *private_message, bool *present)
{
    if (private_message != NULL) {
        secure_zero(private_message, sizeof(*private_message));
    }
    if (present != NULL) {
        *present = false;
    }
}

static void stash_private(
    const werewolf_private_role_message_t *private_message)
{
    werewolf_private_role_message_t *slot;
    bool *present;

    if (private_message->gate_kind == WEREWOLF_GATE_ROLE) {
        slot = &s_app.pending_role;
        present = &s_app.have_pending_role;
    } else {
        slot = &s_app.pending_private_result;
        present = &s_app.have_pending_private_result;
    }
    if (*present &&
        (int32_t)(private_message->gate_epoch - slot->gate_epoch) < 0) {
        return;
    }
    *slot = *private_message;
    *present = true;
}

static void client_sync_pending_private(
    const werewolf_public_state_message_t *state, bool gate_changed)
{
    int32_t delta;

    if (gate_changed) {
        s_app.private_result_ready = false;
        s_app.seer_result_seat = WW_NO_PLAYER;
        s_app.seer_result_faction = WW_CAMP_UNKNOWN;
        if (s_app.have_private) {
            s_app.private_state.seer_result_seat = WW_NO_PLAYER;
            s_app.private_state.seer_result_faction = WW_CAMP_UNKNOWN;
        }
    }
    if (s_app.have_pending_role) {
        delta = (int32_t)(s_app.pending_role.gate_epoch -
                          state->gate_epoch);
        if (delta < 0 ||
            (delta == 0 &&
             !werewolf_messages_private_matches_gate(
                 &s_app.pending_role, state->gate_kind,
                 state->gate_epoch))) {
            clear_pending_private(&s_app.pending_role,
                                  &s_app.have_pending_role);
        } else if (delta == 0) {
            (void)client_activate_role(&s_app.pending_role);
            clear_pending_private(&s_app.pending_role,
                                  &s_app.have_pending_role);
        }
    }
    if (s_app.have_pending_private_result) {
        delta = (int32_t)(s_app.pending_private_result.gate_epoch -
                          state->gate_epoch);
        if (delta < 0 ||
            (delta == 0 &&
             !werewolf_messages_private_matches_gate(
                 &s_app.pending_private_result, state->gate_kind,
                 state->gate_epoch))) {
            clear_pending_private(&s_app.pending_private_result,
                                  &s_app.have_pending_private_result);
        } else if (delta == 0 &&
                   client_activate_private_result(
                       &s_app.pending_private_result)) {
            clear_pending_private(&s_app.pending_private_result,
                                  &s_app.have_pending_private_result);
        }
    }
}

static bool client_activate_pending_role_reveal(void)
{
    if (s_app.have_role_reveal || !s_app.have_pending_role_reveal ||
        !werewolf_messages_public_allows_role_reveal(
            &s_app.public_state)) {
        return false;
    }
    memcpy(s_app.revealed_roles, s_app.pending_revealed_roles,
           sizeof(s_app.revealed_roles));
    secure_zero(s_app.pending_revealed_roles,
                sizeof(s_app.pending_revealed_roles));
    s_app.have_pending_role_reveal = false;
    s_app.have_role_reveal = true;
    return true;
}

static void client_process_role_reveal(
    const ww_role_t roles[WW_PLAYER_COUNT])
{
    if (roles == NULL) {
        return;
    }
    if (s_app.have_role_reveal) {
        if (werewolf_messages_public_allows_role_reveal(
                &s_app.public_state)) {
            show_game_over_page();
        }
        return;
    }
    if (s_app.have_public &&
        werewolf_messages_public_allows_role_reveal(
            &s_app.public_state)) {
        memcpy(s_app.revealed_roles, roles, sizeof(s_app.revealed_roles));
        s_app.have_role_reveal = true;
        show_game_over_page();
        return;
    }
    if (!s_app.have_pending_role_reveal) {
        memcpy(s_app.pending_revealed_roles, roles,
               sizeof(s_app.pending_revealed_roles));
        s_app.have_pending_role_reveal = true;
    }
}

static void client_apply_public(
    const werewolf_public_state_message_t *state, bool phase_signal,
    bool gate_ack_known, bool gate_acknowledged)
{
    bool newer;
    bool gate_changed;
    bool gate_ack_changed = false;
    bool entering_game;

    if (state == NULL) {
        return;
    }
    entering_game = !s_app.game_started;
    newer = !s_app.have_public ||
            (int32_t)(state->phase_epoch - s_app.public_state.phase_epoch) > 0;
    if (s_app.have_public) {
        int32_t phase_delta =
            (int32_t)(state->phase_epoch - s_app.public_state.phase_epoch);
        int32_t gate_delta =
            (int32_t)(state->gate_epoch - s_app.public_state.gate_epoch);

        if (phase_delta < 0 || gate_delta < 0) {
            return;
        }
    }
    gate_changed = !s_app.have_public ||
                   state->gate_epoch != s_app.public_state.gate_epoch ||
                   state->gate_kind != s_app.public_state.gate_kind;
    if (gate_changed) {
        s_app.gate_acknowledged = false;
        if (state->gate_kind == WEREWOLF_GATE_ROLE) {
            s_app.role_confirmed = false;
        } else if (state->gate_kind == WEREWOLF_GATE_PRIVATE_RESULT) {
            s_app.private_result_confirmed = false;
        }
    }
    client_sync_pending_private(state, gate_changed);
    if (gate_ack_known) {
        gate_ack_changed =
            s_app.gate_acknowledged != gate_acknowledged;
        s_app.gate_acknowledged = gate_acknowledged;
        if (state->gate_kind == WEREWOLF_GATE_ROLE) {
            s_app.role_confirmed = gate_acknowledged;
        } else if (state->gate_kind == WEREWOLF_GATE_PRIVATE_RESULT) {
            s_app.private_result_confirmed = gate_acknowledged;
        }
    }
    s_app.public_state = *state;
    s_app.gate = (app_gate_t)state->gate_kind;
    s_app.gate_epoch = state->gate_epoch;
    s_app.have_public = true;
    s_app.game_started = true;
    s_app.state = APP_STATE_GAME;
    if (entering_game) {
        clear_lobby_verification_codes();
    }
    (void)client_activate_pending_role_reveal();
    werewolf_net_set_phase((uint16_t)state->phase_epoch);
    if (!phase_signal && !s_app.reconnecting && !newer &&
        !gate_changed && !gate_ack_changed) {
        if (s_app.gate == APP_GATE_ROLE) {
            show_role_page();
            return;
        }
        if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
            show_private_result_page(s_app.private_result_ready);
            return;
        }
        update_game_ui_common();
        publish_ui();
        return;
    }
    s_app.reconnecting = false;
    if (s_app.gate == APP_GATE_ROLE) {
        show_role_page();
        return;
    }
    if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
        show_private_result_page(s_app.private_result_ready);
        return;
    }
    if (s_app.gate == APP_GATE_DAWN) {
        show_dawn_page();
        return;
    }
    if (s_app.gate == APP_GATE_EXILE) {
        show_exile_page();
        return;
    }
    if (state->phase == WW_PHASE_NIGHT ||
        state->phase == WW_PHASE_WOLF_REVOTE) {
        if (newer || gate_changed) {
            s_app.private_result_ready = false;
            s_app.private_result_confirmed = false;
            s_app.seer_result_seat = WW_NO_PLAYER;
            s_app.seer_result_faction = WW_CAMP_UNKNOWN;
            if (s_app.have_private) {
                s_app.private_state.seer_result_seat = WW_NO_PLAYER;
                s_app.private_state.seer_result_faction = WW_CAMP_UNKNOWN;
            }
        }
        show_night_page();
        return;
    }
    if (state->phase == WW_PHASE_DISCUSSION) {
        show_speaking_page();
    } else if (state->phase == WW_PHASE_VOTE ||
               state->phase == WW_PHASE_REVOTE) {
        show_vote_page();
    } else if (state->phase == WW_PHASE_TIE_DEFENSE) {
        show_speaking_page();
    } else if (state->phase == WW_PHASE_EXILE_RESULT) {
        show_exile_page();
    } else if (state->phase == WW_PHASE_GAME_OVER &&
               s_app.have_role_reveal) {
        show_game_over_page();
    }
}

static void client_process_private(
    const werewolf_private_role_message_t *private_message)
{
    int32_t gate_delta;

    if (private_message == NULL) {
        return;
    }
    if (!s_app.have_public) {
        stash_private(private_message);
        return;
    }
    gate_delta = (int32_t)(private_message->gate_epoch -
                           s_app.public_state.gate_epoch);
    if (gate_delta < 0 ||
        (gate_delta == 0 &&
         !werewolf_messages_private_matches_gate(
             private_message, s_app.public_state.gate_kind,
             s_app.public_state.gate_epoch))) {
        return;
    }
    if (gate_delta > 0) {
        stash_private(private_message);
        return;
    }
    if (private_message->gate_kind == WEREWOLF_GATE_ROLE) {
        if (client_activate_role(private_message)) {
            show_role_page();
        }
    } else if (client_activate_private_result(private_message)) {
        show_private_result_page(true);
    }
}

static void clear_inflight_action(void)
{
    secure_zero(&s_app.inflight_action, sizeof(s_app.inflight_action));
}

static void client_render_normal_phase(void)
{
    if (s_app.gate != APP_GATE_NONE) {
        return;
    }
    if (s_app.public_state.phase == WW_PHASE_NIGHT ||
        s_app.public_state.phase == WW_PHASE_WOLF_REVOTE) {
        show_night_page();
    } else if (s_app.public_state.phase == WW_PHASE_DISCUSSION ||
               s_app.public_state.phase == WW_PHASE_TIE_DEFENSE) {
        show_speaking_page();
    } else if (s_app.public_state.phase == WW_PHASE_VOTE ||
               s_app.public_state.phase == WW_PHASE_REVOTE) {
        show_vote_page();
    }
}

static void client_reconcile_inflight_action(
    const werewolf_public_state_message_t *state)
{
    werewolf_net_snapshot_t net_state;
    uint32_t now;

    if (!s_app.inflight_action.active || state == NULL) {
        return;
    }
    if (werewolf_messages_public_confirms_normal_action(
            s_app.inflight_action.kind,
            s_app.inflight_action.phase_epoch,
            s_app.local_seat,
            s_app.inflight_action.speaker_seat,
            state)) {
        if (s_app.inflight_action.commits_guard_target) {
            s_app.guard_previous_target = s_app.inflight_action.target;
        }
        clear_inflight_action();
        client_render_normal_phase();
        return;
    }
    if (s_app.game_snapshot_revision ==
            s_app.inflight_action.snapshot_revision) {
        return;
    }
    now = app_now_ms();
    werewolf_net_snapshot(&net_state);
    if (net_state.pending_count == 0U &&
        elapsed_ms(now, s_app.inflight_action.sent_ms,
                   APP_ACTION_SETTLE_MS)) {
        /* At least one post-send encrypted Host snapshot disagrees and the
         * transport has no retry left.  Reopen instead of trusting a lost
         * delivery-failure callback or a permanent UI latch. */
        clear_inflight_action();
        client_render_normal_phase();
    }
}

static void client_process_snapshot(
    const werewolf_snapshot_message_t *snapshot, bool was_reconnecting)
{
    if (snapshot->kind == WEREWOLF_SNAPSHOT_LOBBY &&
        s_app.state == APP_STATE_CLIENT_LOBBY) {
        uint32_t revision = snapshot->body.lobby.roster.lobby_revision;

        if ((int32_t)(revision - s_app.roster.lobby_revision) < 0) {
            return;
        }
        if (revision == s_app.roster.lobby_revision) {
            if (memcmp(&snapshot->body.lobby.roster, &s_app.roster,
                       sizeof(s_app.roster)) != 0) {
                clear_session();
                show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                           WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                           "LOBBY REVISION CONFLICT");
            }
            return;
        }
        if ((snapshot->body.lobby.occupied_mask &
             (seat_bit(0U) | seat_bit(s_app.local_seat))) !=
                (seat_bit(0U) | seat_bit(s_app.local_seat))) {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                       "LOCAL SEAT MISSING");
            return;
        }
        s_app.lobby_occupied_mask = snapshot->body.lobby.occupied_mask;
        s_app.ready_mask = snapshot->body.lobby.ready_mask;
        s_app.roster = snapshot->body.lobby.roster;
        if ((s_app.roster.profile_mask & seat_bit(s_app.local_seat)) != 0U &&
            strcmp(s_app.roster.names[s_app.local_seat],
                   s_app.local_nickname) != 0) {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                       "NICKNAME ECHO MISMATCH");
            return;
        }
        s_app.profile_confirmed = local_profile_echoed();
        s_app.local_ready =
            (s_app.ready_mask & seat_bit(s_app.local_seat)) != 0U;
        show_lobby();
    } else if (snapshot->kind == WEREWOLF_SNAPSHOT_GAME) {
        const werewolf_game_snapshot_t *game = &snapshot->body.game;
        bool monotonic = !s_app.have_public ||
            ((int32_t)(game->public_state.phase_epoch -
                       s_app.public_state.phase_epoch) >= 0 &&
             (int32_t)(game->public_state.gate_epoch -
                       s_app.public_state.gate_epoch) >= 0);
        bool refresh = was_reconnecting || !s_app.have_public ||
            game->public_state.phase_epoch !=
                s_app.public_state.phase_epoch ||
            game->public_state.gate_epoch != s_app.public_state.gate_epoch ||
            game->public_state.gate_kind != s_app.public_state.gate_kind ||
            game->local_gate_acknowledged != s_app.gate_acknowledged;

        if (monotonic) {
            ++s_app.game_snapshot_revision;
            if (s_app.game_snapshot_revision == 0U) {
                s_app.game_snapshot_revision = 1U;
            }
            client_apply_public(&game->public_state, refresh, true,
                                game->local_gate_acknowledged);
            client_reconcile_inflight_action(&s_app.public_state);
        }
    } else if (snapshot->kind == WEREWOLF_SNAPSHOT_ROLE_REVEAL) {
        client_process_role_reveal(snapshot->body.roles);
    }
}

static void process_host_message(const werewolf_frame_t *frame,
                                 const werewolf_message_t *message,
                                 const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    if (frame->src > 0U && frame->src < WW_PLAYER_COUNT &&
        s_app.peers[frame->src].paired) {
        s_app.peers[frame->src].first_delivery_failure_ms = 0U;
        s_app.peers[frame->src].last_delivery_failure_ms = 0U;
    }
    if (s_app.state == APP_STATE_HOST_KICKING &&
        message->type == WEREWOLF_MSG_ABORT &&
        message->body.abort.reason == WEREWOLF_ABORT_USER_CANCELLED &&
        frame->src > 0U && frame->src < WW_PLAYER_COUNT) {
        bool target_departed = frame->src == s_app.host_kick.seat;

        (void)host_clear_lobby_peer(frame->src);
        if (target_departed) {
            ESP_LOGI(TAG, "kick target left before completion: seat=%u",
                     frame->src);
            finish_host_kick();
        }
        return;
    }
    if (s_app.state == APP_STATE_TERMINATING &&
        message->type == WEREWOLF_MSG_ABORT &&
        message->body.abort.reason == WEREWOLF_ABORT_USER_CANCELLED &&
        frame->src > 0U && frame->src < WW_PLAYER_COUNT) {
        werewolf_termination_remove_targets(&s_app.termination,
                                            seat_bit(frame->src));
        host_drop_transport_peer(frame->src);
        return;
    }
    if (message->type == WEREWOLF_MSG_JOIN) {
        host_process_join(frame, &message->body.join, mac);
    } else if (message->type == WEREWOLF_MSG_PAIR_CLIENT_REVEAL) {
        host_process_pair_client_reveal(
            frame, &message->body.pair_client_reveal, mac);
    } else if (message->type == WEREWOLF_MSG_PROFILE) {
        host_process_profile(frame->src, &message->body.profile);
    } else if (message->type == WEREWOLF_MSG_READY) {
        host_process_ready(frame->src, &message->body.ready);
    } else if (message->type == WEREWOLF_MSG_ACTION && s_app.game_started &&
               frame->phase_seq ==
                   (uint16_t)message->body.action.expected_phase_epoch) {
        if (message->body.action.kind == WEREWOLF_ACTION_LEAVE_GAME) {
            if (s_app.game.phase == WW_PHASE_GAME_OVER &&
                s_app.gate == APP_GATE_NONE) {
                host_retire_finished_peer(frame->src);
            } else {
                host_drop_transport_peer(frame->src);
                begin_host_fatal(WEREWOLF_ABORT_USER_CANCELLED,
                                 WEREWOLF_UI_ERROR_HOST_LOST,
                                 WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                                 "A PLAYER LEFT. GAME ABORTED.");
            }
        } else {
            host_process_action(frame->src, &message->body.action);
        }
    } else if (message->type == WEREWOLF_MSG_ABORT &&
               message->body.abort.reason == WEREWOLF_ABORT_USER_CANCELLED) {
        if (s_app.state == APP_STATE_HOST_LOBBY) {
            host_remove_lobby_peer(frame->src);
        } else if (s_app.game_started) {
            if (s_app.game.phase == WW_PHASE_GAME_OVER &&
                s_app.gate == APP_GATE_NONE) {
                host_retire_finished_peer(frame->src);
            } else {
                host_drop_transport_peer(frame->src);
                begin_host_fatal(WEREWOLF_ABORT_USER_CANCELLED,
                                 WEREWOLF_UI_ERROR_HOST_LOST,
                                 WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                                 "A PLAYER LEFT. GAME ABORTED.");
            }
        }
    }
}

static void process_client_message(const werewolf_frame_t *frame,
                                   const werewolf_message_t *message,
                                   const uint8_t mac[WEREWOLF_NET_MAC_SIZE])
{
    bool was_reconnecting = s_app.reconnecting;

    if (frame->src != 0U ||
        (message->type != WEREWOLF_MSG_BEACON &&
         (frame->session_id != s_app.session_id ||
          frame->epoch != s_app.protocol_epoch))) {
        return;
    }
    if (werewolf_protocol_type_requires_encryption(message->type)) {
        s_app.last_host_rx_ms = app_now_ms();
        if (s_app.reconnecting) {
            s_app.reconnecting = false;
        }
    }
    switch (message->type) {
    case WEREWOLF_MSG_PAIR_HOST_REVEAL:
        client_process_pair_host_reveal(
            &message->body.pair_host_reveal, mac);
        break;
    case WEREWOLF_MSG_ACCEPT:
        client_accept(&message->body.accept);
        break;
    case WEREWOLF_MSG_START:
        if ((int32_t)(message->body.start.roster.lobby_revision -
                      s_app.roster.lobby_revision) < 0 ||
            (message->body.start.roster.profile_mask &
             seat_bit(s_app.local_seat)) == 0U ||
            strcmp(message->body.start.roster.names[s_app.local_seat],
                   s_app.local_nickname) != 0) {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                       "INVALID START ROSTER");
            break;
        }
        s_app.roster = message->body.start.roster;
        s_app.profile_confirmed = true;
        client_apply_public(&message->body.start.public_state, false,
                            false, false);
        if (s_app.have_private) {
            show_role_page();
        } else {
            /* START and PRIVATE_ROLE are independently reliable unicasts and
             * may arrive in either order. Keep the sealed page noninteractive
             * until the private codec message is available. */
            s_app.ui.page = WEREWOLF_UI_PAGE_ROLE;
            s_app.ui.input_enabled = false;
            s_app.ui.connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
            publish_ui();
        }
        break;
    case WEREWOLF_MSG_PRIVATE_ROLE:
        client_process_private(&message->body.private_role);
        break;
    case WEREWOLF_MSG_PHASE:
        client_apply_public(&message->body.phase.public_state, true,
                            false, false);
        /* A phase advance is itself authoritative proof that this client's
         * required normal action was accepted.  Clear the in-flight guard
         * before the player can acknowledge the newly opened result gate. */
        client_reconcile_inflight_action(&s_app.public_state);
        break;
    case WEREWOLF_MSG_SNAPSHOT:
        client_process_snapshot(&message->body.snapshot, was_reconnecting);
        break;
    case WEREWOLF_MSG_ABORT:
        if (message->body.abort.reason ==
            WEREWOLF_ABORT_HOST_CLOSED_ROOM) {
            show_room_closed_notice();
        } else if (message->body.abort.reason ==
                   WEREWOLF_ABORT_KICKED_BY_HOST) {
            show_kicked_notice();
        } else {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_HOST_LOST,
                       WEREWOLF_UI_CONNECTION_HOST_LOST, false,
                       "HOST ENDED THE SESSION");
        }
        break;
    default:
        break;
    }
}

static void process_net_event(const app_event_t *event)
{
    const werewolf_frame_t *frame = &event->body.net.frame;
    werewolf_message_t message;

    if (werewolf_messages_decode(frame->type, frame->payload,
                                  frame->payload_len, &message) !=
        WEREWOLF_MESSAGES_OK) {
        if (!s_app.is_host && frame->src == 0U &&
            !werewolf_protocol_type_is_discovery(frame->type)) {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_PROTOCOL,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                       "INVALID HOST MESSAGE");
        }
        return;
    }
    if (message.type == WEREWOLF_MSG_BEACON && !s_app.is_host) {
        if (frame->src != 0U) {
            return;
        }
        if (s_app.state == APP_STATE_CLIENT_SCANNING) {
            uint32_t token = WEREWOLF_ROOM_TOKEN_NONE;
            werewolf_room_directory_result_t result =
                werewolf_room_directory_observe(
                    &s_app.room_directory, event->body.net.mac,
                    frame->session_id, frame->epoch,
                    &message.body.beacon, app_now_ms(),
                    s_app.ui.selected_room_token, &token);

            if (result == WEREWOLF_ROOM_DIRECTORY_ADDED ||
                result == WEREWOLF_ROOM_DIRECTORY_UPDATED ||
                result == WEREWOLF_ROOM_DIRECTORY_REPLACED ||
                result == WEREWOLF_ROOM_DIRECTORY_EVICTED) {
                show_room_list();
            }
        }
        /* During CLIENT_JOINING the seat, key pair and session are frozen.
         * A Beacon showing that seat occupied commonly means ACCEPT was lost;
         * the timer must resend the original JOIN, never create a ghost seat. */
        secure_zero(&message, sizeof(message));
        return;
    }
    if (s_app.is_host) {
        process_host_message(frame, &message, event->body.net.mac);
    } else {
        process_client_message(frame, &message, event->body.net.mac);
    }
    secure_zero(&message, sizeof(message));
}

static bool normal_action_requires_snapshot(werewolf_action_kind_t kind)
{
    return kind == WEREWOLF_ACTION_NIGHT_TARGET ||
           kind == WEREWOLF_ACTION_VOTE_TARGET ||
           kind == WEREWOLF_ACTION_PASS_SPEECH;
}

static void client_send_game_action(werewolf_action_kind_t kind,
                                    uint8_t target, app_action_tag_t tag)
{
    uint32_t epoch;

    if (!s_app.game_started || !s_app.have_public) {
        return;
    }
    epoch = s_app.public_state.phase_epoch;
    if (send_action(0U, s_app.local_seat, kind, target, epoch,
                    s_app.public_state.gate_kind,
                    s_app.public_state.gate_epoch, tag)) {
        if (normal_action_requires_snapshot(kind)) {
            s_app.inflight_action = (app_inflight_action_t){
                .active = true,
                .commits_guard_target =
                    kind == WEREWOLF_ACTION_NIGHT_TARGET &&
                    local_role() == WW_ROLE_GUARD &&
                    s_app.public_state.phase == WW_PHASE_NIGHT,
                .kind = kind,
                .target = target,
                .speaker_seat = s_app.public_state.current_speaker,
                .phase_epoch = epoch,
                .sent_ms = app_now_ms(),
                .snapshot_revision = s_app.game_snapshot_revision,
            };
        }
        s_app.ui.input_enabled = false;
        if (kind == WEREWOLF_ACTION_ROLE_SEEN) {
            s_app.role_confirmed = true;
            s_app.gate_acknowledged = true;
        } else if (kind == WEREWOLF_ACTION_ACK_RESULT &&
                   tag == APP_ACTION_TAG_PRIVATE_ACK) {
            s_app.private_result_confirmed = true;
            s_app.gate_acknowledged = true;
        } else if (kind == WEREWOLF_ACTION_ACK_RESULT) {
            s_app.gate_acknowledged = true;
        }
        publish_ui();
    } else {
        /* A local capacity/transient send failure has no retry-exhausted
         * callback.  Force snapshot reconciliation so the UI latch can reopen
         * against the Host-authoritative gate/submission state. */
        s_app.reconnecting = true;
        s_app.ui.connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
        s_app.ui.input_enabled = false;
        publish_ui();
    }
}

static bool local_game_action_allowed(werewolf_ui_action_type_t type)
{
    if (s_app.reconnecting || s_app.inflight_action.active) {
        return false;
    }
    if (type == WEREWOLF_UI_ACTION_ROLE_SEEN) {
        return s_app.gate == APP_GATE_ROLE &&
               !s_app.gate_acknowledged && s_app.have_private &&
               werewolf_messages_private_matches_gate(
                   &s_app.private_state, WEREWOLF_GATE_ROLE,
                   s_app.gate_epoch);
    }
    if (type == WEREWOLF_UI_ACTION_ACK_RESULT) {
        if (s_app.gate_acknowledged) {
            return false;
        }
        if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
            return s_app.private_result_ready && s_app.have_private &&
                   werewolf_messages_private_matches_gate(
                       &s_app.private_state,
                       WEREWOLF_GATE_PRIVATE_RESULT,
                       s_app.gate_epoch);
        }
        return s_app.gate == APP_GATE_DAWN ||
               s_app.gate == APP_GATE_EXILE;
    }
    return s_app.gate == APP_GATE_NONE;
}

static void process_local_game_action(const werewolf_ui_action_t *ui_action)
{
    if (ui_action == NULL) {
        return;
    }
    if (!local_game_action_allowed(ui_action->type)) {
        werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        return;
    }

    werewolf_action_message_t action = {
        .target = ui_action->seat,
        .expected_phase_epoch = s_app.is_host ? s_app.game.phase_epoch
                                              : s_app.public_state.phase_epoch,
        .expected_gate_kind = s_app.is_host
                                  ? (werewolf_gate_kind_t)s_app.gate
                                  : s_app.public_state.gate_kind,
        .expected_gate_epoch = s_app.is_host
                                   ? s_app.gate_epoch
                                   : s_app.public_state.gate_epoch,
    };

    switch (ui_action->type) {
    case WEREWOLF_UI_ACTION_ROLE_SEEN:
        action.kind = WEREWOLF_ACTION_ROLE_SEEN;
        action.target = WW_NO_PLAYER;
        if (s_app.is_host) {
            host_process_action(0U, &action);
        } else {
            client_send_game_action(action.kind, action.target,
                                    APP_ACTION_TAG_ROLE);
        }
        break;
    case WEREWOLF_UI_ACTION_SUBMIT_NIGHT_TARGET:
        action.kind = WEREWOLF_ACTION_NIGHT_TARGET;
        if (s_app.is_host) {
            host_process_action(0U, &action);
        } else {
            client_send_game_action(action.kind, action.target,
                                    APP_ACTION_TAG_NIGHT);
        }
        break;
    case WEREWOLF_UI_ACTION_PASS_SPEECH:
        action.kind = WEREWOLF_ACTION_PASS_SPEECH;
        action.target = WW_NO_PLAYER;
        if (s_app.is_host) {
            host_process_action(0U, &action);
        } else {
            client_send_game_action(action.kind, action.target,
                                    APP_ACTION_TAG_SPEECH);
        }
        break;
    case WEREWOLF_UI_ACTION_SUBMIT_VOTE:
        action.kind = WEREWOLF_ACTION_VOTE_TARGET;
        if (s_app.is_host) {
            host_process_action(0U, &action);
        } else {
            client_send_game_action(action.kind, action.target,
                                    APP_ACTION_TAG_VOTE);
        }
        break;
    case WEREWOLF_UI_ACTION_ACK_RESULT: {
        app_action_tag_t tag = APP_ACTION_TAG_DAWN_ACK;

        action.kind = WEREWOLF_ACTION_ACK_RESULT;
        action.target = WW_NO_PLAYER;
        if (s_app.ui.page == WEREWOLF_UI_PAGE_PRIVATE_RESULT) {
            tag = APP_ACTION_TAG_PRIVATE_ACK;
        } else if (s_app.ui.page == WEREWOLF_UI_PAGE_ELIMINATED) {
            tag = APP_ACTION_TAG_EXILE_ACK;
        }
        if (s_app.is_host) {
            host_process_action(0U, &action);
        } else {
            client_send_game_action(action.kind, action.target, tag);
        }
        break;
    }
    default:
        break;
    }
}

static void process_ui_action(const werewolf_ui_action_t *action)
{
    if (action == NULL) {
        return;
    }
    if (!werewolf_ui_action_matches_model(action, &s_app.ui)) {
        /* Routine same-gate heartbeats may overtake a private confirmation in
         * the controller queue. The private epoch keeps that confirmation
         * valid, while every other stale action remains fail-closed. */
        werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        return;
    }
    switch (action->type) {
    case WEREWOLF_UI_ACTION_CREATE_ROOM:
        if (s_app.state == APP_STATE_MODE) {
            create_room();
        }
        break;
    case WEREWOLF_UI_ACTION_JOIN_ROOM:
        if (s_app.state == APP_STATE_MODE) {
            begin_join_scan();
        }
        break;
    case WEREWOLF_UI_ACTION_SELECT_ROOM:
        if (s_app.state == APP_STATE_CLIENT_SCANNING) {
            werewolf_room_candidate_t candidate;

            if (werewolf_room_directory_find_fresh(
                    &s_app.room_directory, action->room_token,
                    app_now_ms(), &candidate)) {
                s_app.ui.selected_room_token = action->room_token;
                secure_zero(&candidate, sizeof(candidate));
            }
        }
        break;
    case WEREWOLF_UI_ACTION_JOIN_SELECTED_ROOM:
        if (s_app.state == APP_STATE_CLIENT_SCANNING) {
            werewolf_room_candidate_t candidate;

            if (werewolf_room_directory_find_fresh(
                    &s_app.room_directory, action->room_token,
                    app_now_ms(), &candidate)) {
                werewolf_frame_t frame = {
                    .src = 0U,
                    .session_id = candidate.session_id,
                    .epoch = candidate.epoch,
                };

                s_app.ui.selected_room_token = action->room_token;
                adopt_beacon(&frame, &candidate.beacon,
                             candidate.host_mac);
                secure_zero(&frame, sizeof(frame));
                secure_zero(&candidate, sizeof(candidate));
            } else {
                show_room_list();
            }
        }
        break;
    case WEREWOLF_UI_ACTION_TOGGLE_READY:
        if (action->seat != s_app.local_seat ||
            s_app.ui.page != WEREWOLF_UI_PAGE_LOBBY) {
            break;
        }
        if (s_app.state == APP_STATE_HOST_LOBBY) {
            s_app.local_ready = !s_app.local_ready;
            if (s_app.local_ready) {
                s_app.ready_mask |= seat_bit(0U);
            } else {
                s_app.ready_mask &= (uint8_t)~seat_bit(0U);
            }
            bump_lobby_revision();
            show_lobby();
        } else if (s_app.state == APP_STATE_CLIENT_LOBBY &&
                   s_app.profile_confirmed) {
            bool ready = !s_app.local_ready;
            if (!send_ready(ready)) {
                break;
            }
            s_app.local_ready = ready;
            if (ready) {
                s_app.ready_mask |= seat_bit(s_app.local_seat);
            } else {
                s_app.ready_mask &= (uint8_t)~seat_bit(s_app.local_seat);
            }
            show_lobby();
        }
        break;
    case WEREWOLF_UI_ACTION_OPEN_PLAYER_ACTION:
        if (s_app.state == APP_STATE_HOST_LOBBY && s_app.is_host &&
            s_app.ui.page == WEREWOLF_UI_PAGE_LOBBY &&
            action->seat > 0U && action->seat < WW_PLAYER_COUNT &&
            s_app.peers[action->seat].paired &&
            (s_app.game.joined_mask & seat_bit(action->seat)) != 0U) {
            s_app.ui.selected_seat = action->seat;
            s_app.ui.page = WEREWOLF_UI_PAGE_PLAYER_ACTION;
            show_lobby();
        }
        break;
    case WEREWOLF_UI_ACTION_CLOSE_PLAYER_ACTION:
        if (s_app.state == APP_STATE_HOST_LOBBY && s_app.is_host &&
            s_app.ui.page == WEREWOLF_UI_PAGE_PLAYER_ACTION &&
            action->seat == s_app.ui.selected_seat) {
            s_app.ui.kick_seat = WEREWOLF_UI_NO_SEAT;
            s_app.ui.page = WEREWOLF_UI_PAGE_LOBBY;
            show_lobby();
        }
        break;
    case WEREWOLF_UI_ACTION_REQUEST_KICK_PLAYER:
        if (s_app.state == APP_STATE_HOST_LOBBY && s_app.is_host &&
            s_app.ui.page == WEREWOLF_UI_PAGE_PLAYER_ACTION &&
            action->seat > 0U && action->seat < WW_PLAYER_COUNT &&
            action->seat == s_app.ui.selected_seat &&
            s_app.peers[action->seat].paired &&
            (s_app.game.joined_mask & seat_bit(action->seat)) != 0U) {
            begin_host_kick(action->seat);
        }
        break;
    case WEREWOLF_UI_ACTION_START_GAME:
        if (s_app.state == APP_STATE_HOST_LOBBY) {
            host_begin_game();
        }
        break;
    case WEREWOLF_UI_ACTION_ROLE_SEEN:
    case WEREWOLF_UI_ACTION_SUBMIT_NIGHT_TARGET:
    case WEREWOLF_UI_ACTION_PASS_SPEECH:
    case WEREWOLF_UI_ACTION_SUBMIT_VOTE:
    case WEREWOLF_UI_ACTION_ACK_RESULT:
        if (s_app.state == APP_STATE_GAME) {
            process_local_game_action(action);
        }
        break;
    case WEREWOLF_UI_ACTION_RETRY:
        if (s_app.state == APP_STATE_ERROR) {
            if (s_app.ui.mode == WEREWOLF_UI_MODE_CREATE) {
                create_room();
            } else {
                begin_join_scan();
            }
        }
        break;
    case WEREWOLF_UI_ACTION_LEAVE_GAME:
        if (completed_review_active()) {
            reset_to_mode(true);
        } else if (!s_app.is_host && s_app.net_active &&
                   (s_app.state == APP_STATE_CLIENT_LOBBY ||
                    s_app.state == APP_STATE_GAME ||
                    (s_app.state == APP_STATE_CLIENT_JOINING &&
                     s_app.pair_state == APP_PAIR_CLIENT_REVEALED))) {
            begin_client_leave();
        } else {
            reset_to_mode(false);
        }
        break;
    case WEREWOLF_UI_ACTION_REQUEST_CLOSE_ROOM:
        if (s_app.state == APP_STATE_HOST_LOBBY && s_app.is_host &&
            !s_app.ui.room_close_prompt) {
            s_app.ui.room_close_prompt = true;
            publish_ui();
        }
        break;
    case WEREWOLF_UI_ACTION_CANCEL_CLOSE_ROOM:
        if (s_app.state == APP_STATE_HOST_LOBBY && s_app.is_host &&
            s_app.ui.room_close_prompt) {
            s_app.ui.room_close_prompt = false;
            show_lobby();
        }
        break;
    case WEREWOLF_UI_ACTION_CONFIRM_CLOSE_ROOM:
        begin_host_room_close();
        break;
    case WEREWOLF_UI_ACTION_ACK_ROOM_CLOSED:
        if (s_app.state == APP_STATE_ROOM_CLOSED) {
            reset_to_mode(false);
        }
        break;
    default:
        break;
    }
}

static void client_enter_reconcile(void)
{
    s_app.reconnecting = true;
    s_app.gate_acknowledged = false;
    if (s_app.gate == APP_GATE_ROLE) {
        s_app.role_confirmed = false;
    } else if (s_app.gate == APP_GATE_PRIVATE_RESULT) {
        s_app.private_result_confirmed = false;
    }
    s_app.ui.connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
    s_app.ui.input_enabled = false;
    publish_ui();
}

static bool process_sticky_failure_flags(void)
{
    bool input_lost = __atomic_exchange_n(
        &s_input_event_lost, false, __ATOMIC_ACQ_REL);
    bool delivery_lost = __atomic_exchange_n(
        &s_delivery_event_lost, false, __ATOMIC_ACQ_REL);

    if (input_lost) {
        if (s_app.state == APP_STATE_TERMINATING ||
            s_app.state == APP_STATE_HOST_KICKING) {
            ESP_LOGW(TAG, "button input event lost while input is locked");
        } else if (s_app.is_host && s_app.net_active) {
            begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                             WEREWOLF_UI_ERROR_HARDWARE,
                             WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                             "BUTTON INPUT EVENT LOST");
        } else if (!s_app.is_host && s_app.net_active &&
                   (s_app.state == APP_STATE_CLIENT_LOBBY ||
                    s_app.state == APP_STATE_GAME ||
                    (s_app.state == APP_STATE_CLIENT_JOINING &&
                     s_app.pair_state == APP_PAIR_CLIENT_REVEALED))) {
            begin_termination(seat_bit(0U),
                              WEREWOLF_ABORT_INTERNAL_ERROR,
                              APP_TERMINATION_TO_ERROR,
                              WEREWOLF_UI_ERROR_HARDWARE,
                              WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                              "BUTTON INPUT EVENT LOST", false, true);
        } else {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_HARDWARE,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                       "BUTTON INPUT EVENT LOST");
        }
        return true;
    }
    if (!delivery_lost) {
        return false;
    }
    if (s_app.state == APP_STATE_TERMINATING) {
        /* The reliable worker already removed the exhausted pending entry,
         * but the queue overflow hid which peer failed.  Treat every target
         * as unknown so pending==0 cannot be mistaken for acknowledged. */
        werewolf_termination_mark_failed(
            &s_app.termination, s_app.termination.target_mask);
        ESP_LOGW(TAG,
                 "termination delivery event lost; result mask unknown");
        return false;
    }
    if (s_app.state == APP_STATE_HOST_KICKING) {
        s_app.host_kick.delivery_event_lost = true;
        ESP_LOGW(TAG, "ambiguous delivery failure during host kick");
        return false;
    }
    if (s_app.is_host &&
        (s_app.state == APP_STATE_HOST_LOBBY ||
         (s_app.state == APP_STATE_GAME && !completed_review_active()))) {
        begin_host_fatal(WEREWOLF_ABORT_INTERNAL_ERROR,
                         WEREWOLF_UI_ERROR_PROTOCOL,
                         WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                         "DELIVERY FAILURE EVENT LOST");
        return true;
    }
    if (!s_app.is_host &&
        (s_app.state == APP_STATE_CLIENT_LOBBY ||
         s_app.state == APP_STATE_GAME) &&
        !completed_review_active()) {
        client_enter_reconcile();
    }
    return false;
}

static void process_deferred_ui_work(void)
{
    bool model_applied = false;
    app_ui_status_snapshot_t status = { 0 };
    werewolf_ui_feedback_t feedback = WEREWOLF_UI_FEEDBACK_NONE;
    bool release_pending = werewolf_ui_deferred_release_pending(
        &s_deferred_private_release);
    bool rollback_pending = werewolf_ui_deferred_release_pending(
        &s_deferred_ui_rollback);
    bool model_pending = werewolf_ui_deferred_release_pending(
        &s_deferred_ui_model);

    if ((!release_pending && !rollback_pending && !model_pending) ||
        !bsp_lvgl_lock(20)) {
        return;
    }
    /* Apply the newest controller snapshot first.  A disconnect/page change
     * closes private input before a delayed release is interpreted; an
     * unchanged private gate instead gives that release the current revision. */
    if (werewolf_ui_deferred_release_claim(&s_deferred_ui_model, true)) {
        model_applied = apply_ui_snapshot_locked(&status);
        if (!model_applied) {
            werewolf_ui_deferred_release_request(&s_deferred_ui_model);
            werewolf_ui_hide_private();
            bsp_lvgl_unlock();
            return;
        }
    }
    if (werewolf_ui_deferred_release_claim(
            &s_deferred_private_release, true)) {
        /* RELEASE itself is the fail-closed seal.  Do not follow it with
         * hide_private(): a short RELEASE may have armed the later CLICK that
         * explicitly completes this private gate. */
        (void)werewolf_ui_handle_key(
            WEREWOLF_UI_KEY_OK, WEREWOLF_UI_KEY_EVENT_RELEASE, NULL);
        feedback = werewolf_ui_take_feedback();
    }
    if (werewolf_ui_deferred_release_claim(
            &s_deferred_ui_rollback, true)) {
        werewolf_ui_cancel_pending_action();
    }
    bsp_lvgl_unlock();
    if (model_applied) {
        announce_ui_status(&status);
    }
    play_ui_feedback(feedback);
}

static void process_delivery_failure(const app_event_t *event)
{
    uint8_t peer = event->body.delivery.peer_id;
    uint32_t current_generation = 0U;
    (void)event->body.delivery.msg_seq;

    if (event->body.delivery.session_id != s_app.session_id ||
        werewolf_net_get_peer_generation(peer, &current_generation) !=
            WEREWOLF_NET_OK ||
        current_generation != event->body.delivery.peer_generation) {
        return;
    }
    if (s_app.state == APP_STATE_TERMINATING &&
        peer < WW_PLAYER_COUNT &&
        (s_app.termination.target_mask & seat_bit(peer)) != 0U) {
        werewolf_termination_mark_failed(&s_app.termination,
                                         seat_bit(peer));
        ESP_LOGW(TAG,
                 "termination delivery exhausted: peer=%u seq=%" PRIu32,
                 peer, event->body.delivery.msg_seq);
        return;
    }
    if (s_app.state == APP_STATE_HOST_KICKING && s_app.is_host) {
        ESP_LOGW(TAG,
                 "host delivery exhausted during kick: peer=%u seq=%" PRIu32,
                 peer, event->body.delivery.msg_seq);
        if (peer != s_app.host_kick.seat) {
            (void)host_clear_lobby_peer(peer);
        }
        return;
    }
    if (s_app.is_host) {
        if (s_app.state == APP_STATE_HOST_LOBBY) {
            ESP_LOGW(TAG,
                     "host encrypted delivery exhausted: peer=%u seq=%" PRIu32,
                     peer, event->body.delivery.msg_seq);
            host_remove_lobby_peer(peer);
        } else if (s_app.game_started && peer > 0U &&
                   peer < WW_PLAYER_COUNT && s_app.peers[peer].paired) {
            if (s_app.game.phase == WW_PHASE_GAME_OVER &&
                s_app.gate == APP_GATE_NONE) {
                /* Retry exhaustion is not an explicit LEAVE.  Keep the LMK
                 * and peer so a recovered device can receive the next
                 * encrypted snapshot + ROLE_REVEAL heartbeat. */
                s_app.peers[peer].last_delivery_failure_ms = app_now_ms();
                s_app.last_snapshot_ms =
                    app_now_ms() - APP_GAME_OVER_HEARTBEAT_MS;
                return;
            }
            app_peer_t *failed_peer = &s_app.peers[peer];
            uint32_t now = app_now_ms();

            if (failed_peer->first_delivery_failure_ms == 0U ||
                elapsed_ms(now, failed_peer->last_delivery_failure_ms,
                           APP_HOST_LOST_MS)) {
                failed_peer->first_delivery_failure_ms = now;
            }
            failed_peer->last_delivery_failure_ms = now;
            if (elapsed_ms(now, failed_peer->first_delivery_failure_ms,
                           APP_HOST_LOST_MS)) {
                host_drop_transport_peer(peer);
                begin_host_fatal(WEREWOLF_ABORT_HOST_LOST,
                                 WEREWOLF_UI_ERROR_HOST_LOST,
                                 WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                                 "A PLAYER CONNECTION FAILED");
            } else {
                /* Retry-exhausted PRIVATE_ROLE frames are regenerated by the
                 * next encrypted heartbeat while this grace window remains.
                 * Final ROLE_REVEAL failures take the recoverable path above. */
                s_app.last_snapshot_ms = now - APP_HEARTBEAT_MS;
            }
        }
    } else if (peer == 0U &&
               (s_app.state == APP_STATE_CLIENT_LOBBY ||
                s_app.state == APP_STATE_GAME) &&
               !completed_review_active()) {
        client_enter_reconcile();
    }
}

static void process_delivery_fallback_events(void)
{
    app_event_t event;

    if (s_delivery_fallback_queue == NULL) {
        return;
    }
    while (xQueueReceive(s_delivery_fallback_queue, &event, 0U) == pdTRUE) {
        process_delivery_failure(&event);
        secure_zero(&event, sizeof(event));
    }
}

static void controller_tick(uint32_t now)
{
    uint32_t heartbeat_interval = completed_review_active()
                                      ? APP_GAME_OVER_HEARTBEAT_MS
                                      : APP_HEARTBEAT_MS;

    process_deferred_ui_work();
    process_delivery_fallback_events();
    if (process_sticky_failure_flags()) {
        return;
    }
    refresh_signal_ui(now);
    if (s_app.state == APP_STATE_TERMINATING) {
        werewolf_net_snapshot_t net_state = { 0 };
        uint8_t not_queued = werewolf_termination_missing_enqueue_mask(
            &s_app.termination);
        werewolf_termination_result_t result;

        if (not_queued != 0U) {
            werewolf_termination_mark_queued(
                &s_app.termination,
                queue_termination_for_mask(not_queued,
                                            s_app.termination_reason));
        }

        werewolf_net_snapshot(&net_state);
        result = werewolf_termination_poll(
            &s_app.termination, now, net_state.pending_count,
            net_state.tx_exhausted, APP_ROOM_CLOSE_TIMEOUT_MS);
        if (result == WEREWOLF_TERMINATION_ACKNOWLEDGED ||
            result == WEREWOLF_TERMINATION_DEADLINE_REACHED) {
            if (result == WEREWOLF_TERMINATION_DEADLINE_REACHED) {
                ESP_LOGW(TAG,
                         "termination deadline; target=0x%02x queued=0x%02x "
                         "failed=0x%02x pending=%u exhausted_delta=%" PRIu32,
                         s_app.termination.target_mask,
                         s_app.termination.queued_mask,
                         s_app.termination.failed_mask,
                         (unsigned)net_state.pending_count,
                         (uint32_t)(net_state.tx_exhausted -
                                    s_app.termination.tx_exhausted_base));
            }
            finish_termination();
        }
        return;
    }
    if (s_app.state == APP_STATE_HOST_KICKING) {
        poll_host_kick(now);
        return;
    }
    if (s_app.state == APP_STATE_HOST_LOBBY) {
        if ((s_app.pair_state == APP_PAIR_HOST_OFFER ||
             s_app.pair_state == APP_PAIR_HOST_LOCKED) &&
            elapsed_ms(now, s_app.pair_started_ms,
                       APP_PAIR_OFFER_TIMEOUT_MS)) {
            if (!host_prepare_next_offer()) {
                begin_host_fatal(WEREWOLF_ABORT_SECURITY_FAILURE,
                                 WEREWOLF_UI_ERROR_PROTOCOL,
                                 WEREWOLF_UI_CONNECTION_DISCONNECTED, false,
                                 "PAIR OFFER ROTATION FAILED");
                return;
            }
            send_beacon();
            s_app.last_beacon_ms = now;
        } else if (s_app.pair_state == APP_PAIR_HOST_OFFER &&
                   elapsed_ms(now, s_app.last_beacon_ms, APP_BEACON_MS)) {
            send_beacon();
            s_app.last_beacon_ms = now;
        } else if (s_app.pair_state == APP_PAIR_HOST_LOCKED &&
                   elapsed_ms(now, s_app.last_beacon_ms,
                              APP_JOIN_RETRY_MS)) {
            send_pair_host_reveal();
            s_app.last_beacon_ms = now;
        }
    }
    if (s_app.is_host && s_app.state == APP_STATE_GAME &&
        s_app.game.phase == WW_PHASE_GAME_OVER &&
        s_app.gate == APP_GATE_NONE && s_app.have_role_reveal &&
        !s_app.host_review_exit_unlocked) {
        werewolf_net_snapshot_t net_state;

        werewolf_net_snapshot(&net_state);
        if (net_state.pending_count == 0U) {
            s_app.host_review_exit_unlocked = true;
            show_game_over_page();
        }
    }
    if (s_app.is_host && s_app.net_active &&
        (s_app.state == APP_STATE_HOST_LOBBY ||
         s_app.state == APP_STATE_GAME) &&
        elapsed_ms(now, s_app.last_snapshot_ms, heartbeat_interval)) {
        send_heartbeat_snapshots();
        s_app.last_snapshot_ms = now;
    }
    if (s_app.state == APP_STATE_CLIENT_SCANNING &&
        werewolf_room_directory_expire(&s_app.room_directory, now) != 0U) {
        show_room_list();
    }
    if (s_app.state == APP_STATE_CLIENT_JOINING) {
        if (elapsed_ms(now, s_app.last_join_ms, APP_JOIN_RETRY_MS)) {
            if (s_app.pair_state == APP_PAIR_CLIENT_COMMITTED) {
                send_join();
            } else if (s_app.pair_state == APP_PAIR_CLIENT_REVEALED) {
                send_pair_client_reveal();
            }
            s_app.last_join_ms = now;
        }
        if (elapsed_ms(now, s_app.operation_started_ms, APP_JOIN_TIMEOUT_MS)) {
            werewolf_net_snapshot_t net_state;

            werewolf_net_snapshot(&net_state);
            ESP_LOGW(TAG,
                     "client join timeout: secure=%u pending=%u rx_sec=%" PRIu32
                     " rx_proto=%" PRIu32 " rx_queue=%" PRIu32
                     " tx_retry=%" PRIu32 " tx_exhaust=%" PRIu32
                     " tx_err=%" PRIu32,
                     (unsigned)net_state.secure_peer_count,
                     (unsigned)net_state.pending_count,
                     net_state.rx_security_drops,
                     net_state.rx_protocol_errors,
                     net_state.rx_queue_drops,
                     net_state.tx_retries,
                     net_state.tx_exhausted,
                     net_state.tx_transport_errors);
            clear_session();
            show_error(WEREWOLF_UI_ERROR_TIMEOUT,
                       WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                       "JOIN REQUEST TIMED OUT");
            return;
        }
    }
    if (s_app.state == APP_STATE_CLIENT_LOBBY &&
        !s_app.profile_confirmed &&
        elapsed_ms(now, s_app.profile_started_ms,
                   APP_PROFILE_TIMEOUT_MS)) {
        clear_session();
        show_error(WEREWOLF_UI_ERROR_TIMEOUT,
                   WEREWOLF_UI_CONNECTION_DISCONNECTED, true,
                   "NAME SYNC TIMED OUT");
        return;
    }
    if (s_app.state == APP_STATE_CLIENT_LOBBY &&
        !s_app.profile_confirmed &&
        elapsed_ms(now, s_app.last_profile_ms, APP_PROFILE_RETRY_MS)) {
        werewolf_net_snapshot_t net_state = { 0 };

        werewolf_net_snapshot(&net_state);
        if (net_state.pending_count == 0U) {
            (void)send_profile();
            s_app.last_profile_ms = now;
        }
    }
    if (!s_app.is_host &&
        (s_app.state == APP_STATE_CLIENT_LOBBY ||
         s_app.state == APP_STATE_GAME) &&
        s_app.last_host_rx_ms != 0U && !completed_review_active()) {
        if (!s_app.reconnecting &&
            elapsed_ms(now, s_app.last_host_rx_ms, APP_RECONNECT_MS)) {
            s_app.reconnecting = true;
            s_app.ui.connection = WEREWOLF_UI_CONNECTION_RECONNECTING;
            s_app.ui.input_enabled = false;
            publish_ui();
        }
        if (elapsed_ms(now, s_app.last_host_rx_ms, APP_HOST_LOST_MS)) {
            clear_session();
            show_error(WEREWOLF_UI_ERROR_HOST_LOST,
                       WEREWOLF_UI_CONNECTION_HOST_LOST, false,
                       "HOST HEARTBEAT LOST. NO HOST MIGRATION.");
        }
    }
}

static void controller_task(void *argument)
{
    app_event_t event;
    (void)argument;

    for (;;) {
        if (xQueueReceive(s_queue, &event,
                          pdMS_TO_TICKS(APP_TICK_MS)) == pdTRUE) {
            if (event.kind == APP_EVENT_UI) {
                process_ui_action(&event.body.ui);
            } else if (event.kind == APP_EVENT_NET) {
                process_net_event(&event);
            } else if (event.kind == APP_EVENT_DELIVERY_FAILED) {
                process_delivery_failure(&event);
            } else if (event.kind == APP_EVENT_INPUT_FAILURE) {
                __atomic_store_n(&s_input_event_lost, true,
                                 __ATOMIC_RELEASE);
            } else if (event.kind == APP_EVENT_BATTERY) {
                bool changed =
                    s_app.battery_available != event.body.battery.available ||
                    s_app.battery_stale != event.body.battery.stale ||
                    s_app.battery_percent != event.body.battery.percent;
                s_app.battery_available = event.body.battery.available;
                s_app.battery_stale = event.body.battery.stale;
                s_app.battery_percent = event.body.battery.percent;
                if (changed) {
                    publish_telemetry_ui();
                }
            } else if (event.kind == APP_EVENT_AUDIO) {
                s_app.audio_checked = event.body.audio.checked;
                s_app.audio_available = event.body.audio.available;
            }
            secure_zero(&event, sizeof(event));
        }
        controller_tick(app_now_ms());
        if (s_app.audio_checked && s_app.audio_available &&
            werewolf_sound_faulted()) {
            s_app.audio_available = false;
            ESP_LOGW(TAG,
                     "audio runtime fault; continuing without sound cues");
        }
    }
}

static bool map_button(bsp_btn_t button, bsp_btn_ev_t event,
                       werewolf_ui_key_t *key,
                       werewolf_ui_key_event_t *key_event)
{
    if (key == NULL || key_event == NULL) {
        return false;
    }
    if (button == BSP_BTN_UP) {
        *key = WEREWOLF_UI_KEY_UP;
    } else if (button == BSP_BTN_DOWN) {
        *key = WEREWOLF_UI_KEY_DOWN;
    } else if (button == BSP_BTN_OK) {
        *key = WEREWOLF_UI_KEY_OK;
    } else {
        return false;
    }
    if (event == BSP_BTN_PRESS) {
        *key_event = WEREWOLF_UI_KEY_EVENT_PRESS;
    } else if (event == BSP_BTN_RELEASE) {
        *key_event = WEREWOLF_UI_KEY_EVENT_RELEASE;
    } else if (event == BSP_BTN_CLICK) {
        *key_event = WEREWOLF_UI_KEY_EVENT_CLICK;
    } else if (event == BSP_BTN_LONG) {
        *key_event = WEREWOLF_UI_KEY_EVENT_LONG;
    } else {
        return false;
    }
    return true;
}

esp_err_t werewolf_app_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_app, 0, sizeof(s_app));
    if (esp_read_mac(s_app.local_mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(s_app.local_mac, 0, sizeof(s_app.local_mac));
    }
    (void)werewolf_identity_load(s_app.local_nickname, s_app.local_mac);
    ESP_LOGI(TAG, "local nickname: %s", s_app.local_nickname);
    s_sound_snapshot_valid = false;
    s_sound_snapshot_revision = 0U;
    s_sound_public_phase = WEREWOLF_UI_PUBLIC_PHASE_MODE;
    s_sound_connection = WEREWOLF_UI_CONNECTION_RADIO_OFF;
    s_sound_error = WEREWOLF_UI_ERROR_NONE;
    __atomic_store_n(&s_delivery_event_lost, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_input_event_lost, false, __ATOMIC_RELEASE);
    werewolf_ui_deferred_release_reset(&s_deferred_private_release);
    werewolf_ui_deferred_release_reset(&s_deferred_ui_rollback);
    werewolf_ui_deferred_release_reset(&s_deferred_ui_model);
    s_app.local_seat = WEREWOLF_UI_NO_SEAT;
    s_app.seer_result_seat = WW_NO_PLAYER;
    s_app.guard_previous_target = WW_NO_PLAYER;
    s_app.pairing = (ww_pairing_t)WW_PAIRING_CONTEXT_INIT;
    werewolf_ui_model_init(&s_app.ui);
    (void)snprintf(s_app.ui.local_name, sizeof(s_app.ui.local_name), "%s",
                   s_app.local_nickname);
    s_app.last_signal_refresh_ms = app_now_ms();
    s_app.ui.input_enabled = true;
    if (s_ui_snapshot_mutex == NULL) {
        s_ui_snapshot_mutex =
            xSemaphoreCreateMutexStatic(&s_ui_snapshot_mutex_buffer);
    }
    if (s_ui_snapshot_mutex == NULL || !stage_ui_snapshot(false)) {
        return ESP_ERR_NO_MEM;
    }
    if (s_queue == NULL) {
        s_queue = xQueueCreateStatic(APP_QUEUE_DEPTH, sizeof(app_event_t),
                                     s_queue_storage, &s_queue_buffer);
    } else {
        (void)xQueueReset(s_queue);
    }
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_delivery_fallback_queue == NULL) {
        s_delivery_fallback_queue = xQueueCreateStatic(
            APP_DELIVERY_FALLBACK_DEPTH, sizeof(app_event_t),
            s_delivery_fallback_queue_storage,
            &s_delivery_fallback_queue_buffer);
    } else {
        (void)xQueueReset(s_delivery_fallback_queue);
    }
    if (s_delivery_fallback_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!bsp_lvgl_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    bool ui_created = werewolf_ui_create(&s_app.ui);
    bsp_lvgl_unlock();
    if (!ui_created) {
        return ESP_FAIL;
    }
    s_task = xTaskCreateStatic(controller_task, "werewolf_app",
                               sizeof(s_task_stack),
                               NULL, APP_TASK_PRIORITY, s_task_stack,
                               &s_task_buffer);
    if (s_task == NULL) {
        if (bsp_lvgl_lock(1000)) {
            werewolf_ui_destroy();
            bsp_lvgl_unlock();
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void werewolf_app_handle_button(bsp_btn_t button, bsp_btn_ev_t event)
{
    werewolf_ui_key_t key;
    werewolf_ui_key_event_t key_event;
    werewolf_ui_action_t action = { 0 };
    bool have_action;
    bool action_queued = false;
    bool model_applied = false;
    app_ui_status_snapshot_t status = { 0 };
    werewolf_ui_feedback_t deferred_feedback =
        WEREWOLF_UI_FEEDBACK_NONE;
    werewolf_ui_feedback_t feedback = WEREWOLF_UI_FEEDBACK_NONE;
    app_event_t app_event = { .kind = APP_EVENT_UI };

    if (s_task == NULL || !map_button(button, event, &key, &key_event)) {
        return;
    }
    if (!bsp_lvgl_lock(20)) {
        if (key == WEREWOLF_UI_KEY_OK &&
            key_event == WEREWOLF_UI_KEY_EVENT_RELEASE) {
            werewolf_ui_deferred_release_request(
                &s_deferred_private_release);
        }
        return;
    }
    /* Apply the latest controller snapshot before interpreting input.  A
     * same-gate heartbeat preserves the private press/review state; a page,
     * epoch, link, or input-gate change clears it before LONG/CLICK can act. */
    if (werewolf_ui_deferred_release_claim(&s_deferred_ui_model, true)) {
        model_applied = apply_ui_snapshot_locked(&status);
        if (!model_applied) {
            werewolf_ui_deferred_release_request(&s_deferred_ui_model);
            werewolf_ui_hide_private();
            bsp_lvgl_unlock();
            return;
        }
    }
    /* A physical RELEASE that missed the LVGL lock belongs before every later
     * input event.  Draining it here both seals stale pixels before a new
     * PRESS and lets the matching delayed CLICK complete a valid short OK. */
    if (werewolf_ui_deferred_release_claim(
            &s_deferred_private_release, true)) {
        (void)werewolf_ui_handle_key(
            WEREWOLF_UI_KEY_OK, WEREWOLF_UI_KEY_EVENT_RELEASE, &action);
        deferred_feedback = werewolf_ui_take_feedback();
    }
    if (werewolf_ui_deferred_release_claim(
            &s_deferred_ui_rollback, true)) {
        werewolf_ui_cancel_pending_action();
    }
    have_action = werewolf_ui_handle_key(key, key_event, &action);
    feedback = werewolf_ui_take_feedback();
    bsp_lvgl_unlock();
    if (model_applied) {
        announce_ui_status(&status);
    }
    play_ui_feedback(deferred_feedback);
    if (have_action) {
        app_event.body.ui = action;
        if (xQueueSend(s_queue, &app_event, 0U) != pdTRUE) {
            /* The UI has already latched this gate/private/normal action.
             * A sticky rollback makes queue saturation recoverable without
             * requiring another physical key event. */
            werewolf_ui_deferred_release_request(&s_deferred_ui_rollback);
        } else {
            action_queued = true;
        }
    }
    if (!have_action || action_queued) {
        play_ui_feedback(feedback);
    }
}

void werewolf_app_report_input_failure(void)
{
    app_event_t event = { .kind = APP_EVENT_INPUT_FAILURE };

    if (s_queue == NULL || xQueueSend(s_queue, &event, 0U) != pdTRUE) {
        __atomic_store_n(&s_input_event_lost, true, __ATOMIC_RELEASE);
    }
}

void werewolf_app_report_battery(bool available, bool stale, uint8_t percent)
{
    app_event_t event = { .kind = APP_EVENT_BATTERY };

    if (s_queue == NULL) {
        return;
    }
    event.body.battery.available = available && percent <= 100U;
    event.body.battery.stale = stale;
    event.body.battery.percent = percent <= 100U ? percent : 0U;
    (void)xQueueSend(s_queue, &event, 0U);
}

void werewolf_app_report_audio(bool checked, bool available)
{
    app_event_t event = { .kind = APP_EVENT_AUDIO };

    if (s_queue == NULL) {
        return;
    }
    event.body.audio.checked = checked;
    event.body.audio.available = checked && available;
    (void)xQueueSend(s_queue, &event, 0U);
}
