#pragma once

#include <lgfx/v1/lgfx_fonts.hpp>

// Цифры шкалы оборотов, нарисованные вручную по reference/font.png (только 0..9).
// Генерируются tools/generate_digits.py из tools/digits-mask.txt.
extern const lgfx::BMPfont EURO_Digits;
