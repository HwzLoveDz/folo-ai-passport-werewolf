#include "werewolf_nickname.h"

#include <string.h>

static bool supported_char(unsigned char value)
{
    return value >= 0x20U && value <= 0x7eU;
}

bool werewolf_nickname_normalize(werewolf_nickname_t output,
                                 const char *input)
{
    size_t length = 0U;

    if (output == NULL) {
        return false;
    }
    memset(output, 0, WEREWOLF_NICKNAME_CAPACITY);
    if (input == NULL || input[0] == '\0' || input[0] == ' ') {
        return false;
    }

    while (input[length] != '\0' &&
           length < WEREWOLF_NICKNAME_MAX_CHARS) {
        unsigned char value = (unsigned char)input[length];

        if (!supported_char(value)) {
            memset(output, 0, WEREWOLF_NICKNAME_CAPACITY);
            return false;
        }
        output[length] = (char)value;
        ++length;
    }
    if (length == 0U || output[length - 1U] == ' ') {
        memset(output, 0, WEREWOLF_NICKNAME_CAPACITY);
        return false;
    }
    output[length] = '\0';
    return true;
}

bool werewolf_nickname_valid(const char *nickname)
{
    werewolf_nickname_t canonical;
    size_t input_length = 0U;

    if (nickname == NULL) {
        return false;
    }
    while (input_length < WEREWOLF_NICKNAME_CAPACITY &&
           nickname[input_length] != '\0') {
        ++input_length;
    }
    return input_length > 0U &&
           input_length <= WEREWOLF_NICKNAME_MAX_CHARS &&
           werewolf_nickname_normalize(canonical, nickname) &&
           strcmp(canonical, nickname) == 0;
}
