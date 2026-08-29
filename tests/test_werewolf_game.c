#include "werewolf_game.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                    \
                    __FILE__, __LINE__, #condition);                           \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_STATUS(expected, expression)                                    \
    do {                                                                       \
        ww_status_t actual_status = (expression);                              \
        if (actual_status != (expected)) {                                     \
            fprintf(stderr,                                                    \
                    "%s:%d: expected %s, got %s from %s\n",                 \
                    __FILE__, __LINE__, ww_status_string(expected),            \
                    ww_status_string(actual_status), #expression);             \
            return false;                                                      \
        }                                                                      \
    } while (0)

typedef struct {
    uint8_t wolves[2];
    uint8_t wolf_count;
    uint8_t seer;
    uint8_t guard;
    uint8_t villagers[3];
    uint8_t villager_count;
} role_ids_t;

static uint8_t another_living_player(const ww_game_t *game, uint8_t player);

static ww_player_mask_t bit_for(uint8_t player)
{
    return (ww_player_mask_t)(UINT8_C(1) << player);
}

static uint8_t count_mask(ww_player_mask_t mask)
{
    uint8_t count = 0;

    while (mask != 0u) {
        count = (uint8_t)(count + (mask & UINT8_C(1)));
        mask = (ww_player_mask_t)(mask >> 1);
    }
    return count;
}

static bool setup_started_game(ww_game_t *game, uint64_t seed)
{
    uint8_t player;

    CHECK_STATUS(WW_OK, ww_game_init(game));
    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        CHECK_STATUS(WW_OK, ww_game_join(game, player));
    }
    CHECK_STATUS(WW_OK, ww_game_start(game, seed));
    return true;
}

static bool find_role_ids(const ww_game_t *game, role_ids_t *ids)
{
    uint8_t player;

    memset(ids, 0, sizeof(*ids));
    ids->seer = WW_NO_PLAYER;
    ids->guard = WW_NO_PLAYER;
    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        ww_private_view_t view;

        CHECK_STATUS(WW_OK, ww_game_get_private_view(game, player, &view));
        switch (view.role) {
        case WW_ROLE_WOLF:
            CHECK(ids->wolf_count < 2u);
            ids->wolves[ids->wolf_count++] = player;
            break;
        case WW_ROLE_SEER:
            CHECK(ids->seer == WW_NO_PLAYER);
            ids->seer = player;
            break;
        case WW_ROLE_GUARD:
            CHECK(ids->guard == WW_NO_PLAYER);
            ids->guard = player;
            break;
        case WW_ROLE_VILLAGER:
            CHECK(ids->villager_count < 3u);
            ids->villagers[ids->villager_count++] = player;
            break;
        case WW_ROLE_NONE:
        default:
            CHECK(false);
        }
    }
    CHECK(ids->wolf_count == 2u);
    CHECK(ids->seer != WW_NO_PLAYER);
    CHECK(ids->guard != WW_NO_PLAYER);
    CHECK(ids->villager_count == 3u);
    return true;
}

static bool submit_initial_night(ww_game_t *game, const role_ids_t *ids,
                                 uint8_t wolf_zero_target,
                                 uint8_t wolf_one_target,
                                 uint8_t guard_target,
                                 uint8_t seer_target)
{
    uint32_t epoch = game->phase_epoch;
    uint8_t player;

    CHECK(game->phase == WW_PHASE_NIGHT);
    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        ww_private_view_t view;
        uint8_t target;

        if ((game->alive_mask & bit_for(player)) == 0u) {
            continue;
        }
        CHECK_STATUS(WW_OK, ww_game_get_private_view(game, player, &view));
        switch (view.role) {
        case WW_ROLE_WOLF:
            target = player == ids->wolves[0] ? wolf_zero_target
                                               : wolf_one_target;
            break;
        case WW_ROLE_SEER:
            target = seer_target;
            break;
        case WW_ROLE_GUARD:
            target = guard_target;
            break;
        case WW_ROLE_VILLAGER:
            target = player;
            break;
        case WW_ROLE_NONE:
        default:
            CHECK(false);
            target = WW_NO_PLAYER;
            break;
        }
        CHECK_STATUS(WW_OK, ww_game_submit_night_action(game, epoch,
                                                         player, target));
    }
    return true;
}

static bool submit_wolf_revote(ww_game_t *game, const role_ids_t *ids,
                               uint8_t wolf_zero_target,
                               uint8_t wolf_one_target)
{
    uint32_t epoch = game->phase_epoch;
    uint8_t player;

    CHECK(game->phase == WW_PHASE_WOLF_REVOTE);
    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        uint8_t target;

        if ((game->alive_mask & bit_for(player)) == 0u) {
            continue;
        }
        if (player == ids->wolves[0]) {
            target = wolf_zero_target;
        } else if (player == ids->wolves[1]) {
            target = wolf_one_target;
        } else {
            target = player;
        }
        CHECK_STATUS(WW_OK, ww_game_submit_night_action(game, epoch,
                                                         player, target));
    }
    return true;
}

static bool begin_vote_after_dawn(ww_game_t *game)
{
    uint8_t first_speaker;
    uint8_t wrong_speaker;

    CHECK(game->phase == WW_PHASE_DAWN_RESULT);
    CHECK_STATUS(WW_OK,
                 ww_game_begin_discussion(game, game->phase_epoch));
    CHECK(game->phase == WW_PHASE_DISCUSSION);
    CHECK(game->discussion_remaining_mask == game->alive_mask);
    first_speaker = game->current_speaker;
    CHECK(first_speaker != WW_NO_PLAYER);
    CHECK_STATUS(WW_ERR_NOT_READY,
                 ww_game_begin_vote(game, game->phase_epoch));
    wrong_speaker = another_living_player(game, first_speaker);
    CHECK(wrong_speaker != WW_NO_PLAYER);
    CHECK_STATUS(WW_ERR_NOT_CURRENT_SPEAKER,
                 ww_game_pass_speaker(game, game->phase_epoch,
                                      wrong_speaker));
    while (game->current_speaker != WW_NO_PLAYER) {
        uint8_t current = game->current_speaker;

        CHECK_STATUS(WW_OK,
                     ww_game_pass_speaker(game, game->phase_epoch, current));
    }
    CHECK(game->discussion_remaining_mask == 0u);
    CHECK_STATUS(WW_OK, ww_game_begin_vote(game, game->phase_epoch));
    CHECK(game->phase == WW_PHASE_VOTE);
    return true;
}

static uint8_t another_living_player(const ww_game_t *game, uint8_t player)
{
    uint8_t candidate;

    for (candidate = 0; candidate < WW_PLAYER_COUNT; ++candidate) {
        if (candidate != player &&
            (game->alive_mask & bit_for(candidate)) != 0u) {
            return candidate;
        }
    }
    return WW_NO_PLAYER;
}

static bool submit_unique_exile_vote(ww_game_t *game, uint8_t exiled)
{
    uint32_t epoch = game->phase_epoch;
    uint8_t voter;

    CHECK(game->phase == WW_PHASE_VOTE);
    CHECK((game->alive_mask & bit_for(exiled)) != 0u);
    for (voter = 0; voter < WW_PLAYER_COUNT; ++voter) {
        uint8_t target;

        if ((game->alive_mask & bit_for(voter)) == 0u) {
            continue;
        }
        target = voter == exiled ? another_living_player(game, voter) : exiled;
        CHECK(target != WW_NO_PLAYER);
        CHECK_STATUS(WW_OK,
                     ww_game_submit_vote(game, epoch, voter, target));
    }
    CHECK_STATUS(WW_OK, ww_game_finalize_vote(game, epoch));
    return true;
}

static bool test_lobby_and_error_contract(void)
{
    ww_game_t game;
    ww_game_t uninitialized = { 0 };
    ww_public_view_t public_view;
    ww_private_view_t private_view;
    uint32_t epoch;
    uint8_t player;

    CHECK_STATUS(WW_ERR_NULL, ww_game_init(NULL));
    CHECK_STATUS(WW_ERR_UNINITIALIZED,
                 ww_game_get_public_view(&uninitialized, &public_view));
    CHECK_STATUS(WW_OK, ww_game_init(&game));
    CHECK_STATUS(WW_OK, ww_game_get_public_view(&game, &public_view));
    CHECK(public_view.phase == WW_PHASE_LOBBY);
    CHECK(public_view.phase_epoch == 1u);
    CHECK(public_view.joined_count == 0u);
    CHECK(public_view.dawn_victim == WW_NO_PLAYER);
    CHECK_STATUS(WW_ERR_NOT_READY,
                 ww_game_start(&game, UINT64_C(0x1234)));
    CHECK_STATUS(WW_ERR_INVALID_PLAYER,
                 ww_game_join(&game, WW_PLAYER_COUNT));

    epoch = game.phase_epoch;
    CHECK_STATUS(WW_OK, ww_game_join(&game, 0u));
    CHECK(game.phase_epoch != epoch);
    epoch = game.phase_epoch;
    CHECK_STATUS(WW_ERR_ALREADY_JOINED, ww_game_join(&game, 0u));
    CHECK(game.phase_epoch == epoch);
    CHECK_STATUS(WW_OK, ww_game_get_private_view(&game, 0u, &private_view));
    CHECK(private_view.role == WW_ROLE_NONE);
    CHECK_STATUS(WW_ERR_NOT_JOINED,
                 ww_game_get_private_view(&game, 1u, &private_view));
    CHECK_STATUS(WW_OK, ww_game_leave_lobby(&game, 0u));
    CHECK_STATUS(WW_ERR_NOT_JOINED, ww_game_leave_lobby(&game, 0u));

    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        CHECK_STATUS(WW_OK, ww_game_join(&game, player));
    }
    CHECK_STATUS(WW_OK, ww_game_start(&game, UINT64_C(0x1234)));
    CHECK(game.phase == WW_PHASE_NIGHT);
    CHECK_STATUS(WW_ERR_INVALID_PHASE, ww_game_join(&game, 0u));
    CHECK_STATUS(WW_ERR_INVALID_PHASE,
                 ww_game_start(&game, UINT64_C(0x1234)));
    CHECK_STATUS(WW_ERR_GAME_NOT_OVER,
                 ww_game_get_role_reveal(&game, game.roles));
    CHECK(strcmp(ww_status_string(WW_ERR_STALE_ACTION), "STALE_ACTION") == 0);
    CHECK(strcmp(ww_phase_string(WW_PHASE_NIGHT), "NIGHT") == 0);
    CHECK(strcmp(ww_role_string(WW_ROLE_SEER), "SEER") == 0);
    CHECK(ww_role_camp(WW_ROLE_WOLF) == WW_CAMP_WOLF);
    CHECK(ww_role_camp(WW_ROLE_VILLAGER) == WW_CAMP_GOOD);
    return true;
}

static bool test_deterministic_roles_and_private_views(void)
{
    ww_game_t first;
    ww_game_t second;
    ww_game_t varied;
    role_ids_t ids;
    bool found_different_seed = false;
    uint64_t seed;
    uint8_t player;

    CHECK(setup_started_game(&first, UINT64_C(0x0123456789abcdef)));
    CHECK(setup_started_game(&second, UINT64_C(0x0123456789abcdef)));
    CHECK(find_role_ids(&first, &ids));

    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        ww_private_view_t first_view;
        ww_private_view_t second_view;

        CHECK_STATUS(WW_OK,
                     ww_game_get_private_view(&first, player, &first_view));
        CHECK_STATUS(WW_OK,
                     ww_game_get_private_view(&second, player, &second_view));
        CHECK(first_view.role == second_view.role);
        if (first_view.role == WW_ROLE_WOLF) {
            CHECK(count_mask(first_view.wolf_teammates_mask) == 1u);
            CHECK((first_view.wolf_teammates_mask & bit_for(player)) == 0u);
        } else {
            CHECK(first_view.wolf_teammates_mask == 0u);
        }
        if (first_view.role == WW_ROLE_SEER) {
            CHECK(first_view.seer_checked_mask == 0u);
            CHECK(first_view.seer_wolf_mask == 0u);
        }
    }

    for (seed = 1u; seed <= 16u && !found_different_seed; ++seed) {
        CHECK(setup_started_game(&varied, seed));
        for (player = 0; player < WW_PLAYER_COUNT; ++player) {
            ww_private_view_t baseline;
            ww_private_view_t candidate;

            CHECK_STATUS(WW_OK,
                         ww_game_get_private_view(&first, player, &baseline));
            CHECK_STATUS(WW_OK,
                         ww_game_get_private_view(&varied, player, &candidate));
            if (baseline.role != candidate.role) {
                found_different_seed = true;
                break;
            }
        }
    }
    CHECK(found_different_seed);
    return true;
}

static bool test_night_validation_seer_and_hidden_death(void)
{
    ww_game_t game;
    role_ids_t ids;
    ww_public_view_t public_view;
    ww_private_view_t seer_view;
    uint8_t victim;
    uint8_t guard_target;
    uint32_t epoch;

    CHECK(setup_started_game(&game, UINT64_C(0x1111222233334444)));
    CHECK(find_role_ids(&game, &ids));
    epoch = game.phase_epoch;

    CHECK_STATUS(WW_ERR_STALE_ACTION,
                 ww_game_submit_night_action(&game, epoch - 1u,
                                             ids.wolves[0], ids.villagers[0]));
    CHECK(game.submitted_mask == 0u);
    CHECK_STATUS(WW_ERR_SELF_TARGET,
                 ww_game_submit_night_action(&game, epoch,
                                             ids.wolves[0], ids.wolves[0]));
    CHECK_STATUS(WW_ERR_WOLF_FRIENDLY_FIRE,
                 ww_game_submit_night_action(&game, epoch,
                                             ids.wolves[0], ids.wolves[1]));
    CHECK_STATUS(WW_ERR_SELF_TARGET,
                 ww_game_submit_night_action(&game, epoch,
                                             ids.seer, ids.seer));
    CHECK_STATUS(WW_ERR_NOT_READY, ww_game_finalize_night(&game, epoch));

    victim = ids.villagers[0];
    guard_target = ids.guard;
    CHECK(submit_initial_night(&game, &ids, victim, victim,
                               guard_target, ids.wolves[0]));
    CHECK_STATUS(WW_ERR_ALREADY_SUBMITTED,
                 ww_game_submit_night_action(&game, epoch, victim, victim));
    CHECK_STATUS(WW_ERR_STALE_ACTION,
                 ww_game_finalize_night(&game, epoch - 1u));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, epoch));
    CHECK(game.phase == WW_PHASE_DAWN_RESULT);

    CHECK_STATUS(WW_OK, ww_game_get_public_view(&game, &public_view));
    CHECK(public_view.dawn_victim == victim);
    CHECK((public_view.alive_mask & bit_for(victim)) == 0u);
    CHECK(public_view.winner == WW_WINNER_NONE);
    CHECK_STATUS(WW_ERR_GAME_NOT_OVER,
                 ww_game_get_role_reveal(&game, game.roles));

    CHECK_STATUS(WW_OK,
                 ww_game_get_private_view(&game, ids.seer, &seer_view));
    CHECK((seer_view.seer_checked_mask & bit_for(ids.wolves[0])) != 0u);
    CHECK((seer_view.seer_wolf_mask & bit_for(ids.wolves[0])) != 0u);
    CHECK_STATUS(WW_ERR_STALE_ACTION,
                 ww_game_begin_discussion(&game, epoch));
    CHECK(begin_vote_after_dawn(&game));
    CHECK_STATUS(WW_ERR_PLAYER_DEAD,
                 ww_game_submit_vote(&game, game.phase_epoch,
                                     victim, ids.wolves[0]));
    CHECK_STATUS(WW_ERR_TARGET_DEAD,
                 ww_game_submit_vote(&game, game.phase_epoch,
                                     ids.wolves[0], victim));
    return true;
}

static bool test_guard_self_protect_and_consecutive_rule(void)
{
    ww_game_t game;
    role_ids_t ids;
    ww_private_view_t guard_view;
    uint8_t exiled;

    CHECK(setup_started_game(&game, UINT64_C(0x5555666677778888)));
    CHECK(find_role_ids(&game, &ids));

    CHECK(submit_initial_night(&game, &ids, ids.guard, ids.guard,
                               ids.guard, ids.wolves[0]));
    CHECK_STATUS(WW_OK,
                 ww_game_finalize_night(&game, game.phase_epoch));
    CHECK(game.phase == WW_PHASE_DAWN_RESULT);
    CHECK(game.dawn_victim == WW_NO_PLAYER);
    CHECK_STATUS(WW_OK,
                 ww_game_get_private_view(&game, ids.guard, &guard_view));
    CHECK(guard_view.guard_previous_target == ids.guard);

    CHECK(begin_vote_after_dawn(&game));
    exiled = ids.villagers[0];
    CHECK(submit_unique_exile_vote(&game, exiled));
    CHECK(game.phase == WW_PHASE_EXILE_RESULT);
    CHECK_STATUS(WW_OK,
                 ww_game_begin_next_night(&game, game.phase_epoch));
    CHECK(game.round_number == 2u);
    CHECK_STATUS(WW_ERR_GUARD_REPEAT,
                 ww_game_submit_night_action(&game, game.phase_epoch,
                                             ids.guard, ids.guard));
    CHECK(game.submitted_mask == 0u);
    CHECK_STATUS(WW_OK,
                 ww_game_submit_night_action(&game, game.phase_epoch,
                                             ids.guard, ids.seer));
    return true;
}

static bool test_wolf_revote_windows_and_empty_kill(void)
{
    ww_game_t game;
    ww_game_t empty_kill_game;
    role_ids_t ids;
    role_ids_t empty_ids;
    uint8_t target_a;
    uint8_t target_b;
    uint32_t initial_epoch;
    uint32_t revote_epoch;
    uint8_t player;

    CHECK(setup_started_game(&game, UINT64_C(0x9999aaaabbbbcccc)));
    CHECK(find_role_ids(&game, &ids));
    target_a = ids.villagers[0];
    target_b = ids.villagers[1];
    initial_epoch = game.phase_epoch;
    CHECK(submit_initial_night(&game, &ids, target_a, target_b,
                               ids.guard, ids.wolves[0]));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, initial_epoch));
    CHECK(game.phase == WW_PHASE_WOLF_REVOTE);
    CHECK(game.submitted_mask == 0u);
    revote_epoch = game.phase_epoch;
    CHECK_STATUS(WW_ERR_STALE_ACTION,
                 ww_game_submit_night_action(&game, initial_epoch,
                                             ids.wolves[0], target_a));

    CHECK_STATUS(WW_OK,
                 ww_game_submit_night_action(&game, revote_epoch,
                                             ids.wolves[0], target_a));
    CHECK_STATUS(WW_OK,
                 ww_game_submit_night_action(&game, revote_epoch,
                                             ids.wolves[1], target_a));
    CHECK_STATUS(WW_ERR_NOT_READY,
                 ww_game_finalize_night(&game, revote_epoch));
    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        if (player == ids.wolves[0] || player == ids.wolves[1]) {
            continue;
        }
        CHECK_STATUS(WW_OK,
                     ww_game_submit_night_action(&game, revote_epoch,
                                                 player, player));
    }
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, revote_epoch));
    CHECK(game.phase == WW_PHASE_DAWN_RESULT);
    CHECK(game.dawn_victim == target_a);

    CHECK(setup_started_game(&empty_kill_game,
                             UINT64_C(0xddddeeeeffff0001)));
    CHECK(find_role_ids(&empty_kill_game, &empty_ids));
    target_a = empty_ids.villagers[0];
    target_b = empty_ids.villagers[1];
    CHECK(submit_initial_night(&empty_kill_game, &empty_ids,
                               target_a, target_b, empty_ids.guard,
                               empty_ids.wolves[0]));
    CHECK_STATUS(WW_OK,
                 ww_game_finalize_night(&empty_kill_game,
                                        empty_kill_game.phase_epoch));
    CHECK(empty_kill_game.phase == WW_PHASE_WOLF_REVOTE);
    CHECK(submit_wolf_revote(&empty_kill_game, &empty_ids,
                             target_a, target_b));
    CHECK_STATUS(WW_OK,
                 ww_game_finalize_night(&empty_kill_game,
                                        empty_kill_game.phase_epoch));
    CHECK(empty_kill_game.phase == WW_PHASE_DAWN_RESULT);
    CHECK(empty_kill_game.dawn_victim == WW_NO_PLAYER);
    CHECK(empty_kill_game.alive_mask == WW_ALL_PLAYERS_MASK);
    return true;
}

static bool prepare_full_alive_vote(ww_game_t *game, role_ids_t *ids,
                                    uint64_t seed)
{
    CHECK(setup_started_game(game, seed));
    CHECK(find_role_ids(game, ids));
    CHECK(submit_initial_night(game, ids, ids->guard, ids->guard,
                               ids->guard, ids->wolves[0]));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(game, game->phase_epoch));
    CHECK(game->dawn_victim == WW_NO_PLAYER);
    CHECK(begin_vote_after_dawn(game));
    return true;
}

static bool test_tie_defense_and_unique_revote(void)
{
    ww_game_t game;
    role_ids_t ids;
    ww_private_view_t dead_view;
    uint32_t vote_epoch;
    uint32_t defense_epoch;
    uint32_t revote_epoch;
    static const uint8_t first_votes[WW_PLAYER_COUNT] = { 1, 0, 0, 1, 1, 0, 2 };
    uint8_t voter;

    CHECK(prepare_full_alive_vote(&game, &ids,
                                  UINT64_C(0x1020304050607080)));
    vote_epoch = game.phase_epoch;
    for (voter = 0; voter < WW_PLAYER_COUNT; ++voter) {
        CHECK(first_votes[voter] != voter);
        CHECK_STATUS(WW_OK,
                     ww_game_submit_vote(&game, vote_epoch, voter,
                                         first_votes[voter]));
    }
    CHECK_STATUS(WW_OK, ww_game_finalize_vote(&game, vote_epoch));
    CHECK(game.phase == WW_PHASE_TIE_DEFENSE);
    CHECK(game.tie_mask == (ww_player_mask_t)(bit_for(0u) | bit_for(1u)));
    CHECK(game.exiled_player == WW_NO_PLAYER);
    CHECK(game.discussion_remaining_mask == game.tie_mask);
    CHECK(game.current_speaker == 0u);

    defense_epoch = game.phase_epoch;
    CHECK_STATUS(WW_ERR_STALE_ACTION,
                 ww_game_begin_revote(&game, vote_epoch));
    CHECK_STATUS(WW_ERR_NOT_READY,
                 ww_game_begin_revote(&game, defense_epoch));
    CHECK_STATUS(WW_ERR_NOT_CURRENT_SPEAKER,
                 ww_game_pass_speaker(&game, defense_epoch, 2u));
    CHECK(game.phase_epoch == defense_epoch);
    CHECK(game.discussion_remaining_mask == game.tie_mask);
    CHECK(game.current_speaker == 0u);
    CHECK_STATUS(WW_OK,
                 ww_game_pass_speaker(&game, defense_epoch, 0u));
    CHECK(game.phase_epoch != defense_epoch);
    CHECK(game.discussion_remaining_mask == bit_for(1u));
    CHECK(game.current_speaker == 1u);
    CHECK_STATUS(WW_ERR_STALE_ACTION,
                 ww_game_pass_speaker(&game, defense_epoch, 1u));
    defense_epoch = game.phase_epoch;
    CHECK_STATUS(WW_OK,
                 ww_game_pass_speaker(&game, defense_epoch, 1u));
    CHECK(game.discussion_remaining_mask == 0u);
    CHECK(game.current_speaker == WW_NO_PLAYER);
    defense_epoch = game.phase_epoch;
    CHECK_STATUS(WW_OK, ww_game_begin_revote(&game, defense_epoch));
    CHECK(game.phase == WW_PHASE_REVOTE);
    revote_epoch = game.phase_epoch;
    CHECK_STATUS(WW_ERR_TARGET_NOT_TIED,
                 ww_game_submit_vote(&game, revote_epoch, 0u, 2u));
    CHECK(game.submitted_mask == 0u);
    for (voter = 0; voter < WW_PLAYER_COUNT; ++voter) {
        uint8_t target = voter == 0u ? 1u : 0u;

        CHECK_STATUS(WW_OK,
                     ww_game_submit_vote(&game, revote_epoch, voter, target));
    }
    CHECK_STATUS(WW_OK, ww_game_finalize_vote(&game, revote_epoch));
    CHECK(game.phase == WW_PHASE_EXILE_RESULT);
    CHECK(game.exiled_player == 0u);
    CHECK((game.alive_mask & bit_for(0u)) == 0u);
    CHECK_STATUS(WW_ERR_GAME_NOT_OVER,
                 ww_game_get_role_reveal(&game, game.roles));
    CHECK_STATUS(WW_OK, ww_game_get_private_view(&game, 0u, &dead_view));
    CHECK(!dead_view.alive);
    CHECK(dead_view.role != WW_ROLE_NONE);
    return true;
}

static bool test_second_tie_means_no_exile(void)
{
    ww_game_t game;
    role_ids_t ids;
    uint32_t epoch;
    static const uint8_t first_votes[WW_PLAYER_COUNT] = { 1, 0, 0, 1, 2, 2, 3 };
    static const uint8_t second_votes[WW_PLAYER_COUNT] = { 1, 0, 0, 0, 1, 1, 2 };
    uint8_t voter;

    CHECK(prepare_full_alive_vote(&game, &ids,
                                  UINT64_C(0x8877665544332211)));
    epoch = game.phase_epoch;
    for (voter = 0; voter < WW_PLAYER_COUNT; ++voter) {
        CHECK(first_votes[voter] != voter);
        CHECK_STATUS(WW_OK,
                     ww_game_submit_vote(&game, epoch, voter,
                                         first_votes[voter]));
    }
    CHECK_STATUS(WW_OK, ww_game_finalize_vote(&game, epoch));
    CHECK(game.phase == WW_PHASE_TIE_DEFENSE);
    CHECK(game.tie_mask == (ww_player_mask_t)(bit_for(0u) |
                                              bit_for(1u) |
                                              bit_for(2u)));
    CHECK(game.discussion_remaining_mask == game.tie_mask);
    CHECK(game.current_speaker == 0u);
    while (game.current_speaker != WW_NO_PLAYER) {
        uint8_t current = game.current_speaker;

        CHECK_STATUS(WW_OK,
                     ww_game_pass_speaker(&game, game.phase_epoch, current));
    }
    CHECK(game.discussion_remaining_mask == 0u);
    CHECK_STATUS(WW_OK, ww_game_begin_revote(&game, game.phase_epoch));
    epoch = game.phase_epoch;
    for (voter = 0; voter < WW_PLAYER_COUNT; ++voter) {
        CHECK(second_votes[voter] != voter);
        CHECK_STATUS(WW_OK,
                     ww_game_submit_vote(&game, epoch, voter,
                                         second_votes[voter]));
    }
    CHECK_STATUS(WW_OK, ww_game_finalize_vote(&game, epoch));
    CHECK(game.phase == WW_PHASE_EXILE_RESULT);
    CHECK(game.exiled_player == WW_NO_PLAYER);
    CHECK(game.alive_mask == WW_ALL_PLAYERS_MASK);
    CHECK_STATUS(WW_OK,
                 ww_game_begin_next_night(&game, game.phase_epoch));
    CHECK(game.phase == WW_PHASE_NIGHT);
    CHECK(game.round_number == 2u);
    CHECK(game.tie_mask == 0u);
    return true;
}

static bool test_good_win_and_terminal_reveal(void)
{
    ww_game_t game;
    role_ids_t ids;
    ww_role_t revealed[WW_PLAYER_COUNT];
    uint8_t second_guard_target;

    CHECK(setup_started_game(&game, UINT64_C(0x13579bdf2468ace0)));
    CHECK(find_role_ids(&game, &ids));

    CHECK(submit_initial_night(&game, &ids, ids.guard, ids.guard,
                               ids.guard, ids.wolves[0]));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, game.phase_epoch));
    CHECK(begin_vote_after_dawn(&game));
    CHECK(submit_unique_exile_vote(&game, ids.wolves[0]));
    CHECK(game.phase == WW_PHASE_EXILE_RESULT);
    CHECK_STATUS(WW_OK,
                 ww_game_begin_next_night(&game, game.phase_epoch));

    second_guard_target = ids.villagers[0];
    CHECK((game.alive_mask & bit_for(second_guard_target)) != 0u);
    CHECK(second_guard_target != ids.guard);
    CHECK(submit_initial_night(&game, &ids,
                               WW_NO_PLAYER, second_guard_target,
                               second_guard_target, ids.wolves[1]));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, game.phase_epoch));
    CHECK(game.phase == WW_PHASE_DAWN_RESULT);
    CHECK(game.dawn_victim == WW_NO_PLAYER);
    CHECK(begin_vote_after_dawn(&game));
    CHECK(submit_unique_exile_vote(&game, ids.wolves[1]));
    CHECK(game.phase == WW_PHASE_GAME_OVER);
    CHECK(game.winner == WW_WINNER_GOOD);
    CHECK_STATUS(WW_OK, ww_game_get_role_reveal(&game, revealed));
    CHECK(memcmp(revealed, game.roles, sizeof(revealed)) == 0);
    CHECK_STATUS(WW_ERR_INVALID_PHASE,
                 ww_game_begin_next_night(&game, game.phase_epoch));
    return true;
}

static bool test_wolf_parity_win_after_dawn(void)
{
    ww_game_t game;
    role_ids_t ids;
    ww_role_t revealed[WW_PLAYER_COUNT];
    uint8_t first_victim;
    uint8_t day_exile;
    uint8_t second_victim;
    uint8_t second_guard_target;

    CHECK(setup_started_game(&game, UINT64_C(0xfedcba9876543210)));
    CHECK(find_role_ids(&game, &ids));
    first_victim = ids.villagers[0];
    CHECK(submit_initial_night(&game, &ids, first_victim, first_victim,
                               ids.guard, ids.wolves[0]));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, game.phase_epoch));
    CHECK(game.dawn_victim == first_victim);
    CHECK(begin_vote_after_dawn(&game));
    day_exile = ids.villagers[1];
    CHECK(submit_unique_exile_vote(&game, day_exile));
    CHECK(game.phase == WW_PHASE_EXILE_RESULT);
    CHECK_STATUS(WW_OK,
                 ww_game_begin_next_night(&game, game.phase_epoch));

    CHECK_STATUS(WW_ERR_PLAYER_DEAD,
                 ww_game_submit_night_action(&game, game.phase_epoch,
                                             first_victim, first_victim));
    second_victim = ids.villagers[2];
    second_guard_target = ids.seer;
    CHECK(submit_initial_night(&game, &ids, second_victim, second_victim,
                               second_guard_target, ids.wolves[0]));
    CHECK_STATUS(WW_OK, ww_game_finalize_night(&game, game.phase_epoch));
    CHECK(game.phase == WW_PHASE_GAME_OVER);
    CHECK(game.winner == WW_WINNER_WOLVES);
    CHECK(game.dawn_victim == second_victim);
    CHECK(count_mask(game.alive_mask) == 4u);
    CHECK_STATUS(WW_OK, ww_game_get_role_reveal(&game, revealed));
    return true;
}

typedef bool (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t function;
} test_case_t;

int main(void)
{
    static const test_case_t tests[] = {
        { "lobby_and_error_contract", test_lobby_and_error_contract },
        { "deterministic_roles_and_private_views",
          test_deterministic_roles_and_private_views },
        { "night_validation_seer_and_hidden_death",
          test_night_validation_seer_and_hidden_death },
        { "guard_self_protect_and_consecutive_rule",
          test_guard_self_protect_and_consecutive_rule },
        { "wolf_revote_windows_and_empty_kill",
          test_wolf_revote_windows_and_empty_kill },
        { "tie_defense_and_unique_revote",
          test_tie_defense_and_unique_revote },
        { "second_tie_means_no_exile", test_second_tie_means_no_exile },
        { "good_win_and_terminal_reveal",
          test_good_win_and_terminal_reveal },
        { "wolf_parity_win_after_dawn",
          test_wolf_parity_win_after_dawn },
    };
    size_t index;
    size_t passed = 0;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (!tests[index].function()) {
            fprintf(stderr, "FAIL %s\n", tests[index].name);
            return 1;
        }
        ++passed;
        printf("PASS %s\n", tests[index].name);
    }
    printf("%zu/%zu tests passed\n", passed,
           sizeof(tests) / sizeof(tests[0]));
    return 0;
}
