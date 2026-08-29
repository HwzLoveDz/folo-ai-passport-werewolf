#include "werewolf_sound.h"

#include <stdint.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SOUND_SAMPLE_RATE       16000U
#define SOUND_CHUNK_SAMPLES       128U
#define SOUND_AMPLITUDE           4800
#define SOUND_VOLUME                90U
#define SOUND_QUEUE_DEPTH            6U
#define SOUND_TASK_STACK_BYTES     3072U
#define SOUND_TASK_PRIORITY           3U

static const char *TAG = "werewolf_sound";

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint16_t gap_ms;
} tone_step_t;

typedef struct {
    const tone_step_t *steps;
    uint8_t count;
} tone_pattern_t;

/* Every pattern is intentionally short. Private cues are identical for every
 * role and public phase cues carry no role, target or completion information. */
static const tone_step_t s_startup[] = {
    {520U, 50U, 18U}, {760U, 70U, 0U},
};
static const tone_step_t s_move[] = {{680U, 26U, 0U}};
static const tone_step_t s_select[] = {
    {780U, 32U, 12U}, {1080U, 48U, 0U},
};
static const tone_step_t s_ready_on[] = {
    {880U, 38U, 12U}, {1100U, 52U, 0U},
};
static const tone_step_t s_ready_off[] = {
    {720U, 32U, 12U}, {480U, 48U, 0U},
};
static const tone_step_t s_connected[] = {
    {720U, 45U, 18U}, {1040U, 72U, 0U},
};
static const tone_step_t s_private_reveal[] = {{1320U, 42U, 0U}};
static const tone_step_t s_private_seal[] = {{820U, 40U, 0U}};
static const tone_step_t s_confirm_armed[] = {
    {920U, 32U, 10U}, {1120U, 45U, 0U},
};
static const tone_step_t s_confirmed[] = {
    {880U, 38U, 12U}, {1100U, 52U, 0U},
};
static const tone_step_t s_phase[] = {
    {540U, 35U, 15U}, {760U, 55U, 0U},
};
static const tone_step_t s_result[] = {
    {620U, 45U, 14U}, {850U, 60U, 0U},
};
static const tone_step_t s_disconnected[] = {
    {560U, 55U, 16U}, {330U, 80U, 0U},
};
static const tone_step_t s_error[] = {
    {190U, 60U, 22U}, {190U, 60U, 22U}, {190U, 80U, 0U},
};
static const tone_step_t s_game_over[] = {
    {620U, 45U, 15U}, {850U, 50U, 15U}, {1160U, 75U, 0U},
};

static const tone_pattern_t s_patterns[] = {
    [WEREWOLF_SOUND_STARTUP] = {s_startup, 2U},
    [WEREWOLF_SOUND_MOVE] = {s_move, 1U},
    [WEREWOLF_SOUND_SELECT] = {s_select, 2U},
    [WEREWOLF_SOUND_READY_ON] = {s_ready_on, 2U},
    [WEREWOLF_SOUND_READY_OFF] = {s_ready_off, 2U},
    [WEREWOLF_SOUND_CONNECTED] = {s_connected, 2U},
    [WEREWOLF_SOUND_PRIVATE_REVEAL] = {s_private_reveal, 1U},
    [WEREWOLF_SOUND_PRIVATE_SEAL] = {s_private_seal, 1U},
    [WEREWOLF_SOUND_CONFIRM_ARMED] = {s_confirm_armed, 2U},
    [WEREWOLF_SOUND_CONFIRMED] = {s_confirmed, 2U},
    [WEREWOLF_SOUND_PHASE] = {s_phase, 2U},
    [WEREWOLF_SOUND_RESULT] = {s_result, 2U},
    [WEREWOLF_SOUND_DISCONNECTED] = {s_disconnected, 2U},
    [WEREWOLF_SOUND_ERROR] = {s_error, 3U},
    [WEREWOLF_SOUND_GAME_OVER] = {s_game_over, 3U},
};

_Static_assert(sizeof(s_patterns) / sizeof(s_patterns[0]) ==
                   WEREWOLF_SOUND_COUNT,
               "every Werewolf sound cue needs a pattern");

static QueueHandle_t s_queue;
static StaticQueue_t s_queue_buffer;
static uint8_t s_queue_storage[SOUND_QUEUE_DEPTH *
                               sizeof(werewolf_sound_cue_t)];
static TaskHandle_t s_task;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[SOUND_TASK_STACK_BYTES /
                                sizeof(StackType_t)];
static bool s_busy;
static bool s_faulted;

static int16_t triangle_sample(uint32_t phase, int amplitude)
{
    uint32_t folded = phase < 32768U ? phase : 65535U - phase;
    int32_t sample =
        (int32_t)(folded * (uint32_t)(amplitude * 4) / 65535U) - amplitude;

    return (int16_t)sample;
}

static bool play_silence(uint16_t duration_ms)
{
    int16_t pcm[SOUND_CHUNK_SAMPLES] = {0};
    uint32_t remaining = (uint32_t)duration_ms * SOUND_SAMPLE_RATE / 1000U;

    while (remaining > 0U) {
        uint32_t count = remaining < SOUND_CHUNK_SAMPLES
                             ? remaining
                             : SOUND_CHUNK_SAMPLES;
        if (bsp_audio_write(pcm, count * sizeof(pcm[0])) != ESP_OK) {
            return false;
        }
        remaining -= count;
    }
    return true;
}

static bool play_tone(const tone_step_t *step)
{
    int16_t pcm[SOUND_CHUNK_SAMPLES];
    uint32_t phase = 0U;
    uint32_t total = (uint32_t)step->duration_ms * SOUND_SAMPLE_RATE / 1000U;
    uint32_t written = 0U;
    uint32_t phase_step =
        (uint32_t)step->frequency_hz * 65536U / SOUND_SAMPLE_RATE;

    while (written < total) {
        uint32_t count = total - written;
        if (count > SOUND_CHUNK_SAMPLES) {
            count = SOUND_CHUNK_SAMPLES;
        }
        for (uint32_t i = 0U; i < count; ++i) {
            uint32_t position = written + i;
            uint32_t edge = total / 6U;
            int amplitude = SOUND_AMPLITUDE;

            if (edge > 0U && position < edge) {
                amplitude = SOUND_AMPLITUDE * (int)position / (int)edge;
            } else if (edge > 0U && position + edge > total) {
                amplitude = SOUND_AMPLITUDE * (int)(total - position) /
                            (int)edge;
            }
            phase = (phase + phase_step) & 0xFFFFU;
            pcm[i] = triangle_sample(phase, amplitude);
        }
        if (bsp_audio_write(pcm, count * sizeof(pcm[0])) != ESP_OK) {
            return false;
        }
        written += count;
    }
    return step->gap_ms == 0U || play_silence(step->gap_ms);
}

static void sound_task(void *argument)
{
    werewolf_sound_cue_t cue;
    bool stack_reported = false;
    (void)argument;

    for (;;) {
        if (xQueueReceive(s_queue, &cue, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if ((unsigned)cue >= (unsigned)WEREWOLF_SOUND_COUNT) {
            continue;
        }
        __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
        const tone_pattern_t *pattern = &s_patterns[cue];
        for (uint8_t i = 0U; i < pattern->count; ++i) {
            if (!play_tone(&pattern->steps[i])) {
                __atomic_store_n(&s_faulted, true, __ATOMIC_RELEASE);
                break;
            }
        }
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
        if (!stack_reported) {
            ESP_LOGI(TAG, "task stack minimum free=%u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            stack_reported = true;
        }
    }
}

bool werewolf_sound_start(void)
{
    if (s_task != NULL) {
        return true;
    }
    if (bsp_audio_init() != ESP_OK ||
        bsp_audio_set_format(SOUND_SAMPLE_RATE, 16U, 1U) != ESP_OK) {
        return false;
    }
    bsp_audio_set_volume(SOUND_VOLUME);

    s_queue = xQueueCreateStatic(SOUND_QUEUE_DEPTH,
                                 sizeof(werewolf_sound_cue_t),
                                 s_queue_storage, &s_queue_buffer);
    if (s_queue == NULL) {
        return false;
    }
    __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_faulted, false, __ATOMIC_RELEASE);
    s_task = xTaskCreateStatic(sound_task, "werewolf_sound",
                               sizeof(s_task_stack), NULL,
                               SOUND_TASK_PRIORITY, s_task_stack,
                               &s_task_buffer);
    if (s_task == NULL) {
        s_queue = NULL;
        return false;
    }
    werewolf_sound_play(WEREWOLF_SOUND_STARTUP);
    return true;
}

bool werewolf_sound_faulted(void)
{
    return __atomic_load_n(&s_faulted, __ATOMIC_ACQUIRE);
}

void werewolf_sound_play(werewolf_sound_cue_t cue)
{
    if (s_queue == NULL ||
        (unsigned)cue >= (unsigned)WEREWOLF_SOUND_COUNT ||
        __atomic_load_n(&s_faulted, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (cue == WEREWOLF_SOUND_MOVE &&
        (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) ||
         uxQueueMessagesWaiting(s_queue) != 0U)) {
        return;
    }
    (void)xQueueSend(s_queue, &cue, 0U);
}
