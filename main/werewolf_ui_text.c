#include "werewolf_ui_text.h"

#include <stdio.h>

static bool text_written(char *output, size_t output_size, int written)
{
    if (output == NULL || output_size == 0U || written < 0 ||
        (size_t)written >= output_size) {
        if (output != NULL && output_size != 0U) {
            output[0] = '\0';
        }
        return false;
    }
    return true;
}

bool werewolf_ui_format_verify_code(char *output, size_t output_size,
                                    uint32_t code)
{
    int written;

    if (output == NULL || output_size < WEREWOLF_UI_VERIFY_TEXT_MAX ||
        code > WEREWOLF_UI_VERIFY_CODE_MAX) {
        if (output != NULL && output_size != 0U) {
            output[0] = '\0';
        }
        return false;
    }
    written = snprintf(output, output_size, "VERIFY %06u", (unsigned)code);
    return written == (int)(WEREWOLF_UI_VERIFY_TEXT_MAX - 1U);
}

bool werewolf_ui_format_phase(char *output, size_t output_size,
                              werewolf_ui_public_phase_t phase,
                              uint8_t round)
{
    const char *label = NULL;
    bool include_round = false;
    int written;

    switch (phase) {
    case WEREWOLF_UI_PUBLIC_PHASE_MODE:       label = "MODE SELECT"; break;
    case WEREWOLF_UI_PUBLIC_PHASE_LOBBY:      label = "LOBBY"; break;
    case WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK: label = "ROLE CHECK"; break;
    case WEREWOLF_UI_PUBLIC_PHASE_NIGHT:      label = "NIGHT"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_DAWN:       label = "DAWN"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_DISCUSSION: label = "SPEAK"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_VOTE:       label = "VOTE"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_DEFENCE:    label = "DEFEND"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_REVOTE:     label = "REVOTE"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_EXILE:      label = "EXILE"; include_round = true; break;
    case WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER:  label = "GAME OVER"; break;
    case WEREWOLF_UI_PUBLIC_PHASE_ERROR:      label = "ERROR"; break;
    default:
        if (output != NULL && output_size != 0U) {
            output[0] = '\0';
        }
        return false;
    }

    if (include_round) {
        written = round == 0U
                      ? snprintf(output, output_size, "%s --", label)
                      : snprintf(output, output_size, "%s %02u", label,
                                 (unsigned)round);
    } else {
        written = snprintf(output, output_size, "%s", label);
    }
    return text_written(output, output_size, written);
}

bool werewolf_ui_phase_reveals_alive_state(
    werewolf_ui_public_phase_t phase, bool game_started)
{
    if (!game_started) {
        return false;
    }
    switch (phase) {
    case WEREWOLF_UI_PUBLIC_PHASE_DAWN:
    case WEREWOLF_UI_PUBLIC_PHASE_DISCUSSION:
    case WEREWOLF_UI_PUBLIC_PHASE_VOTE:
    case WEREWOLF_UI_PUBLIC_PHASE_DEFENCE:
    case WEREWOLF_UI_PUBLIC_PHASE_REVOTE:
    case WEREWOLF_UI_PUBLIC_PHASE_EXILE:
    case WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER:
        return true;
    case WEREWOLF_UI_PUBLIC_PHASE_MODE:
    case WEREWOLF_UI_PUBLIC_PHASE_LOBBY:
    case WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK:
    case WEREWOLF_UI_PUBLIC_PHASE_NIGHT:
    case WEREWOLF_UI_PUBLIC_PHASE_ERROR:
    default:
        return false;
    }
}

uint8_t werewolf_ui_update_visible_alive_mask(
    uint8_t previous_visible_mask, uint8_t occupied_mask,
    uint8_t authoritative_alive_mask, bool game_started,
    werewolf_ui_public_phase_t phase)
{
    previous_visible_mask &= WEREWOLF_UI_ALL_PLAYERS_MASK;
    occupied_mask &= WEREWOLF_UI_ALL_PLAYERS_MASK;
    authoritative_alive_mask &= occupied_mask;

    if (!game_started || phase == WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK) {
        /* START can arrive after the last lobby snapshot seen by a guest.
         * ROLE_CHECK is still pre-action public state, so seed every occupied
         * seat as alive before later private phases begin freezing the mask. */
        return occupied_mask;
    }
    if (werewolf_ui_phase_reveals_alive_state(phase, true)) {
        return authoritative_alive_mask;
    }
    return previous_visible_mask & occupied_mask;
}

werewolf_ui_player_indicator_t werewolf_ui_player_indicator(
    bool occupied, bool ready, bool game_started, bool publicly_alive)
{
    if (!occupied) {
        return WEREWOLF_UI_PLAYER_INDICATOR_EMPTY;
    }
    if (game_started) {
        return publicly_alive ? WEREWOLF_UI_PLAYER_INDICATOR_ALIVE
                              : WEREWOLF_UI_PLAYER_INDICATOR_DEAD;
    }
    return ready ? WEREWOLF_UI_PLAYER_INDICATOR_READY
                 : WEREWOLF_UI_PLAYER_INDICATOR_JOINED;
}

unsigned werewolf_ui_battery_filled_segments(
    werewolf_ui_battery_state_t state, uint8_t soc)
{
    if (state == WEREWOLF_UI_BATTERY_UNAVAILABLE || soc > 100U) {
        return 0U;
    }
    if (state != WEREWOLF_UI_BATTERY_FRESH &&
        state != WEREWOLF_UI_BATTERY_STALE) {
        return 0U;
    }
    /* A valid 0% keeps one red segment so it cannot look unavailable. */
    return soc == 0U
               ? 1U
               : ((unsigned)soc * WEREWOLF_UI_BATTERY_SEGMENTS + 99U) / 100U;
}

werewolf_ui_signal_t werewolf_ui_signal_from_rssi(int8_t rssi_dbm)
{
    /* Sample availability travels separately from the signed dBm value. This
     * mapper therefore accepts the full int8 range; very strong RF readings
     * must not accidentally turn into NO_SAMPLE. */
    if (rssi_dbm >= -55) {
        return WEREWOLF_UI_SIGNAL_STRONG;
    }
    if (rssi_dbm >= -67) {
        return WEREWOLF_UI_SIGNAL_GOOD;
    }
    if (rssi_dbm >= -78) {
        return WEREWOLF_UI_SIGNAL_FAIR;
    }
    return WEREWOLF_UI_SIGNAL_WEAK;
}

unsigned werewolf_ui_signal_filled_segments(werewolf_ui_signal_t signal)
{
    switch (signal) {
    case WEREWOLF_UI_SIGNAL_WEAK:   return 1U;
    case WEREWOLF_UI_SIGNAL_FAIR:   return 2U;
    case WEREWOLF_UI_SIGNAL_GOOD:   return 3U;
    case WEREWOLF_UI_SIGNAL_STRONG: return 4U;
    case WEREWOLF_UI_SIGNAL_NO_SAMPLE:
    case WEREWOLF_UI_SIGNAL_STALE:
    case WEREWOLF_UI_SIGNAL_DISCONNECTED:
    default:
        return 0U;
    }
}

bool werewolf_ui_action_gate_open(bool input_enabled, bool action_latched)
{
    return input_enabled && !action_latched;
}

void werewolf_ui_deferred_release_reset(
    werewolf_ui_deferred_release_t *state)
{
    if (state != NULL) {
        __atomic_store_n(&state->pending, false, __ATOMIC_RELEASE);
    }
}

void werewolf_ui_deferred_release_request(
    werewolf_ui_deferred_release_t *state)
{
    if (state != NULL) {
        __atomic_store_n(&state->pending, true, __ATOMIC_RELEASE);
    }
}

bool werewolf_ui_deferred_release_pending(
    const werewolf_ui_deferred_release_t *state)
{
    return state != NULL &&
           __atomic_load_n(&state->pending, __ATOMIC_ACQUIRE);
}

bool werewolf_ui_deferred_release_claim(
    werewolf_ui_deferred_release_t *state, bool lvgl_lock_acquired)
{
    if (state == NULL || !lvgl_lock_acquired) {
        return false;
    }
    return __atomic_exchange_n(&state->pending, false, __ATOMIC_ACQ_REL);
}
