#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEREWOLF_TERMINATION_PLAYER_MASK UINT8_C(0x7f)

typedef struct {
    uint8_t target_mask;
    uint8_t queued_mask;
    uint8_t failed_mask;
    uint32_t started_ms;
    uint32_t tx_exhausted_base;
} werewolf_termination_t;

typedef enum {
    WEREWOLF_TERMINATION_INVALID = 0,
    WEREWOLF_TERMINATION_WAITING,
    WEREWOLF_TERMINATION_ACKNOWLEDGED,
    WEREWOLF_TERMINATION_DEADLINE_REACHED,
} werewolf_termination_result_t;

void werewolf_termination_begin(werewolf_termination_t *termination,
                                uint8_t target_mask,
                                uint32_t started_ms,
                                uint32_t tx_exhausted_base);

uint8_t werewolf_termination_missing_enqueue_mask(
    const werewolf_termination_t *termination);

void werewolf_termination_mark_queued(werewolf_termination_t *termination,
                                      uint8_t queued_mask);

void werewolf_termination_mark_failed(werewolf_termination_t *termination,
                                      uint8_t failed_mask);

void werewolf_termination_remove_targets(werewolf_termination_t *termination,
                                         uint8_t removed_mask);

/* now_ms and the transport exhaustion counter are compared with unsigned
 * subtraction, so ordinary uint32_t wraparound is supported. timeout_ms must
 * be shorter than half of the uint32_t time domain. */
werewolf_termination_result_t werewolf_termination_poll(
    const werewolf_termination_t *termination,
    uint32_t now_ms,
    size_t pending_count,
    uint32_t tx_exhausted,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
