#ifndef WEB_H
#define WEB_H

#include <Arduino.h>

// Wi-Fi access point and HTTP server. web_loop() must be called regularly from
// loop() so pending client requests are handled without blocking the UI.
void web_init();
void web_loop();

#endif
