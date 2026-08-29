#pragma once

#include <stdint.h>

int png_write_rgb565(const char *path, const uint16_t *pixels,
                     unsigned width, unsigned height);
