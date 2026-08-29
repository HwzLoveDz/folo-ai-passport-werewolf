#include <assert.h>

#include "werewolf_game.h"
#include "werewolf_lobby.h"

int main(void)
{
    const uint8_t all = WW_ALL_PLAYERS_MASK;

    assert(werewolf_lobby_can_start(all, all, all));
    assert(!werewolf_lobby_can_start(UINT8_C(0x3f), all, all));
    assert(!werewolf_lobby_can_start(all, UINT8_C(0x3f), all));
    assert(!werewolf_lobby_can_start(all, all, UINT8_C(0x3f)));
    assert(!werewolf_lobby_can_start(0U, 0U, 0U));
    return 0;
}
