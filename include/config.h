#ifndef CONFIG_H
#define CONFIG_H

// Единый источник правды для пинов платы, геометрии экрана и параметров мотоцикла.
// Значения, которые потенциально могут настраиваться, лучше держать здесь, а не
// разбрасывать "магические числа" по модулям UI и BLE.
//
// Плата: JC3636W518C (ESP32-S3R8, 1.8" круглый IPS 360x360, ST77916 QSPI, CST816S touch)
// Пины из демо производителя: JC3636W518EN/2-Program Example/Demo_Arduino_V1.0/ST77916_LVGL_DEMO/pincfg.h

#define SCREEN_WIDTH  360
#define SCREEN_HEIGHT 360

// Поворот изображения в градусах. Панель ST77916 умеет только кратные 90°,
// поэтому остальные значения settings_normalize() округляет до ближайшего.
#define DISPLAY_ROTATION 0

// Serial.printf через USB CDC может блокировать задачу на сотни миллисекунд.
// Поэтому служебные логи по умолчанию отключены; включать только для отладки
// через -DSMARTDASH_SERIAL_DEBUG=1.
#ifndef SMARTDASH_SERIAL_DEBUG
#define SMARTDASH_SERIAL_DEBUG 0
#endif
#if SMARTDASH_SERIAL_DEBUG
#define SD_LOG(...)   Serial.printf(__VA_ARGS__)
#define SD_LOGln(...) Serial.println(__VA_ARGS__)
#else
#define SD_LOG(...)   do {} while (0)
#define SD_LOGln(...) do {} while (0)
#endif

// Дисплей QSPI (ST77916)
#define TFT_BLK  15
#define TFT_RST  47
#define TFT_CS   10
#define TFT_SCK  9
#define TFT_SDA0 11
#define TFT_SDA1 12
#define TFT_SDA2 13
#define TFT_SDA3 14

// Тач I2C (CST816S). Контроллер сидит на порту I2C 0; тач используется для
// свайпов и выбора адаптера ELM; настройки планируются на экран Settings.
#define TOUCH_PIN_NUM_I2C_SCL 8
#define TOUCH_PIN_NUM_I2C_SDA 7
#define TOUCH_PIN_NUM_INT     41
#define TOUCH_PIN_NUM_RST     40

// Кнопка
#define BTN_PIN 0

// Wi-Fi точка доступа для веб-настроек
#define WIFI_AP_SSID "SmartDash"
#define WIFI_AP_PASS "smartdash"

// Параметры мотоцикла
#define VEHICLE_NAME "Honda Dunk AF78"
#define RPM_REDLINE 8000
#define RPM_MAX 12000  // верхняя граница тахометра

// Единицы измерения. Сейчас рендер зафиксирован на km/h и °C; эти флаги
// фиксируют направление для будущего экрана настроек (пока не используются).
#define USE_KMH true
#define USE_CELSIUS true

// ELM. Автоподключение использует адрес из NVS; интервал переподключения в
// src/elm327.cpp задан отдельно, чтобы BLE-задача реагировала без вызова API.
#define ELM_AUTO_CONNECT true
#define ELM_RECONNECT_INTERVAL 3000

// Период сводки длительностей циклов в журнал (см. src/perf.cpp).
#define PERF_REPORT_MS 2000

// Геометрия UI
#define SCREEN_CENTER_X (SCREEN_WIDTH / 2)
#define SCREEN_CENTER_Y (SCREEN_HEIGHT / 2)
#define SCREEN_RADIUS   (SCREEN_WIDTH / 2)

#endif
