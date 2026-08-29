#include "werewolf_game.h"

#include <string.h>

#define WW_GAME_MAGIC UINT32_C(0x57574631)

static bool player_is_valid(uint8_t player)
{
    return player < WW_PLAYER_COUNT;
}

static ww_player_mask_t player_bit(uint8_t player)
{
    return (ww_player_mask_t)(UINT8_C(1) << player);
}

static uint8_t mask_count(ww_player_mask_t mask)
{
    uint8_t count = 0;

    while (mask != 0u) {
        count = (uint8_t)(count + (mask & UINT8_C(1)));
        mask = (ww_player_mask_t)(mask >> 1);
    }
    return count;
}

static void clear_targets(uint8_t targets[WW_PLAYER_COUNT])
{
    uint8_t player;

    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        targets[player] = WW_NO_PLAYER;
    }
}

static ww_status_t validate_game(const ww_game_t *game)
{
    if (game == NULL) {
        return WW_ERR_NULL;
    }
    if (game->magic != WW_GAME_MAGIC) {
        return WW_ERR_UNINITIALIZED;
    }
    return WW_OK;
}

static ww_status_t validate_epoch(const ww_game_t *game,
                                  uint32_t expected_epoch)
{
    ww_status_t status = validate_game(game);

    if (status != WW_OK) {
        return status;
    }
    if (expected_epoch != game->phase_epoch) {
        return WW_ERR_STALE_ACTION;
    }
    return WW_OK;
}

static void bump_epoch(ww_game_t *game)
{
    ++game->phase_epoch;
    if (game->phase_epoch == 0u) {
        game->phase_epoch = 1u;
    }
}

static void enter_phase(ww_game_t *game, ww_phase_t phase)
{
    game->phase = phase;
    game->submitted_mask = 0u;
    bump_epoch(game);
}

static uint64_t splitmix64_next(uint64_t *state)
{
    uint64_t value;

    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t random_below(uint64_t *state, uint64_t upper_bound)
{
    uint64_t value;
    uint64_t threshold = (uint64_t)(0u - upper_bound) % upper_bound;

    do {
        value = splitmix64_next(state);
    } while (value < threshold);
    return value % upper_bound;
}

static void shuffle_roles(ww_game_t *game, uint64_t seed)
{
    static const ww_role_t deck[WW_PLAYER_COUNT] = {
        WW_ROLE_WOLF,
        WW_ROLE_WOLF,
        WW_ROLE_SEER,
        WW_ROLE_GUARD,
        WW_ROLE_VILLAGER,
        WW_ROLE_VILLAGER,
        WW_ROLE_VILLAGER,
    };
    uint64_t random_state = seed;
    uint8_t index;

    memcpy(game->roles, deck, sizeof(deck));
    for (index = (uint8_t)(WW_PLAYER_COUNT - 1u); index > 0u; --index) {
        uint8_t other = (uint8_t)random_below(&random_state,
                                               (uint64_t)index + 1u);
        ww_role_t temporary = game->roles[index];

        game->roles[index] = game->roles[other];
        game->roles[other] = temporary;
    }
}

static ww_winner_t evaluate_winner(const ww_game_t *game)
{
    uint8_t player;
    uint8_t living_wolves = 0;
    uint8_t living_others = 0;

    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        if ((game->alive_mask & player_bit(player)) == 0u) {
            continue;
        }
        if (game->roles[player] == WW_ROLE_WOLF) {
            ++living_wolves;
        } else {
            ++living_others;
        }
    }

    if (living_wolves == 0u) {
        return WW_WINNER_GOOD;
    }
    if (living_wolves >= living_others) {
        return WW_WINNER_WOLVES;
    }
    return WW_WINNER_NONE;
}

static void enter_result_or_game_over(ww_game_t *game,
                                      ww_phase_t result_phase)
{
    game->winner = evaluate_winner(game);
    if (game->winner != WW_WINNER_NONE) {
        enter_phase(game, WW_PHASE_GAME_OVER);
    } else {
        enter_phase(game, result_phase);
    }
}

static ww_status_t validate_living_actor(const ww_game_t *game,
                                         uint8_t player)
{
    if (!player_is_valid(player)) {
        return WW_ERR_INVALID_PLAYER;
    }
    if ((game->joined_mask & player_bit(player)) == 0u) {
        return WW_ERR_NOT_JOINED;
    }
    if ((game->alive_mask & player_bit(player)) == 0u) {
        return WW_ERR_PLAYER_DEAD;
    }
    return WW_OK;
}

static ww_status_t validate_living_target(const ww_game_t *game,
                                          uint8_t target)
{
    if (!player_is_valid(target)) {
        return WW_ERR_INVALID_TARGET;
    }
    if ((game->alive_mask & player_bit(target)) == 0u) {
        return WW_ERR_TARGET_DEAD;
    }
    return WW_OK;
}

static ww_status_t validate_night_target(const ww_game_t *game,
                                         uint8_t player, uint8_t target)
{
    ww_role_t role = game->roles[player];
    ww_status_t status = validate_living_target(game, target);

    if (status != WW_OK) {
        return status;
    }

    if (game->phase == WW_PHASE_WOLF_REVOTE && role != WW_ROLE_WOLF) {
        return WW_OK;
    }

    switch (role) {
    case WW_ROLE_WOLF:
        if (target == player) {
            return WW_ERR_SELF_TARGET;
        }
        if (game->roles[target] == WW_ROLE_WOLF) {
            return WW_ERR_WOLF_FRIENDLY_FIRE;
        }
        return WW_OK;
    case WW_ROLE_SEER:
        return target == player ? WW_ERR_SELF_TARGET : WW_OK;
    case WW_ROLE_GUARD:
        if (game->phase == WW_PHASE_NIGHT &&
            target == game->guard_previous_target) {
            return WW_ERR_GUARD_REPEAT;
        }
        return WW_OK;
    case WW_ROLE_VILLAGER:
        return WW_OK;
    case WW_ROLE_NONE:
    default:
        return WW_ERR_BAD_STATE;
    }
}

static ww_status_t wolf_consensus(const ww_game_t *game, bool *out_agreed,
                                  uint8_t *out_target)
{
    uint8_t player;
    uint8_t first_target = WW_NO_PLAYER;
    bool found_wolf = false;
    bool agreed = true;

    if (out_agreed == NULL || out_target == NULL) {
        return WW_ERR_NULL;
    }

    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        if ((game->alive_mask & player_bit(player)) == 0u ||
            game->roles[player] != WW_ROLE_WOLF) {
            continue;
        }
        if (!found_wolf) {
            first_target = game->action_targets[player];
            found_wolf = true;
        } else if (game->action_targets[player] != first_target) {
            agreed = false;
        }
    }

    if (!found_wolf || first_target == WW_NO_PLAYER) {
        return WW_ERR_BAD_STATE;
    }
    *out_agreed = agreed;
    *out_target = first_target;
    return WW_OK;
}

static ww_status_t apply_initial_night_support_actions(ww_game_t *game)
{
    uint8_t player;

    game->pending_guard_target = WW_NO_PLAYER;
    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        uint8_t target;

        if ((game->alive_mask & player_bit(player)) == 0u) {
            continue;
        }
        target = game->action_targets[player];
        if (!player_is_valid(target)) {
            return WW_ERR_BAD_STATE;
        }

        if (game->roles[player] == WW_ROLE_GUARD) {
            game->pending_guard_target = target;
            game->guard_previous_target = target;
        } else if (game->roles[player] == WW_ROLE_SEER) {
            game->seer_checked_mask |= player_bit(target);
            if (game->roles[target] == WW_ROLE_WOLF) {
                game->seer_wolf_mask |= player_bit(target);
            } else {
                game->seer_wolf_mask &= (ww_player_mask_t)~player_bit(target);
            }
        }
    }
    return WW_OK;
}

static void resolve_night(ww_game_t *game, bool wolves_agreed,
                          uint8_t wolf_target)
{
    game->dawn_victim = WW_NO_PLAYER;
    if (wolves_agreed && wolf_target != game->pending_guard_target) {
        game->alive_mask &= (ww_player_mask_t)~player_bit(wolf_target);
        game->dawn_victim = wolf_target;
    }
    clear_targets(game->action_targets);
    enter_result_or_game_over(game, WW_PHASE_DAWN_RESULT);
}

static void clear_votes(ww_game_t *game)
{
    clear_targets(game->vote_targets);
}

static ww_status_t collect_top_vote_mask(const ww_game_t *game,
                                         ww_player_mask_t *out_top_mask)
{
    uint8_t counts[WW_PLAYER_COUNT] = { 0 };
    uint8_t voter;
    uint8_t target;
    uint8_t maximum = 0;
    ww_player_mask_t top_mask = 0u;

    if (out_top_mask == NULL) {
        return WW_ERR_NULL;
    }

    for (voter = 0; voter < WW_PLAYER_COUNT; ++voter) {
        if ((game->alive_mask & player_bit(voter)) == 0u) {
            continue;
        }
        target = game->vote_targets[voter];
        if (!player_is_valid(target) ||
            (game->alive_mask & player_bit(target)) == 0u) {
            return WW_ERR_BAD_STATE;
        }
        ++counts[target];
    }

    for (target = 0; target < WW_PLAYER_COUNT; ++target) {
        if (counts[target] > maximum) {
            maximum = counts[target];
            top_mask = player_bit(target);
        } else if (counts[target] == maximum && maximum != 0u) {
            top_mask |= player_bit(target);
        }
    }

    if (maximum == 0u || top_mask == 0u) {
        return WW_ERR_BAD_STATE;
    }
    *out_top_mask = top_mask;
    return WW_OK;
}

static uint8_t only_player_in_mask(ww_player_mask_t mask)
{
    uint8_t player;

    for (player = 0; player < WW_PLAYER_COUNT; ++player) {
        if ((mask & player_bit(player)) != 0u) {
            return player;
        }
    }
    return WW_NO_PLAYER;
}

static uint8_t first_player_in_mask(ww_player_mask_t mask)
{
    return only_player_in_mask(mask);
}

ww_status_t ww_game_init(ww_game_t *game)
{
    if (game == NULL) {
        return WW_ERR_NULL;
    }

    memset(game, 0, sizeof(*game));
    game->magic = WW_GAME_MAGIC;
    game->phase_epoch = 1u;
    game->phase = WW_PHASE_LOBBY;
    game->guard_previous_target = WW_NO_PLAYER;
    game->pending_guard_target = WW_NO_PLAYER;
    game->current_speaker = WW_NO_PLAYER;
    game->dawn_victim = WW_NO_PLAYER;
    game->exiled_player = WW_NO_PLAYER;
    clear_targets(game->action_targets);
    clear_votes(game);
    return WW_OK;
}

ww_status_t ww_game_join(ww_game_t *game, uint8_t player)
{
    ww_status_t status = validate_game(game);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_LOBBY) {
        return WW_ERR_INVALID_PHASE;
    }
    if (!player_is_valid(player)) {
        return WW_ERR_INVALID_PLAYER;
    }
    if ((game->joined_mask & player_bit(player)) != 0u) {
        return WW_ERR_ALREADY_JOINED;
    }

    game->joined_mask |= player_bit(player);
    bump_epoch(game);
    return WW_OK;
}

ww_status_t ww_game_leave_lobby(ww_game_t *game, uint8_t player)
{
    ww_status_t status = validate_game(game);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_LOBBY) {
        return WW_ERR_INVALID_PHASE;
    }
    if (!player_is_valid(player)) {
        return WW_ERR_INVALID_PLAYER;
    }
    if ((game->joined_mask & player_bit(player)) == 0u) {
        return WW_ERR_NOT_JOINED;
    }

    game->joined_mask &= (ww_player_mask_t)~player_bit(player);
    bump_epoch(game);
    return WW_OK;
}

ww_status_t ww_game_start(ww_game_t *game, uint64_t shuffle_seed)
{
    ww_status_t status = validate_game(game);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_LOBBY) {
        return WW_ERR_INVALID_PHASE;
    }
    if (game->joined_mask != WW_ALL_PLAYERS_MASK) {
        return WW_ERR_NOT_READY;
    }

    game->shuffle_seed = shuffle_seed;
    shuffle_roles(game, shuffle_seed);
    game->winner = WW_WINNER_NONE;
    game->alive_mask = WW_ALL_PLAYERS_MASK;
    game->tie_mask = 0u;
    game->discussion_remaining_mask = 0u;
    game->seer_checked_mask = 0u;
    game->seer_wolf_mask = 0u;
    game->round_number = 1u;
    game->guard_previous_target = WW_NO_PLAYER;
    game->pending_guard_target = WW_NO_PLAYER;
    game->current_speaker = WW_NO_PLAYER;
    game->dawn_victim = WW_NO_PLAYER;
    game->exiled_player = WW_NO_PLAYER;
    clear_targets(game->action_targets);
    clear_votes(game);
    enter_phase(game, WW_PHASE_NIGHT);
    return WW_OK;
}

ww_status_t ww_game_get_public_view(const ww_game_t *game,
                                    ww_public_view_t *out_view)
{
    ww_status_t status = validate_game(game);

    if (status != WW_OK) {
        return status;
    }
    if (out_view == NULL) {
        return WW_ERR_NULL;
    }

    memset(out_view, 0, sizeof(*out_view));
    out_view->phase = game->phase;
    out_view->winner = game->winner;
    out_view->phase_epoch = game->phase_epoch;
    out_view->round_number = game->round_number;
    out_view->joined_mask = game->joined_mask;
    out_view->alive_mask = game->alive_mask;
    out_view->submitted_mask = game->submitted_mask;
    out_view->tie_mask = game->tie_mask;
    out_view->discussion_remaining_mask = game->discussion_remaining_mask;
    out_view->joined_count = mask_count(game->joined_mask);
    out_view->alive_count = mask_count(game->alive_mask);
    out_view->submitted_count = mask_count(game->submitted_mask);
    out_view->current_speaker = game->current_speaker;
    out_view->dawn_victim = game->dawn_victim;
    out_view->exiled_player = game->exiled_player;
    return WW_OK;
}

ww_status_t ww_game_get_private_view(const ww_game_t *game, uint8_t player,
                                     ww_private_view_t *out_view)
{
    ww_status_t status = validate_game(game);
    ww_role_t role;
    uint8_t other;

    if (status != WW_OK) {
        return status;
    }
    if (out_view == NULL) {
        return WW_ERR_NULL;
    }
    if (!player_is_valid(player)) {
        return WW_ERR_INVALID_PLAYER;
    }
    if ((game->joined_mask & player_bit(player)) == 0u) {
        return WW_ERR_NOT_JOINED;
    }

    memset(out_view, 0, sizeof(*out_view));
    out_view->player = player;
    out_view->joined = true;
    out_view->alive = (game->alive_mask & player_bit(player)) != 0u;
    out_view->guard_previous_target = WW_NO_PLAYER;
    out_view->submitted_target = WW_NO_PLAYER;

    if (game->phase == WW_PHASE_LOBBY) {
        out_view->role = WW_ROLE_NONE;
        return WW_OK;
    }

    role = game->roles[player];
    out_view->role = role;
    if (role == WW_ROLE_WOLF) {
        for (other = 0; other < WW_PLAYER_COUNT; ++other) {
            if (other != player && game->roles[other] == WW_ROLE_WOLF) {
                out_view->wolf_teammates_mask |= player_bit(other);
            }
        }
    } else if (role == WW_ROLE_SEER) {
        out_view->seer_checked_mask = game->seer_checked_mask;
        out_view->seer_wolf_mask = game->seer_wolf_mask;
    } else if (role == WW_ROLE_GUARD) {
        out_view->guard_previous_target = game->guard_previous_target;
    }

    if (game->phase == WW_PHASE_NIGHT ||
        game->phase == WW_PHASE_WOLF_REVOTE) {
        out_view->action_submitted =
            (game->submitted_mask & player_bit(player)) != 0u;
        if (out_view->action_submitted) {
            out_view->submitted_target = game->action_targets[player];
        }
    } else if (game->phase == WW_PHASE_VOTE ||
               game->phase == WW_PHASE_REVOTE) {
        out_view->action_submitted =
            (game->submitted_mask & player_bit(player)) != 0u;
        if (out_view->action_submitted) {
            out_view->submitted_target = game->vote_targets[player];
        }
    }
    return WW_OK;
}

ww_status_t ww_game_get_role_reveal(const ww_game_t *game,
                                    ww_role_t out_roles[WW_PLAYER_COUNT])
{
    ww_status_t status = validate_game(game);

    if (status != WW_OK) {
        return status;
    }
    if (out_roles == NULL) {
        return WW_ERR_NULL;
    }
    if (game->phase != WW_PHASE_GAME_OVER) {
        return WW_ERR_GAME_NOT_OVER;
    }

    memcpy(out_roles, game->roles, sizeof(game->roles));
    return WW_OK;
}

ww_status_t ww_game_submit_night_action(ww_game_t *game,
                                        uint32_t expected_epoch,
                                        uint8_t player, uint8_t target)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_NIGHT &&
        game->phase != WW_PHASE_WOLF_REVOTE) {
        return WW_ERR_INVALID_PHASE;
    }
    status = validate_living_actor(game, player);
    if (status != WW_OK) {
        return status;
    }
    if ((game->submitted_mask & player_bit(player)) != 0u) {
        return WW_ERR_ALREADY_SUBMITTED;
    }
    status = validate_night_target(game, player, target);
    if (status != WW_OK) {
        return status;
    }

    game->action_targets[player] = target;
    game->submitted_mask |= player_bit(player);
    return WW_OK;
}

ww_status_t ww_game_finalize_night(ww_game_t *game,
                                   uint32_t expected_epoch)
{
    ww_status_t status = validate_epoch(game, expected_epoch);
    bool wolves_agreed;
    uint8_t wolf_target;

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_NIGHT &&
        game->phase != WW_PHASE_WOLF_REVOTE) {
        return WW_ERR_INVALID_PHASE;
    }
    if ((game->submitted_mask & game->alive_mask) != game->alive_mask) {
        return WW_ERR_NOT_READY;
    }

    status = wolf_consensus(game, &wolves_agreed, &wolf_target);
    if (status != WW_OK) {
        return status;
    }

    if (game->phase == WW_PHASE_NIGHT) {
        status = apply_initial_night_support_actions(game);
        if (status != WW_OK) {
            return status;
        }
        if (!wolves_agreed) {
            clear_targets(game->action_targets);
            enter_phase(game, WW_PHASE_WOLF_REVOTE);
            return WW_OK;
        }
        resolve_night(game, true, wolf_target);
        return WW_OK;
    }

    resolve_night(game, wolves_agreed, wolf_target);
    return WW_OK;
}

ww_status_t ww_game_begin_discussion(ww_game_t *game,
                                     uint32_t expected_epoch)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_DAWN_RESULT) {
        return WW_ERR_INVALID_PHASE;
    }

    game->discussion_remaining_mask = game->alive_mask;
    game->current_speaker = first_player_in_mask(game->discussion_remaining_mask);
    if (game->current_speaker == WW_NO_PLAYER) {
        return WW_ERR_BAD_STATE;
    }
    enter_phase(game, WW_PHASE_DISCUSSION);
    return WW_OK;
}

ww_status_t ww_game_pass_speaker(ww_game_t *game, uint32_t expected_epoch,
                                 uint8_t player)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_DISCUSSION &&
        game->phase != WW_PHASE_TIE_DEFENSE) {
        return WW_ERR_INVALID_PHASE;
    }
    status = validate_living_actor(game, player);
    if (status != WW_OK) {
        return status;
    }
    if (player != game->current_speaker) {
        return WW_ERR_NOT_CURRENT_SPEAKER;
    }

    game->discussion_remaining_mask &=
        (ww_player_mask_t)~player_bit(player);
    game->current_speaker =
        first_player_in_mask(game->discussion_remaining_mask);
    bump_epoch(game);
    return WW_OK;
}

ww_status_t ww_game_begin_vote(ww_game_t *game, uint32_t expected_epoch)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_DISCUSSION) {
        return WW_ERR_INVALID_PHASE;
    }
    if (game->discussion_remaining_mask != 0u ||
        game->current_speaker != WW_NO_PLAYER) {
        return WW_ERR_NOT_READY;
    }

    game->tie_mask = 0u;
    game->exiled_player = WW_NO_PLAYER;
    clear_votes(game);
    enter_phase(game, WW_PHASE_VOTE);
    return WW_OK;
}

ww_status_t ww_game_submit_vote(ww_game_t *game, uint32_t expected_epoch,
                                uint8_t player, uint8_t target)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_VOTE && game->phase != WW_PHASE_REVOTE) {
        return WW_ERR_INVALID_PHASE;
    }
    status = validate_living_actor(game, player);
    if (status != WW_OK) {
        return status;
    }
    if ((game->submitted_mask & player_bit(player)) != 0u) {
        return WW_ERR_ALREADY_SUBMITTED;
    }
    status = validate_living_target(game, target);
    if (status != WW_OK) {
        return status;
    }
    if (target == player) {
        return WW_ERR_SELF_TARGET;
    }
    if (game->phase == WW_PHASE_REVOTE &&
        (game->tie_mask & player_bit(target)) == 0u) {
        return WW_ERR_TARGET_NOT_TIED;
    }

    game->vote_targets[player] = target;
    game->submitted_mask |= player_bit(player);
    return WW_OK;
}

ww_status_t ww_game_finalize_vote(ww_game_t *game,
                                  uint32_t expected_epoch)
{
    ww_status_t status = validate_epoch(game, expected_epoch);
    ww_player_mask_t top_mask;

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_VOTE && game->phase != WW_PHASE_REVOTE) {
        return WW_ERR_INVALID_PHASE;
    }
    if ((game->submitted_mask & game->alive_mask) != game->alive_mask) {
        return WW_ERR_NOT_READY;
    }

    status = collect_top_vote_mask(game, &top_mask);
    if (status != WW_OK) {
        return status;
    }

    clear_votes(game);
    if (mask_count(top_mask) == 1u) {
        uint8_t exiled = only_player_in_mask(top_mask);

        game->alive_mask &= (ww_player_mask_t)~player_bit(exiled);
        game->exiled_player = exiled;
        enter_result_or_game_over(game, WW_PHASE_EXILE_RESULT);
        return WW_OK;
    }

    if (game->phase == WW_PHASE_VOTE) {
        game->tie_mask = top_mask;
        game->discussion_remaining_mask = top_mask;
        game->current_speaker = first_player_in_mask(top_mask);
        enter_phase(game, WW_PHASE_TIE_DEFENSE);
        return WW_OK;
    }

    game->exiled_player = WW_NO_PLAYER;
    enter_result_or_game_over(game, WW_PHASE_EXILE_RESULT);
    return WW_OK;
}

ww_status_t ww_game_begin_revote(ww_game_t *game,
                                 uint32_t expected_epoch)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_TIE_DEFENSE) {
        return WW_ERR_INVALID_PHASE;
    }
    if (mask_count(game->tie_mask) < 2u) {
        return WW_ERR_BAD_STATE;
    }
    if (game->discussion_remaining_mask != 0u ||
        game->current_speaker != WW_NO_PLAYER) {
        return WW_ERR_NOT_READY;
    }

    clear_votes(game);
    enter_phase(game, WW_PHASE_REVOTE);
    return WW_OK;
}

ww_status_t ww_game_begin_next_night(ww_game_t *game,
                                     uint32_t expected_epoch)
{
    ww_status_t status = validate_epoch(game, expected_epoch);

    if (status != WW_OK) {
        return status;
    }
    if (game->phase != WW_PHASE_EXILE_RESULT) {
        return WW_ERR_INVALID_PHASE;
    }

    if (game->round_number < UINT16_MAX) {
        ++game->round_number;
    }
    game->tie_mask = 0u;
    game->discussion_remaining_mask = 0u;
    game->pending_guard_target = WW_NO_PLAYER;
    game->current_speaker = WW_NO_PLAYER;
    game->dawn_victim = WW_NO_PLAYER;
    game->exiled_player = WW_NO_PLAYER;
    clear_targets(game->action_targets);
    clear_votes(game);
    enter_phase(game, WW_PHASE_NIGHT);
    return WW_OK;
}

ww_camp_t ww_role_camp(ww_role_t role)
{
    switch (role) {
    case WW_ROLE_WOLF:
        return WW_CAMP_WOLF;
    case WW_ROLE_SEER:
    case WW_ROLE_GUARD:
    case WW_ROLE_VILLAGER:
        return WW_CAMP_GOOD;
    case WW_ROLE_NONE:
    default:
        return WW_CAMP_UNKNOWN;
    }
}

const char *ww_status_string(ww_status_t status)
{
    switch (status) {
    case WW_OK: return "OK";
    case WW_ERR_NULL: return "NULL";
    case WW_ERR_UNINITIALIZED: return "UNINITIALIZED";
    case WW_ERR_INVALID_PLAYER: return "INVALID_PLAYER";
    case WW_ERR_INVALID_TARGET: return "INVALID_TARGET";
    case WW_ERR_INVALID_PHASE: return "INVALID_PHASE";
    case WW_ERR_STALE_ACTION: return "STALE_ACTION";
    case WW_ERR_NOT_JOINED: return "NOT_JOINED";
    case WW_ERR_ALREADY_JOINED: return "ALREADY_JOINED";
    case WW_ERR_NOT_READY: return "NOT_READY";
    case WW_ERR_ALREADY_SUBMITTED: return "ALREADY_SUBMITTED";
    case WW_ERR_PLAYER_DEAD: return "PLAYER_DEAD";
    case WW_ERR_TARGET_DEAD: return "TARGET_DEAD";
    case WW_ERR_SELF_TARGET: return "SELF_TARGET";
    case WW_ERR_WOLF_FRIENDLY_FIRE: return "WOLF_FRIENDLY_FIRE";
    case WW_ERR_GUARD_REPEAT: return "GUARD_REPEAT";
    case WW_ERR_TARGET_NOT_TIED: return "TARGET_NOT_TIED";
    case WW_ERR_GAME_NOT_OVER: return "GAME_NOT_OVER";
    case WW_ERR_BAD_STATE: return "BAD_STATE";
    case WW_ERR_NOT_CURRENT_SPEAKER: return "NOT_CURRENT_SPEAKER";
    default: return "UNKNOWN_STATUS";
    }
}

const char *ww_phase_string(ww_phase_t phase)
{
    switch (phase) {
    case WW_PHASE_LOBBY: return "LOBBY";
    case WW_PHASE_NIGHT: return "NIGHT";
    case WW_PHASE_WOLF_REVOTE: return "WOLF_REVOTE";
    case WW_PHASE_DAWN_RESULT: return "DAWN_RESULT";
    case WW_PHASE_DISCUSSION: return "DISCUSSION";
    case WW_PHASE_VOTE: return "VOTE";
    case WW_PHASE_TIE_DEFENSE: return "TIE_DEFENSE";
    case WW_PHASE_REVOTE: return "REVOTE";
    case WW_PHASE_EXILE_RESULT: return "EXILE_RESULT";
    case WW_PHASE_GAME_OVER: return "GAME_OVER";
    default: return "UNKNOWN_PHASE";
    }
}

const char *ww_role_string(ww_role_t role)
{
    switch (role) {
    case WW_ROLE_NONE: return "NONE";
    case WW_ROLE_WOLF: return "WOLF";
    case WW_ROLE_SEER: return "SEER";
    case WW_ROLE_GUARD: return "GUARD";
    case WW_ROLE_VILLAGER: return "VILLAGER";
    default: return "UNKNOWN_ROLE";
    }
}
