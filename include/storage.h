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
// Имя успешно подключённого адаптера: нужно для экрана информации о ELM после
// перезагрузки, когда подключение идёт прямо по сохранённому MAC без сканирования.
String storage_get_last_elm_name();
void storage_save_last_elm_name(const String &name);
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

// Подробные данные заезда: по одному CSV-файлу на заезд в каталоге /drag.
// Имя вида "0001_t60_b52.csv" кодирует номер, цель и номер загрузки.
void storage_drag_begin(uint16_t target, uint32_t startMs);
void storage_drag_sample(uint32_t tMs, uint32_t txMs, uint32_t rxMs,
                         int rpm, int speed, const char* event);
void storage_drag_finish(uint32_t totalCs, const char* splits,
                         bool aborted, const char* reason);
// Отменить незавершённый заезд: закрыть и удалить его CSV (например, по сбросу
// замера двойным тапом). Ничего не делает, если файл уже закрыт.
void storage_drag_reset();

// Доступ к файлам заездов по индексу (0 — самый старый).
size_t storage_drag_count();
String storage_drag_name(size_t index);
size_t storage_drag_size(size_t index);
size_t storage_drag_read(size_t index, char* buf, size_t len, size_t offset);
void storage_drag_remove(const String& name);  // удалить один заезд по имени файла
void storage_drag_clear();

// Вывод через USB Serial: "log" (хвост) и "log all" (весь файл).
void storage_dump_log_serial(bool all);
void storage_serial_loop();

#endif
