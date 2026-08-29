#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "werewolf_nickname.h"

int main(void)
{
    werewolf_nickname_t name;
    char unterminated[WEREWOLF_NICKNAME_CAPACITY];

    assert(werewolf_nickname_normalize(name, "Harvey"));
    assert(strcmp(name, "Harvey") == 0);
    assert(werewolf_nickname_normalize(name, "GG Bond"));
    assert(strcmp(name, "GG Bond") == 0);
    assert(werewolf_nickname_normalize(name, "ABCDEFGHIJKL"));
    assert(strcmp(name, "ABCDEFGHIJ") == 0);
    assert(werewolf_nickname_valid("ABCDEFGHIJ"));
    assert(!werewolf_nickname_valid("ABCDEFGHIJK"));
    assert(!werewolf_nickname_normalize(name, ""));
    assert(!werewolf_nickname_normalize(name, " LEAD"));
    assert(!werewolf_nickname_normalize(name, "TAIL "));
    assert(!werewolf_nickname_normalize(name, "BAD\nNAME"));
    assert(!werewolf_nickname_normalize(name, "\xe5\x93\x88\xe7\xbb\xb4"));
    memset(unterminated, 'X', sizeof(unterminated));
    assert(!werewolf_nickname_valid(unterminated));

    puts("werewolf nickname tests passed");
    return 0;
}
