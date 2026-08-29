#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable room fingerprints are rendered as R plus six hex digits. */
#define WEREWOLF_UI_ROOM_CODE_MAX 12U
#define WEREWOLF_UI_VERIFY_CODE_MAX UINT32_C(999999)
#define WEREWOLF_UI_VERIFY_TEXT_MAX sizeof("VERIFY 000000")

/* Header text and graphical states are derived from public/session telemetry
 * only. Keeping these helpers outside LVGL makes privacy and formatting rules
 * host-testable. */
#define WEREWOLF_UI_BATTERY_SEGMENTS          5U
#define WEREWOLF_UI_BATTERY_LOW_PERCENT      20U
#define WEREWOLF_UI_BATTERY_CRITICAL_PERCENT 10U
#define WEREWOLF_UI_SIGNAL_SEGMENTS            4U
#define WEREWOLF_UI_PHASE_TEXT_MAX            16U
#define WEREWOLF_UI_ALL_PLAYERS_MASK          UINT8_C(0x7f)

typedef enum {
    WEREWOLF_UI_PUBLIC_PHASE_MODE = 0,
    WEREWOLF_UI_PUBLIC_PHASE_LOBBY,
    WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK,
    WEREWOLF_UI_PUBLIC_PHASE_NIGHT,
    WEREWOLF_UI_PUBLIC_PHASE_DAWN,
    WEREWOLF_UI_PUBLIC_PHASE_DISCUSSION,
    WEREWOLF_UI_PUBLIC_PHASE_VOTE,
    WEREWOLF_UI_PUBLIC_PHASE_DEFENCE,
    WEREWOLF_UI_PUBLIC_PHASE_REVOTE,
    WEREWOLF_UI_PUBLIC_PHASE_EXILE,
    WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER,
    WEREWOLF_UI_PUBLIC_PHASE_ERROR,
} werewolf_ui_public_phase_t;

typedef enum {
    WEREWOLF_UI_CONNECTION_RADIO_OFF = 0,
    WEREWOLF_UI_CONNECTION_SCANNING,
    WEREWOLF_UI_CONNECTION_PAIRING,
    WEREWOLF_UI_CONNECTION_ONLINE,
    WEREWOLF_UI_CONNECTION_RECONNECTING,
    WEREWOLF_UI_CONNECTION_DISCONNECTED,
    WEREWOLF_UI_CONNECTION_HOST_LOST,
} werewolf_ui_connection_t;

typedef enum {
    WEREWOLF_UI_BATTERY_UNAVAILABLE = 0,
    WEREWOLF_UI_BATTERY_FRESH,
    WEREWOLF_UI_BATTERY_STALE,
} werewolf_ui_battery_state_t;

typedef enum {
    WEREWOLF_UI_SIGNAL_NO_SAMPLE = 0,
    WEREWOLF_UI_SIGNAL_WEAK,
    WEREWOLF_UI_SIGNAL_FAIR,
    WEREWOLF_UI_SIGNAL_GOOD,
    WEREWOLF_UI_SIGNAL_STRONG,
    WEREWOLF_UI_SIGNAL_STALE,
    WEREWOLF_UI_SIGNAL_DISCONNECTED,
} werewolf_ui_signal_t;

typedef enum {
    WEREWOLF_UI_PLAYER_INDICATOR_EMPTY = 0,
    WEREWOLF_UI_PLAYER_INDICATOR_JOINED,
    WEREWOLF_UI_PLAYER_INDICATOR_READY,
    WEREWOLF_UI_PLAYER_INDICATOR_ALIVE,
    WEREWOLF_UI_PLAYER_INDICATOR_DEAD,
} werewolf_ui_player_indicator_t;

/* Lock-independent sticky work request used for model publication, UI latch
 * rollback, and an OK-release that could not acquire the LVGL mutex. */
typedef struct {
    bool pending;
} werewolf_ui_deferred_release_t;

bool werewolf_ui_format_verify_code(char *output, size_t output_size,
                                    uint32_t code);
bool werewolf_ui_format_phase(char *output, size_t output_size,
                              werewolf_ui_public_phase_t phase,
                              uint8_t round);
bool werewolf_ui_phase_reveals_alive_state(
    werewolf_ui_public_phase_t phase, bool game_started);
uint8_t werewolf_ui_update_visible_alive_mask(
    uint8_t previous_visible_mask, uint8_t occupied_mask,
    uint8_t authoritative_alive_mask, bool game_started,
    werewolf_ui_public_phase_t phase);
werewolf_ui_player_indicator_t werewolf_ui_player_indicator(
    bool occupied, bool ready, bool game_started, bool publicly_alive);
unsigned werewolf_ui_battery_filled_segments(
    werewolf_ui_battery_state_t state, uint8_t soc);
werewolf_ui_signal_t werewolf_ui_signal_from_rssi(int8_t rssi_dbm);
unsigned werewolf_ui_signal_filled_segments(werewolf_ui_signal_t signal);
bool werewolf_ui_action_gate_open(bool input_enabled, bool action_latched);
void werewolf_ui_deferred_release_reset(
    werewolf_ui_deferred_release_t *state);
void werewolf_ui_deferred_release_request(
    werewolf_ui_deferred_release_t *state);
bool werewolf_ui_deferred_release_pending(
    const werewolf_ui_deferred_release_t *state);
bool werewolf_ui_deferred_release_claim(
    werewolf_ui_deferred_release_t *state, bool lvgl_lock_acquired);

#ifdef __cplusplus
}
#endif
