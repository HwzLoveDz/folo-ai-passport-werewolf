#include "werewolf_lobby.h"

#include "werewolf_game.h"

bool werewolf_lobby_can_start(uint8_t occupied_mask,
                              uint8_t profile_mask,
                              uint8_t ready_mask)
{
    return occupied_mask == WW_ALL_PLAYERS_MASK &&
           profile_mask == WW_ALL_PLAYERS_MASK &&
           ready_mask == WW_ALL_PLAYERS_MASK;
}
