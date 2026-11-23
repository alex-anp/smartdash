# SmartDash Dev Notes

## Board
- JC3636W518C / ESP32-S3R8, 1.8" round 360x360 IPS, ST77916 QSPI, CST816S touch
- Vendor docs/examples: `JC3636W518EN/` (gitignored, local reference)
- Pinout from `JC3636W518EN/2-Program Example/Demo_Arduino_V1.0/ST77916_LVGL_DEMO/pincfg.h`

## Vehicle
- Honda Dunk AF78, 4-pin Honda diagnostic connector
- Adapter: Honda 4 pin -> OBD2 for ELM327
- Redline: 8000 RPM

## Key constraints
- ESP32-S3 has **no Bluetooth Classic** - ELM327 must be BLE compatible
- Display is QSPI ST77916 360x360, not SPI GC9A01

## TODO
1. Run vendor LVGL demo on board (Arduino 3.x, ESP32_Display_Panel 0.1.4, ESP32_IO_Expander 0.0.2, LVGL 8.4.x)
2. Confirm touch/controller and backlight behavior
3. Implement minimal dashboard UI on 360x360
4. Implement BLE ELM327 scan/connect + AT init
5. Implement web config UI (AP mode)

## Libraries to consider (Arduino IDE)
- ESP32_Display_Panel + ESP32_IO_Expander (vendor path)
- LVGL 8.4.x (vendor demo) or custom lightweight drawing
- NimBLE-Arduino / BLEDevice for BLE ELM327
- WiFi + WebServer (built-in)
- Preferences (built-in)
- ArduinoJson (for config)

## Wiring notes
- Power: Type-C
- BT to ELM327 over BLE, auto-connect to last saved
- Config via built-in Wi-Fi AP web interface (http://192.168.4.1)
- UI: minimal 4 params on round display
