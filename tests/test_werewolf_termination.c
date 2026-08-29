#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "werewolf_termination.h"

static void test_begin_and_missing_enqueue_mask(void)
{
    werewolf_termination_t termination;
    const uint8_t targets = UINT8_C(0x45);

    werewolf_termination_begin(&termination, UINT8_C(0xc5), 123U, 77U);
    assert(termination.target_mask == targets);
    assert(termination.queued_mask == 0U);
    assert(termination.failed_mask == 0U);
    assert(termination.started_ms == 123U);
    assert(termination.tx_exhausted_base == 77U);
    assert(werewolf_termination_missing_enqueue_mask(&termination) == targets);

    werewolf_termination_mark_queued(&termination, UINT8_C(0xc1));
    assert(termination.queued_mask == UINT8_C(0x41));
    assert(werewolf_termination_missing_enqueue_mask(&termination) ==
           UINT8_C(0x04));
    werewolf_termination_mark_queued(&termination, UINT8_C(0x04));
    assert(werewolf_termination_missing_enqueue_mask(&termination) == 0U);
}

static void test_acknowledged_only_after_all_queued_and_pending_drained(void)
{
    werewolf_termination_t termination;

    werewolf_termination_begin(&termination, UINT8_C(0x06), 1000U, 9U);
    werewolf_termination_mark_queued(&termination, UINT8_C(0x02));
    assert(werewolf_termination_poll(&termination, 1100U, 0U, 9U, 9000U) ==
           WEREWOLF_TERMINATION_WAITING);

    werewolf_termination_mark_queued(&termination, UINT8_C(0x04));
    assert(werewolf_termination_poll(&termination, 1100U, 1U, 9U, 9000U) ==
           WEREWOLF_TERMINATION_WAITING);
    assert(werewolf_termination_poll(&termination, 1100U, 0U, 9U, 9000U) ==
           WEREWOLF_TERMINATION_ACKNOWLEDGED);
}

static void test_exhaustion_and_failure_wait_for_deadline(void)
{
    werewolf_termination_t termination;

    werewolf_termination_begin(&termination, UINT8_C(0x02), 500U, 41U);
    werewolf_termination_mark_queued(&termination, UINT8_C(0x02));
    assert(werewolf_termination_poll(&termination, 8960U, 0U, 42U, 9000U) ==
           WEREWOLF_TERMINATION_WAITING);
    assert(werewolf_termination_poll(&termination, 9500U, 0U, 42U, 9000U) ==
           WEREWOLF_TERMINATION_DEADLINE_REACHED);

    werewolf_termination_begin(&termination, UINT8_C(0x02), 500U, 42U);
    werewolf_termination_mark_queued(&termination, UINT8_C(0x02));
    werewolf_termination_mark_failed(&termination, UINT8_C(0x02));
    assert(werewolf_termination_poll(&termination, 8960U, 0U, 42U, 9000U) ==
           WEREWOLF_TERMINATION_WAITING);
    assert(werewolf_termination_poll(&termination, 9500U, 0U, 42U, 9000U) ==
           WEREWOLF_TERMINATION_DEADLINE_REACHED);
}

static void test_counter_and_time_wraparound(void)
{
    werewolf_termination_t termination;

    werewolf_termination_begin(&termination, UINT8_C(0x01),
                               UINT32_C(0xffffff9c), UINT32_MAX);
    werewolf_termination_mark_queued(&termination, UINT8_C(0x01));
    assert(werewolf_termination_poll(&termination, 49U, 0U, 0U, 150U) ==
           WEREWOLF_TERMINATION_WAITING);
    assert(werewolf_termination_poll(&termination, 50U, 0U, 0U, 150U) ==
           WEREWOLF_TERMINATION_DEADLINE_REACHED);
}

static void test_zero_targets_and_removal(void)
{
    werewolf_termination_t termination;

    werewolf_termination_begin(&termination, 0U, 100U, 20U);
    assert(werewolf_termination_poll(&termination, 100U, 7U, 30U, 9000U) ==
           WEREWOLF_TERMINATION_ACKNOWLEDGED);

    werewolf_termination_begin(&termination, UINT8_C(0x0e), 100U, 20U);
    werewolf_termination_mark_queued(&termination, UINT8_C(0x0e));
    werewolf_termination_mark_failed(&termination, UINT8_C(0x04));
    werewolf_termination_remove_targets(&termination, UINT8_C(0x84));
    assert(termination.target_mask == UINT8_C(0x0a));
    assert(termination.queued_mask == UINT8_C(0x0a));
    assert(termination.failed_mask == 0U);
    assert(werewolf_termination_poll(&termination, 101U, 0U, 20U, 9000U) ==
           WEREWOLF_TERMINATION_ACKNOWLEDGED);
}

static void test_missing_enqueue_reaches_deadline(void)
{
    werewolf_termination_t termination;

    werewolf_termination_begin(&termination, UINT8_C(0x03), 100U, 5U);
    werewolf_termination_mark_queued(&termination, UINT8_C(0x01));
    assert(werewolf_termination_poll(&termination, 9099U, 0U, 5U, 9000U) ==
           WEREWOLF_TERMINATION_WAITING);
    assert(werewolf_termination_poll(&termination, 9100U, 0U, 5U, 9000U) ==
           WEREWOLF_TERMINATION_DEADLINE_REACHED);
}

int main(void)
{
    test_begin_and_missing_enqueue_mask();
    test_acknowledged_only_after_all_queued_and_pending_drained();
    test_exhaustion_and_failure_wait_for_deadline();
    test_counter_and_time_wraparound();
    test_zero_targets_and_removal();
    test_missing_enqueue_reaches_deadline();
    assert(werewolf_termination_poll(NULL, 0U, 0U, 0U, 0U) ==
           WEREWOLF_TERMINATION_INVALID);
    return 0;
}
