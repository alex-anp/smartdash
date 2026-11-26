#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>

// Тонкая обёртка над Arduino Preferences (NVS) и LittleFS.
// В NVS лежит адрес последнего успешного ELM и счётчик загрузок;
// журнал подключений пишется в файл на флэше и живёт между поездками.
void storage_init();
String storage_get_last_elm();
void storage_save_last_elm(const String &addr);
// Тип BLE-адреса: -1 = не сохранён (старая прошивка), 0 = public, 1 = random.
int storage_get_last_elm_type();
void storage_save_last_elm_type(uint8_t type);
void storage_clear_last_elm();

// Последний и лучший замер разгона. Формат строки задаёт dragrace.cpp:
// "target,total_cs,cs0|cs1|...", пустая строка = замеров ещё не было.
// Лучший результат хранится отдельно для каждой цели (60/80/100).
String storage_get_drag_last();
void storage_save_drag_last(const String& line);
String storage_get_drag_best(uint16_t target);
void storage_save_drag_best(uint16_t target, const String& line);

// Одна строка: "b<boot> t<секунды>.<мс> <сообщение>".
void storage_log(const char* fmt, ...);

// Чтение журнала порциями: prev + current без лишней копии в флэше.
size_t storage_log_size();
size_t storage_read_log(char* buf, size_t len, size_t offset);
void storage_clear_log();

// Вывод через USB Serial: "log" (хвост) и "log all" (весь файл).
void storage_dump_log_serial(bool all);
void storage_serial_loop();

#endif
