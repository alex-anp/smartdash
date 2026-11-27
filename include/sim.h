#ifndef SIM_H
#define SIM_H

#include "ui.h"

// Тестовый режим для проверки вывода без реального ELM327.
// Команды приходят по тому же USB Serial, что и 'log' / 'clear'.
//
//   sim                                  включить с текущими значениями
//   sim rpm=3500 spd=60 coolant=88 volt=14.1   задать поля выборочно
//   sim 3500 60 88 14.1                  то же в позиционном виде
//   sim off                              выключить, вернуть данные от ELM
//   sim reset                            стартовые значения (холостые)
//   sim status                           текущее состояние
//
// Возвращает true, если строка была командой sim (в этом случае обычный
// разбор 'log' / 'clear' её пропускает).
bool sim_handle_line(const char* line);

bool sim_is_active();                     // тестовый режим включён
bool sim_get_data(DashData& out);         // забрать очередной снимок (как ui_get_data)

#endif
