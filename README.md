ESP32 Tuya Multi-Device Monitor + OLED UI
==========================================

This project replaces the stock ESP-IDF "hello world" with a small app that:
- Connects to Wi‑Fi and talks to multiple Tuya LAN devices (protocol 3.5).
- Maps six GPIO inputs to per-device ON/OFF commands.
- Shows status on a 128x32 SSD1306 OLED using a tiny 1bpp GFX layer.
- Periodically queries device state and shows online/offline rings per device.

Hardware Overview
-----------------
- Target: ESP32 (IDF, C++17).
- OLED: SSD1306 128x32, I2C address `0x3C`, SDA=GPIO21, SCL=GPIO22.
- Buttons (pull-up, falling-edge interrupt):
  - LED1 OFF = GPIO2, LED1 ON = GPIO13
  - LED2 OFF = GPIO27, LED2 ON = GPIO25
  - LED3 OFF = GPIO32, LED3 ON = GPIO19
- Tuya LAN devices: configured in `main/app_main.cpp` (`kTuyaDevices` list).

What It Does
------------
- Wi‑Fi STA using credentials from `main/wifi_cred.hpp`.
- For each Tuya device:
  - Maintains a LAN session, heartbeats, and DP queries.
  - Button presses send DP20 true/false to the matched device.
  - DP20 updates drive a ring on the OLED (filled = ON, outline = OFF).
  - If no activity for 30s, the device is marked offline, the ring hides, and the client is told to reconnect.
- OLED:
  - Shows connection text briefly (e.g., "Connecting WiFi", button actions).
  - Default view shows up to 3 rings across the screen; offline devices are hidden.
  - Auto-clears transient messages after 10s and returns to the ring view.

Project Layout
--------------
- `main/app_main.cpp`   — Wiring, Wi‑Fi, OLED, button/DP mappings, device spawning.
- `main/tuya_client.*`  — Tuya protocol 3.5 client (LAN socket, crypto, DP I/O).
- `main/ssd1306.*`      — OLED driver (I2C transport).
- `main/gfx.*`          — 1bpp framebuffer helpers (text, lines, circles).
- `main/bitmaps.*`      — Glyphs for the tiny font.
- `main/CMakeLists.txt` — Component registration and sources.
- `sdkconfig*`          — IDF config (provided).

Prerequisites
-------------
- ESP-IDF installed and exported in your shell.
- ESP32 board wired as described above.
- Tuya device info (ID, local key, IP, version 3.5).

Setup
-----
1) Wi‑Fi credentials  
   Edit `main/wifi_cred.hpp`:
   ```c++
   #define WIFI_SSID "your-ssid"
   #define WIFI_PASSWORD "your-password"
   ```

2) Tuya devices  
   Update `kTuyaDevices` in `main/app_main.cpp` with your devices:
   ```c++
   static TuyaDeviceConfig kTuyaDevices[] = {
       {"<id1>", "<local_key1>", "<ip1>", "3.5"},
       {"<id2>", "<local_key2>", "<ip2>", "3.5"},
       {"<id3>", "<local_key3>", "<ip3>", "3.5"},
   };
   ```
   Maintain the index order to match the LED button pairs above.

3) OLED wiring  
   - SDA → GPIO21, SCL → GPIO22, address 0x3C, 3.3V power. Keep I2C pull-ups in place.

4) Button wiring  
   - Use the six GPIOs listed in Hardware Overview. Internal pull-ups are enabled; use momentary buttons to ground.

Build & Flash
-------------
```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
Adjust the serial port as needed. On boot you should see an I2C scan log, Wi‑Fi connect, Tuya connects, and the OLED showing rings.

Using It
--------
- Press OFF/ON buttons to send DP20=false/true to the paired device.
- The OLED shows a transient "Turning ON/OFF" message, then returns to the ring view.
- Rings:
  - Filled circle = device DP20 true.
  - Outline circle = device DP20 false.
  - Hidden = no recent activity or device offline.
- Status refresh:
  - DP queries every minute.
  - If no activity for 30s, the client forces a reconnect and the ring hides until data arrives.

Troubleshooting
---------------
- OLED blank: confirm SDA/SCL pins (21/22), address 0x3C, and that `ssd1306` init logs succeed.
- No Tuya data: confirm device IPs and keys; ensure the devices are on the same LAN and support 3.5.
- Buttons ignored: check the GPIO mapping and that the buttons pull to GND (inputs have pull-ups).
- Frequent reconnects: indicates missed traffic; verify network stability and device availability.

Implementation Notes (Tuya 3.5 crypto/transport)
-------------------------------------------------
- Protocol: Tuya LAN “3.5” encrypted frames over TCP/6668.
- Session establishment:
  - Client generates a local nonce and sends it (command 3).
  - Device responds with its nonce; client HMACs it with the local key to derive the session key and completes with command 5.
  - Once established, the client uses a per-message IV (random bytes) and AES-128-GCM with the session key.
- Message layout:
  - Prefix `0x6699`, header with seq/command, payload length, IV (12 bytes), encrypted payload, GCM tag (16 bytes), suffix `0x9966`.
  - For DP writes, payload is JSON wrapped with protocol/version info; for DP queries, it’s the gwId/devId/uid/timestamp JSON.
- Crypto helpers (in `tuya_client.cpp`):
  - `aes_128_gcm_encrypt/decrypt` from mbedTLS for confidentiality/authenticity.
  - `hmac_sha256` for deriving the session key from the device’s response nonce + local key.
- State machine:
  - DISCONNECTED → CONNECTING (non-blocking connect) → NEGOTIATING (session) → CONNECTED.
  - Heartbeats every 5s; DP queries every minute; idle >30s triggers reconnect.
- DP handling:
  - Buttons enqueue DP20 bool commands to a per-device queue.
  - Incoming messages are decrypted and JSON scanned for DP20 to update the OLED state.
- OLED flush:
  - `GFX` framebuffer calls `OLED_GfxFlushCallback` which wraps the SSD1306 driver (I2C write with control byte).
- Watchdogs:
  - If no activity in 30s, mark device offline, hide the ring, and send a RESET to force reconnect.

License / Notes
---------------
This repo builds on ESP-IDF boilerplate. Tuya protocol implementation is minimal and tailored to local LAN devices on protocol 3.5.
