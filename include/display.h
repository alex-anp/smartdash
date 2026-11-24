#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <LovyanGFX.hpp>

// display.cpp owns the board-specific LovyanGFX instance. UI code receives the
// device through display_get() and optionally wraps drawing in a PSRAM sprite.
LGFX_Device& display_get();
void display_init();
void display_clear(uint16_t color);
void display_set_brightness(uint8_t percent);

#endif
