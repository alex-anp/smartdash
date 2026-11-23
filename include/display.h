#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <LovyanGFX.hpp>

LGFX_Device& display_get();
void display_init();
void display_clear(uint16_t color);
void display_set_brightness(uint8_t percent);

#endif
