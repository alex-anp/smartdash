# SmartDash Dev Notes

## Board
- ESP32-2424S012 (1.28" round 240x240 IPS) - pinout/touch driver TBD after receipt
- Link from order: https://ozon.ru/t/J5B9QgE

## Vehicle
- Honda Dunk AF78, 4-pin Honda diagnostic connector
- Adapter: Honda 4 pin -> OBD2 for ELM327
- Redline: 8000 RPM

## TODO on board arrival
1. Get pinout for display (SPI), backlight
2. Identify display driver (GC9A01 likely)
3. Test SPI display init
4. Test Bluetooth Classic + BLE with ELM327
5. Implement web config UI (AP mode)

## Libraries to consider (Arduino IDE)
- TFT_eSPI (configure User_Setup for GC9A01/board) OR Arduino_GFX OR LovyanGFX (good for round/circle UI)
- WebServer/WiFi (built-in ESP32)
- BluetoothSerial (built-in)
- BLE Serial / NimBLEArduino
- Preferences (built-in)
- ArduinoJson (for config)

## Wiring notes
- Power: Type-C
- BT to ELM327, auto-connect to last saved
- Config via built-in Wi-Fi AP web interface (http://192.168.4.1)
- UI: minimal 4 params on round display
