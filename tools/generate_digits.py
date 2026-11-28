#!/usr/bin/env python3
"""Генератор цифр шкалы RPM из текстовых масок digits-mask.txt.

Редактируемый источник — tools/digits-mask.txt: десять цифр по 18 строк
шириной 15 символов ('#' — чёрный пиксель, '.' — пустой). Правим пиксели
в этом файле, запускаем `python3 tools/generate_digits.py`, получаем
пересобранный src/font_euro_digits.cpp. `python3 tools/generate_digits.py
--preview` показывает текущие маски без пересборки.

Источник геометрии — наш собственный bitmap reference/font.png,
внешних лицензий нет.

Особенности lgfx::BMPfont учтены генератором:
  - глифы лежат по индексам 16..25 (drawChar вычитает 0x20);
  - последний столбец таблицы движок пропускает, поэтому справа пустой столбец;
  - биты строки выровнены по старшему разряду байта.
"""

from pathlib import Path
import sys

W, H = 15, 18
TABLE_W = W + 1
BYTES = (TABLE_W + 7) // 8
TOTAL_BITS = BYTES * 8
MASKS_FILE = Path(__file__).resolve().with_name("digits-mask.txt")


def load_masks():
    digits = {}
    current = None
    for lineno, raw in enumerate(MASKS_FILE.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("# "):  # комментарии начинаются с '# '
            continue
        if len(line) == 2 and line[0] in "0123456789" and line[1] == ":":
            current = line[0]
            digits[current] = []
            continue
        if current is None:
            raise ValueError("строка %d: маска до заголовка цифры" % lineno)
        if len(line) != W:
            raise ValueError("строка %d: маска должна быть шириной %d: %r" % (lineno, W, line))
        if any(c not in "#." for c in line):
            raise ValueError("строка %d: только '#' и '.': %r" % (lineno, line))
        digits[current].append(line)

    missing = sorted(set("0123456789") - set(digits))
    if missing:
        raise ValueError("нет масок для цифр: %s" % ", ".join(missing))
    for ch, rows in digits.items():
        if len(rows) != H:
            raise ValueError("цифра %s: строк %d, нужно %d" % (ch, len(rows), H))
    return digits


def pack(mask):
    out = []
    for row in mask:
        assert len(row) == W, "строка должна быть шириной %d: %r" % (W, row)
        val = 0
        for x, c in enumerate(row):
            if c == "#":
                val |= 1 << (TOTAL_BITS - 1 - x)
        out.extend(val.to_bytes(BYTES, "big"))
    return out


def preview(digits):
    for y in range(H):
        print(" ".join("".join("#" if digits[ch][y][x] == "#" else "."
                               for x in range(W)) for ch in "0123456789"))


def main():
    digits = load_masks()
    if "--preview" in sys.argv:
        preview(digits)
        return
    lines = [
        "/*",
        " * Цифры шкалы оборотов, нарисованные вручную по нашему bitmap",
        " * reference/font.png и сжатые до 15x18 px. Внешних лицензий нет.",
        " * Генерируется tools/generate_digits.py из tools/digits-mask.txt.",
        " * Включены только цифры 0..9.",
        " */",
        '#include "font_euro_digits.h"',
        "",
        "namespace {",
        "",
        "constexpr uint8_t EURO_HEIGHT = %d;" % H,
        "constexpr uint8_t EURO_WIDTH = %d;" % TABLE_W,
        "",
    ]

    def emit(name, data):
        lines.append("const uint8_t %s[EURO_HEIGHT * %d] = {" % (name, BYTES))
        for i in range(0, len(data), 12):
            chunk = data[i:i + 12]
            lines.append("   " + "".join(" 0x%02X," % v for v in chunk))
        lines[-1] = lines[-1].rstrip(",")
        lines.append("};")
        lines.append("")

    emit("euro_empty", [0] * (H * BYTES))
    for ch in "0123456789":
        emit("euro_glyph_%s" % ch, pack(digits[ch]))

    lines.append("const uint8_t* euro_glyphs[96] = {")
    for row in range(12):
        vals = ["euro_glyph_%d" % (idx - 16) if 16 <= idx <= 25 else "euro_empty"
                for idx in range(row * 8, row * 8 + 8)]
        lines.append("  " + ", ".join(vals) + ",")
    lines[-1] = lines[-1].rstrip(",")
    lines.append("};")
    lines.append("")
    lines.append("const uint8_t euro_widths[96] = {")
    for row in range(12):
        vals = [str(TABLE_W) if 16 <= idx <= 25 else "0"
                for idx in range(row * 8, row * 8 + 8)]
        lines.append("  " + ", ".join(vals) + ",")
    lines[-1] = lines[-1].rstrip(",")
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace")
    lines.append("")
    lines.append("const lgfx::BMPfont EURO_Digits(euro_glyphs, euro_widths, EURO_WIDTH, EURO_HEIGHT, EURO_HEIGHT);")
    lines.append("")

    out = Path(__file__).resolve().parents[1] / "src" / "font_euro_digits.cpp"
    out.write_text("\n".join(lines))
    print("wrote", out)


if __name__ == "__main__":
    main()
