#ifndef WEB_H
#define WEB_H

#include <Arduino.h>

// Точка доступа Wi-Fi и HTTP-сервер. web_loop() нужно регулярно вызывать
// из loop(), чтобы запросы обрабатывались без блокировки UI.
void web_init();
void web_loop();

#endif
