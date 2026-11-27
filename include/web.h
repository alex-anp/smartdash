#ifndef WEB_H
#define WEB_H

#include <Arduino.h>

// Точка доступа Wi-Fi и HTTP-сервер. web_loop() нужно регулярно вызывать
// из loop(), чтобы запросы обрабатывались без блокировки UI.
void web_init();          // Зарегистрировать маршруты и применить сохранённое состояние
void web_loop();          // Обработать один ожидающий HTTP-запрос, если включено

// Включить/выключить AP, DNS и HTTP. Состояние пишется в gSettings/NVS.
// Выключение полностью снимает радио WiFi — фоновых расходов нет.
void web_set_enabled(bool enabled);
bool web_is_enabled();

// Имя AP с суффиксом efuse — показывается на экране Settings, чтобы с телефона
// было понятно, к какой сети подключаться.
String web_ap_ssid();

#endif
