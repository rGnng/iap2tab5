#pragma once
#include <stdbool.h>
#include <stdint.h>
bool jpeg_thumbnail_decode(const char *path, uint8_t *rgb565, unsigned size);
void jpeg_thumbnail_cleanup(void);
