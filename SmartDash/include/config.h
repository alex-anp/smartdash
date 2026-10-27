#ifndef CONFIG_H
#define CONFIG_H

// Board: ESP32-2424S012
// Display 240x240 round IPS - likely GC9A01
// Touch controller - TBD after board inspection

// Pin mapping - PLACEHOLDER, verify against actual ESP32-2424S012 board
// Common variants exist; adjust on arrival
#define TFT_CS   5
#define TFT_DC   4
#define TFT_RST  2
#define TFT_SDA  23  // MOSI
#define TFT_SCL  18  // SCK
#define TFT_BL   27  // backlight (if present)

// Touch I2C - PLACEHOLDER
#define TOUCH_SDA 21
#define TOUCH_SCL 22
#define TOUCH_INT 33

// Vehicle specific
#define VEHICLE_NAME "Honda Dunk AF78"
#define RPM_REDLINE 8000
#define RPM_MAX 10000  // adjust if needed

// Units
#define USE_KMH true
#define USE_CELSIUS true

// ELM
#define ELM_AUTO_CONNECT true
#define ELM_RECONNECT_INTERVAL 3000

// UI
#define SCREEN_CENTER_X 120
#define SCREEN_CENTER_Y 120
#define SCREEN_RADIUS 120

#endif
