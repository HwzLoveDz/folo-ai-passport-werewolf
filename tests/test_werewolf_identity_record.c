#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "werewolf_identity_record.h"

int main(void)
{
    uint8_t record[WEREWOLF_IDENTITY_RECORD_SIZE];
    uint8_t invalid[WEREWOLF_IDENTITY_RECORD_SIZE];
    werewolf_nickname_t nickname;
    uint32_t revision = 0U;

    assert(werewolf_identity_record_encode(record, "GG Bond",
                                            UINT32_C(0x01020304)));
    assert(record[0] == 'W' && record[1] == 'N' && record[2] == 1U);
    assert(record[3] == 7U);
    assert(memcmp(&record[4], (uint8_t[]){1U, 2U, 3U, 4U}, 4U) == 0);
    assert(werewolf_identity_record_decode(record, sizeof(record), nickname,
                                            &revision));
    assert(strcmp(nickname, "GG Bond") == 0);
    assert(revision == UINT32_C(0x01020304));

    assert(!werewolf_identity_record_encode(record, "", 1U));
    assert(!werewolf_identity_record_encode(record, "Harvey", 0U));
    memcpy(invalid, record, sizeof(invalid));
    invalid[0] = 0U;
    assert(!werewolf_identity_record_decode(invalid, sizeof(invalid), nickname,
                                             &revision));
    assert(werewolf_identity_record_encode(invalid, "Harvey", 1U));
    invalid[17] = 1U;
    assert(!werewolf_identity_record_decode(invalid, sizeof(invalid), nickname,
                                             &revision));
    assert(!werewolf_identity_record_decode(record, sizeof(record) - 1U,
                                             nickname, &revision));

    puts("werewolf identity record tests passed");
    return 0;
}
