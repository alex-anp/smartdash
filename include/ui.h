#ifndef UI_H
#define UI_H

#include <Arduino.h>

// Снимок значений из BLE-задачи (или из стартовой анимации).
// coolant = -1 и volt = 0 означают "данных ещё нет" и выводятся как "--".
struct DashData {
  int speed_kmh = 0;   // 0..99 км/ч
  int rpm = 0;         // 0..RPM_MAX
  int coolant_c = -1;  // °C, отрицательное значение = нет данных
  float volt = 0.0f;   // В, <= 0.1 = нет данных
  bool connected = false;
};

void ui_init();                              // Выделить спрайт, сбросить кэши и состояние анимации
void ui_set_data(const DashData& data);      // Вызывается из BLE-задачи, помечает дашборд «грязным»
void ui_show_boot();                         // Однократная заставка "SmartDash"
void ui_show_search();                       // Текстовый экран "Searching ELM..."
void ui_show_dash();                         // Перерисовать изменившиеся виджеты в спрайте
void ui_show_elm_connect();                  // Открыть экран ELM Connect и начать поиск
void ui_hide_elm_connect();                  // Вернуться на дашборд с полной перерисовкой
void ui_loop();                              // Опрос ввода, анимация, тикер и рендер активного экрана

#endif
