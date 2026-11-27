# SmartDash Dev Notes

## Board

- JC3636W518C / ESP32-S3R8, 1.8" round 360x360 IPS, ST77916 QSPI, CST816S touch
- Vendor docs/examples: `JC3636W518EN/` (gitignored, local reference)
- Pinout source: `JC3636W518EN/2-Program Example/Demo_Arduino_V1.0/ST77916_LVGL_DEMO/pincfg.h`

## Vehicle

- Honda Dunk AF78, 4-pin Honda diagnostic connector
- Adapter: Honda 4 pin -> OBD2 for ELM327
- Redline: 8000 RPM; tachometer full scale: 12000 RPM

## Key constraints

- ESP32-S3 has **no Bluetooth Classic** - ELM327 must be BLE compatible
- Display is QSPI ST77916 360x360, not SPI GC9A01
- `Serial.printf` over USB CDC can block ~800 ms, so runtime logs are disabled
  unless `-DSMARTDASH_SERIAL_DEBUG=1` is added to `build_flags`
- BLE scan/connect/AT state machine runs on core 0; UI + web run on core 1

## Current status

- [x] PlatformIO build/upload confirmed on board
- [x] Touch/controller and backlight behavior confirmed
- [x] Minimal dashboard UI implemented in `src/ui.cpp`
- [x] BLE scan/connect + AT init implemented in `src/elm327.cpp`
- [x] OBD polling: `010C0D` (RPM + speed in one response) as fast path; `0105` and `ATRV`
  alternating once per second (each parameter every 2 s); automatic fallback to separate
  `010C`/`010D`/`0105`/`ATRV` if multi-PID is unsupported
- [x] Display update: incremental RPM arc and dirty-rectangle sprite pushes instead of full-frame flush
- [x] One-shot startup sweep, no endless demo
- [x] Swipe up from Dash opens ELM Monitor, swipe down closes it, left/right toggles Dash <-> Drag; vertical swipes are ignored on Drag
- [x] ELM Monitor shows scan results while searching/connecting, and connected adapter
  info (name, address, address type, protocol, RSSI, uptime) once AT init is done;
  opening the screen does not start a scan, `Scan` button does
- [x] NVS storage for last connected ELM MAC, address type, name and drag results
- [x] Persistent ELM connection log in LittleFS (`log`, `log all`, `clear` over USB)
- [x] Drag screen: idle wait, speed-based launch, 10 km/h splits, demo ramp, per-target best, raw sample log
- [x] Wi-Fi AP started; HTTP root is still a placeholder
- [ ] Full web config forms (RPM limits, units, colors)
- [x] Test with a real ELM327: connection, AT init and data reception confirmed
- [x] `010C0D` multi-PID support used with automatic legacy fallback

## PlatformIO

- Env: `jc3636w518c` in `platformio.ini`
- Build: `~/.platformio/penv/bin/pio run`
- Upload: `~/.platformio/penv/bin/pio run -t upload`
- Port is auto-detected (Linux: `/dev/ttyACM0`)
- `pio device monitor` requires an interactive stdin; use a normal terminal
  monitor if output is piped/redirected

## Libraries

- LovyanGFX 1.3.x (display + touch)
- NimBLE-Arduino 2.5.x (BLE ELM327; 2.x avoids the core 3.x crash of NimBLE 1.4.3)
- WiFi + WebServer (built-in)
- Preferences (built-in)
- ArduinoJson 7.4.x (reserved for future config payloads)

## UI snapshot

- Outer RPM arc: continuous fill, radii 160..176, 150°..390°, zones 8k/10k
- Inner scale: ticks every 1000 rpm, labels every 2000 rpm, radii 138..150
- Speed: 0..99 seven-segment digits; 0..9 uses one centered digit
- Bottom: coolant color thresholds <60 / 60..90 / >90, voltage <11 / 11..12 / 12..15 / >15
- ELM status: three blinking dots while searching, `ELM OK` when connected
- ELM Monitor screen: scan list before connection; connected-adapter details after
  `ELM CONNECTED` (refreshed every 600 ms, so uptime ticks)
- Drag screen: fixed `DRAG MOD` title, status line between title and timer, line
  `speed - ss.cc` (live during the run, reached speed on finish), 10 rows
  (`km/h | time | split`). The run ends at the selected limit or on an RPM drop;
  taps are ignored. In race mode
  `elm327.cpp` polls only `010C0D` with a `millis()` timestamp; `DashData` snapshots
  are copied under a mutex so the UI never renders a half-updated sample.

## Wiring notes

- Power: Type-C (DC/DC 12 V -> 5 V on the bike)
- BT to ELM327 over BLE, auto-connect to last saved MAC
- Config via built-in Wi-Fi AP `SmartDash-<id>` / `smartdash` (http://192.168.4.1)
- UI: minimal 4 params on round display
