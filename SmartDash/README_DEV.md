# SmartDash Dev Notes

## Board
- ESP32-2424S012 (1.28" round 240x240 IPS) - pinout/touch driver TBD after receipt
- Link from order: https://ozon.ru/t/J5B9QgE

## Vehicle
- Honda Dunk AF78, 4-pin Honda diagnostic connector
- Adapter: Honda 4 pin -> OBD2 for ELM327
- Redline: 8000 RPM

## TODO on board arrival
1. Get pinout for display (SPI), backlight, touch I2C/INT
2. Identify display driver (GC9A01 likely) and touch (CST816S likely)
3. Test SPI display init
4. Test touch
5. Test Bluetooth Classic + BLE with ELM327

## Libraries to consider (Arduino IDE)
- TFT_eSPI (configure User_Setup for GC9A01/board) OR Arduino_GFX
- LovyanGFX (good for round/circle UI)
- BluetoothSerial (built-in)
- BLE Serial / NimBLEArduino
- Preferences (built-in)
- ArduinoJson (if needed later)

## Wiring notes
- Power: Type-C
- BT to ELM327, auto-connect to last saved
- UI: touch only, minimal 4 params
