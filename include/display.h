#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <LovyanGFX.hpp>

// display.cpp владеет экземпляром LovyanGFX с настройками конкретной платы.
// UI получает устройство через display_get() и при необходимости рисует
// в спрайт, расположенный в PSRAM.
LGFX_Device& display_get();
void display_init();
void display_set_rotation(int degrees);  // только 0/90/180/270, остальные округляются
void display_clear(uint16_t color);
void display_set_brightness(uint8_t percent);

#endif
