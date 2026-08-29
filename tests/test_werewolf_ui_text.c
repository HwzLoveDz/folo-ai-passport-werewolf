#include <assert.h>
#include <string.h>

#include "werewolf_ui_text.h"

int main(void)
{
    char verify[WEREWOLF_UI_VERIFY_TEXT_MAX];
    char phase[WEREWOLF_UI_PHASE_TEXT_MAX];
    werewolf_ui_deferred_release_t private_release = { 0 };
    werewolf_ui_deferred_release_t private_rollback = { 0 };
    werewolf_ui_deferred_release_t normal_rollback = { 0 };
    static const werewolf_ui_public_phase_t hidden_alive_phases[] = {
        WEREWOLF_UI_PUBLIC_PHASE_MODE,
        WEREWOLF_UI_PUBLIC_PHASE_LOBBY,
        WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK,
        WEREWOLF_UI_PUBLIC_PHASE_NIGHT,
        WEREWOLF_UI_PUBLIC_PHASE_ERROR,
    };
    static const werewolf_ui_public_phase_t public_alive_phases[] = {
        WEREWOLF_UI_PUBLIC_PHASE_DAWN,
        WEREWOLF_UI_PUBLIC_PHASE_DISCUSSION,
        WEREWOLF_UI_PUBLIC_PHASE_VOTE,
        WEREWOLF_UI_PUBLIC_PHASE_DEFENCE,
        WEREWOLF_UI_PUBLIC_PHASE_REVOTE,
        WEREWOLF_UI_PUBLIC_PHASE_EXILE,
        WEREWOLF_UI_PUBLIC_PHASE_GAME_OVER,
    };

    assert(werewolf_ui_format_verify_code(verify, sizeof(verify), 0U));
    assert(strcmp(verify, "VERIFY 000000") == 0);
    assert(werewolf_ui_format_verify_code(verify, sizeof(verify), 42U));
    assert(strcmp(verify, "VERIFY 000042") == 0);
    assert(werewolf_ui_format_verify_code(
        verify, sizeof(verify), WEREWOLF_UI_VERIFY_CODE_MAX));
    assert(strcmp(verify, "VERIFY 999999") == 0);
    assert(!werewolf_ui_format_verify_code(
        verify, sizeof(verify), WEREWOLF_UI_VERIFY_CODE_MAX + 1U));
    assert(verify[0] == '\0');

    assert(werewolf_ui_format_phase(
        phase, sizeof(phase), WEREWOLF_UI_PUBLIC_PHASE_MODE, 0U));
    assert(strcmp(phase, "MODE SELECT") == 0);
    assert(werewolf_ui_format_phase(
        phase, sizeof(phase), WEREWOLF_UI_PUBLIC_PHASE_NIGHT, 2U));
    assert(strcmp(phase, "NIGHT 02") == 0);
    assert(werewolf_ui_format_phase(
        phase, sizeof(phase), WEREWOLF_UI_PUBLIC_PHASE_NIGHT, 0U));
    assert(strcmp(phase, "NIGHT --") == 0);
    assert(!werewolf_ui_format_phase(
        phase, sizeof(phase), (werewolf_ui_public_phase_t)99, 1U));
    assert(phase[0] == '\0');
    for (size_t i = 0U;
         i < sizeof(hidden_alive_phases) / sizeof(hidden_alive_phases[0]);
         ++i) {
        assert(!werewolf_ui_phase_reveals_alive_state(
            hidden_alive_phases[i], true));
    }
    for (size_t i = 0U;
         i < sizeof(public_alive_phases) / sizeof(public_alive_phases[0]);
         ++i) {
        assert(werewolf_ui_phase_reveals_alive_state(
            public_alive_phases[i], true));
        assert(!werewolf_ui_phase_reveals_alive_state(
            public_alive_phases[i], false));
    }

    /* Lobby occupancy seeds the public strip. Night resolution must retain
     * the last public state until DAWN, then later nights keep that death
     * visible while withholding any newly pending death. */
    uint8_t visible = werewolf_ui_update_visible_alive_mask(
        0U, UINT8_C(0x7f), UINT8_C(0x7f), false,
        WEREWOLF_UI_PUBLIC_PHASE_LOBBY);
    assert(visible == UINT8_C(0x7f));
    /* START/ROLE_CHECK reseeds from authoritative occupancy in case this
     * guest missed the final lobby snapshot before heartbeats stopped. */
    visible = werewolf_ui_update_visible_alive_mask(
        UINT8_C(0x07), UINT8_C(0x7f), UINT8_C(0x7f), true,
        WEREWOLF_UI_PUBLIC_PHASE_ROLE_CHECK);
    assert(visible == UINT8_C(0x7f));
    visible = werewolf_ui_update_visible_alive_mask(
        visible, UINT8_C(0x7f), UINT8_C(0x5f), true,
        WEREWOLF_UI_PUBLIC_PHASE_NIGHT);
    assert(visible == UINT8_C(0x7f));
    visible = werewolf_ui_update_visible_alive_mask(
        visible, UINT8_C(0x7f), UINT8_C(0x5f), true,
        WEREWOLF_UI_PUBLIC_PHASE_DAWN);
    assert(visible == UINT8_C(0x5f));
    visible = werewolf_ui_update_visible_alive_mask(
        visible, UINT8_C(0x7f), UINT8_C(0x1f), true,
        WEREWOLF_UI_PUBLIC_PHASE_NIGHT);
    assert(visible == UINT8_C(0x5f));
    assert(werewolf_ui_update_visible_alive_mask(
               UINT8_C(0xff), UINT8_C(0xff), UINT8_C(0xff), false,
               WEREWOLF_UI_PUBLIC_PHASE_LOBBY) == UINT8_C(0x7f));

    assert(werewolf_ui_player_indicator(
               false, false, false, true) ==
           WEREWOLF_UI_PLAYER_INDICATOR_EMPTY);
    assert(werewolf_ui_player_indicator(
               true, false, false, true) ==
           WEREWOLF_UI_PLAYER_INDICATOR_JOINED);
    assert(werewolf_ui_player_indicator(
               true, true, false, true) ==
           WEREWOLF_UI_PLAYER_INDICATOR_READY);
    assert(werewolf_ui_player_indicator(
               true, false, true, true) ==
           WEREWOLF_UI_PLAYER_INDICATOR_ALIVE);
    assert(werewolf_ui_player_indicator(
               true, true, true, false) ==
           WEREWOLF_UI_PLAYER_INDICATOR_DEAD);
    assert(werewolf_ui_player_indicator(
               false, true, true, false) ==
           WEREWOLF_UI_PLAYER_INDICATOR_EMPTY);
    assert(werewolf_ui_player_indicator(
               true, true, false, false) ==
           WEREWOLF_UI_PLAYER_INDICATOR_READY);

    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_UNAVAILABLE, 0U) == 0U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_FRESH, 0U) == 1U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_FRESH, 1U) == 1U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_FRESH, 20U) == 1U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_FRESH, 21U) == 2U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_FRESH, 100U) == 5U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_STALE, 76U) == 4U);
    assert(werewolf_ui_battery_filled_segments(
               WEREWOLF_UI_BATTERY_FRESH, 101U) == 0U);

    assert(werewolf_ui_signal_from_rssi(INT8_MIN) ==
           WEREWOLF_UI_SIGNAL_WEAK);
    assert(werewolf_ui_signal_from_rssi(0) ==
           WEREWOLF_UI_SIGNAL_STRONG);
    assert(werewolf_ui_signal_from_rssi(-79) ==
           WEREWOLF_UI_SIGNAL_WEAK);
    assert(werewolf_ui_signal_from_rssi(-78) ==
           WEREWOLF_UI_SIGNAL_FAIR);
    assert(werewolf_ui_signal_from_rssi(-68) ==
           WEREWOLF_UI_SIGNAL_FAIR);
    assert(werewolf_ui_signal_from_rssi(-67) ==
           WEREWOLF_UI_SIGNAL_GOOD);
    assert(werewolf_ui_signal_from_rssi(-56) ==
           WEREWOLF_UI_SIGNAL_GOOD);
    assert(werewolf_ui_signal_from_rssi(-55) ==
           WEREWOLF_UI_SIGNAL_STRONG);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_NO_SAMPLE) == 0U);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_WEAK) == 1U);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_FAIR) == 2U);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_GOOD) == 3U);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_STRONG) == 4U);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_STALE) == 0U);
    assert(werewolf_ui_signal_filled_segments(
               WEREWOLF_UI_SIGNAL_DISCONNECTED) == 0U);

    assert(!werewolf_ui_action_gate_open(false, false));
    assert(!werewolf_ui_action_gate_open(true, true));
    assert(werewolf_ui_action_gate_open(true, false));

    /* First lock attempt fails and no further key event occurs.  The next
     * successful tick claims the release exactly once. */
    werewolf_ui_deferred_release_request(&private_release);
    assert(werewolf_ui_deferred_release_pending(&private_release));
    assert(!werewolf_ui_deferred_release_claim(&private_release, false));
    assert(werewolf_ui_deferred_release_pending(&private_release));
    assert(werewolf_ui_deferred_release_claim(&private_release, true));
    assert(!werewolf_ui_deferred_release_pending(&private_release));
    assert(!werewolf_ui_deferred_release_claim(&private_release, true));

    /* Queue-full rollback is sticky for private/gate and normal actions. */
    werewolf_ui_deferred_release_request(&private_rollback);
    assert(!werewolf_ui_deferred_release_claim(&private_rollback, false));
    assert(werewolf_ui_deferred_release_claim(&private_rollback, true));
    assert(!werewolf_ui_deferred_release_claim(&private_rollback, true));

    werewolf_ui_deferred_release_request(&normal_rollback);
    assert(!werewolf_ui_deferred_release_claim(&normal_rollback, false));
    assert(werewolf_ui_deferred_release_claim(&normal_rollback, true));
    assert(!werewolf_ui_deferred_release_claim(&normal_rollback, true));
    werewolf_ui_deferred_release_request(&normal_rollback);
    werewolf_ui_deferred_release_reset(&normal_rollback);
    assert(!werewolf_ui_deferred_release_pending(&normal_rollback));
    return 0;
}
