#include "werewolf_termination.h"

#include <stdbool.h>
#include <string.h>

static uint8_t valid_targets(uint8_t mask)
{
    return (uint8_t)(mask & WEREWOLF_TERMINATION_PLAYER_MASK);
}

static bool deadline_reached(uint32_t now_ms, uint32_t started_ms,
                             uint32_t timeout_ms)
{
    return (uint32_t)(now_ms - started_ms) >= timeout_ms;
}

void werewolf_termination_begin(werewolf_termination_t *termination,
                                uint8_t target_mask,
                                uint32_t started_ms,
                                uint32_t tx_exhausted_base)
{
    if (termination == NULL) {
        return;
    }
    memset(termination, 0, sizeof(*termination));
    termination->target_mask = valid_targets(target_mask);
    termination->started_ms = started_ms;
    termination->tx_exhausted_base = tx_exhausted_base;
}

uint8_t werewolf_termination_missing_enqueue_mask(
    const werewolf_termination_t *termination)
{
    if (termination == NULL) {
        return 0U;
    }
    return (uint8_t)(termination->target_mask &
                     (uint8_t)~termination->queued_mask);
}

void werewolf_termination_mark_queued(werewolf_termination_t *termination,
                                      uint8_t queued_mask)
{
    if (termination == NULL) {
        return;
    }
    termination->queued_mask |=
        (uint8_t)(valid_targets(queued_mask) & termination->target_mask);
}

void werewolf_termination_mark_failed(werewolf_termination_t *termination,
                                      uint8_t failed_mask)
{
    if (termination == NULL) {
        return;
    }
    termination->failed_mask |=
        (uint8_t)(valid_targets(failed_mask) & termination->target_mask);
}

void werewolf_termination_remove_targets(werewolf_termination_t *termination,
                                         uint8_t removed_mask)
{
    uint8_t retained_mask;

    if (termination == NULL) {
        return;
    }
    retained_mask = (uint8_t)~valid_targets(removed_mask);
    termination->target_mask &= retained_mask;
    termination->queued_mask &= retained_mask;
    termination->failed_mask &= retained_mask;
}

werewolf_termination_result_t werewolf_termination_poll(
    const werewolf_termination_t *termination,
    uint32_t now_ms,
    size_t pending_count,
    uint32_t tx_exhausted,
    uint32_t timeout_ms)
{
    uint32_t exhausted_delta;

    if (termination == NULL) {
        return WEREWOLF_TERMINATION_INVALID;
    }
    if (termination->target_mask == 0U) {
        return WEREWOLF_TERMINATION_ACKNOWLEDGED;
    }

    exhausted_delta =
        (uint32_t)(tx_exhausted - termination->tx_exhausted_base);
    if (termination->queued_mask == termination->target_mask &&
        termination->failed_mask == 0U && pending_count == 0U &&
        exhausted_delta == 0U) {
        return WEREWOLF_TERMINATION_ACKNOWLEDGED;
    }
    if (deadline_reached(now_ms, termination->started_ms, timeout_ms)) {
        return WEREWOLF_TERMINATION_DEADLINE_REACHED;
    }
    return WEREWOLF_TERMINATION_WAITING;
}
