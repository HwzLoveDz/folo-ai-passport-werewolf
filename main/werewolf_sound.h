#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cues describe only public state or a local UI gesture. Keep role, faction,
 * target and remote-player timing out of this interface. */
typedef enum {
    WEREWOLF_SOUND_STARTUP = 0,
    WEREWOLF_SOUND_MOVE,
    WEREWOLF_SOUND_SELECT,
    WEREWOLF_SOUND_READY_ON,
    WEREWOLF_SOUND_READY_OFF,
    WEREWOLF_SOUND_CONNECTED,
    WEREWOLF_SOUND_PRIVATE_REVEAL,
    WEREWOLF_SOUND_PRIVATE_SEAL,
    WEREWOLF_SOUND_CONFIRM_ARMED,
    WEREWOLF_SOUND_CONFIRMED,
    WEREWOLF_SOUND_PHASE,
    WEREWOLF_SOUND_RESULT,
    WEREWOLF_SOUND_DISCONNECTED,
    WEREWOLF_SOUND_ERROR,
    WEREWOLF_SOUND_GAME_OVER,
    WEREWOLF_SOUND_COUNT,
} werewolf_sound_cue_t;

/* Initializes the shared-I2C codec and a statically allocated playback task.
 * The function is idempotent after a successful start. */
bool werewolf_sound_start(void);

/* Sticky runtime health for logging and contextual recovery.  Audio faults do
 * not reserve a permanent slot in the product status bar. */
bool werewolf_sound_faulted(void);

/* Best-effort, zero-wait task API. It is not an ISR API. MOVE is deliberately
 * discarded while another cue is active or pending so navigation cannot build
 * an audible backlog. */
void werewolf_sound_play(werewolf_sound_cue_t cue);

#ifdef __cplusplus
}
#endif
