# Otto Sensor — Software Architecture

## 1. System Overview

Otto Sensor is an ESP32-based multi-sensor firmware (Heltec Wireless Stick v2) that reads environmental sensors and publishes telemetry to **AWS IoT Core** over MQTT with mutual TLS. It supports remote commands, over-the-air (OTA) firmware updates, a Wi-Fi provisioning portal, a local web UI, and LoRa pager output. Current firmware version: **1.0.15**.

## 2. Hardware Platform

| Component | Interface | Purpose |
|---|---|---|
| Heltec Wireless Stick v2 (ESP32) | — | MCU, Wi-Fi, OLED display, LoRa radio |
| COZIR CO₂ sensor | UART (Serial2, 9600 baud, pins 32/17) | CO₂ ppm + temperature |
| BME680 (via BSEC2) | I²C (pins 13/21, 100 kHz) | Temp, humidity, pressure, altitude, IAQ/VOC |
| APDS9960 | I²C | Ambient + RGB light |
| ADC (pin 37) | Analog | Battery/input voltage (0–3.3 V) |
| Buzzer (pin 33, LEDC ch 3) | PWM | Audible CO₂ alert (≥800 ppm, 3 s) |
| PWM output (pin 25, LEDC ch 4) | PWM | Remote-controlled output (freq/duty) |
| Power switch (pin 12) | GPIO | Remote on/off control |
| LoRa (915 MHz, SF7, BW125, sync 0xCD) | SPI | Pager notification packets |

## 3. Repository / Module Structure

```
├── platformio.ini            Build config: board, AWS endpoint flag, embedded certs, lib deps
├── src/
│   ├── main.cpp   (~1,700 ln) Application core: all logic described below
│   ├── OTA.cpp / OTA.h        HTTPS firmware download + flash writer (ESP Update)
│   ├── Watchdog.cpp/.h        Watchdog timer wrapper (reset each loop)
│   ├── Debug.h                DBG_PRINT macros gated by -DDEBUG_LOG
│   └── images.h               OLED bitmap assets
├── lib/
│   ├── COZIR/                 CO₂ sensor UART driver
│   ├── PubSubClient/          MQTT client (vendored)
│   └── ESP32httpUpdate/       Legacy HTTP update helper (vendored)
├── certs/                     Git-ignored: AmazonRootCA1.pem, device cert/key
│                              (embedded into firmware via board_build.embed_txtfiles)
└── tools/update_notifier.py   Desktop OTA notifier (AWS IoT mutual-TLS client)
```

## 4. Firmware Architecture (main.cpp functional blocks)

### a) Identity & configuration
- `get_imei()` — derives device ID (IMEI) from MAC; used in MQTT topics and client ID.
- Credentials stored in NVS via `Preferences` (namespace `wifi`); fallback to compiled-in defaults.
- AWS IoT endpoint injected at build time (`-DAWS_IOT_ENDPOINT`); TLS certs embedded as binary symbols.

### b) Network & provisioning
- `setup_network()` — STA mode with WPA2/PMF-tolerant config (802.11b/g/n forced for router compatibility); on failure starts the captive portal.
- `startWiFiSetup()`, `handleWiFiSetupPage()`, `handleWiFiSave()`, `scanNetworksForPortal()` — AP-mode portal `OttoSensor-<MAC>` at 192.168.4.1 with DNS hijack; saves credentials to NVS.
- When connected, a `WebServer` on port 80 serves the config page and OTA pages; mDNS advertises `http://OttoSensors-<id>.local`.

### c) Cloud connectivity (MQTT)
- `mqtt_connect()` — mutual-TLS connect to AWS IoT (port 8883), LWT publishes `state:INACTIVE`, then publishes `state:ACTIVE` and subscribes to the command topic. 3 retries, then falls back to the setup portal.
- `checkConnections()` — non-blocking Wi-Fi/MQTT health check every 10 s (Wi-Fi retry every 15 s).
- Topics (per device):
  - `ottosensors/device/<imei>/command` (subscribe)
  - `ottosensors/device/<imei>/update` (telemetry publish)
  - `ottosensors/device/<imei>/state` (status/LWT publish)

### d) Command handling
- `receivedCallback()` parses JSON commands: `pwm` (freq/duty), `onoff` (power pin), `ota` (URL+MD5+version), `ota_check`, `ota_install`; anything else triggers a telemetry republish.

### e) OTA update system
- `runOta()` → `firmware_ota()` (OTA.cpp): streams the image over HTTP(S), verifies MD5, writes to the next OTA slot, reboots on success.
- `otaProgress()` reports progress via MQTT, OLED progress bar, and keeps the web UI responsive.
- `checkForUpdate()` polls the GitHub releases API for `ottospo/Otto_Sensor` every 6 h (or on `ota_check`); `isNewerVersion()` compares dotted versions.
- `handleUpdatePage/Install/Status` — browser-based manual update at `/update`.

### f) Sensor subsystem
- `initSensorCOZIR()` / `ReadSensorCozir()` — CO₂ + temp over UART; fresh-air calibration at boot.
- `initSensorBME680()` / `runBSEC()` / `newDataCallback()` / `saveBsecState()` — BSEC2 library with periodic state persistence to NVS; produces T/RH/pressure/altitude/IAQ with validity flags.
- `initSensorAPDS9960()` / `ReadSensorAPDS9960()` — RGB + clear light.
- `setupADC()` / `ReadVoltageBattery()` — battery voltage.
- `scanSensorI2CBus()` — boot-time I²C diagnostics.

### g) Telemetry & output
- `populateMeasurementJson()` — builds the JSON payload (temp, humidity, CO₂, VOC/IAQ, RGB/clear, pressure, altitude, voltage, PWM state, power).
- `get_data_to_send()` — publishes to the update topic.
- `sendCo2ToPager()` — LoRa packet transmission.
- `DisplayMeasurementData()` — OLED rendering.
- Buzzer alert logic: CO₂ ≥ 800 ppm → 3 s tone.

## 5. Runtime Model

- **`setup()`**: IMEI → Heltec/LoRa/OLED init → Serial2 → ADC → COZIR (+calibration) → I²C bus scan → APDS9960 → BME680/BSEC → PWM/power GPIO → network → MQTT.
- **`loop()`** (cooperative, non-blocking, watchdog-fed each iteration):
  1. If provisioning: service DNS + web portal only.
  2. Otherwise: web server, connection checks, `client.loop()`, pending OTA, 6-hourly update check, requested publishes.
  3. Every pass: BSEC, light sensor, OLED refresh.
  4. Every 5 s: CO₂, battery voltage, telemetry publish, LoRa pager, buzzer check.

## 6. Security & Credentials

- AWS IoT mutual TLS (root CA + device cert + private key embedded from git-ignored `certs/`).
- OTA image integrity via MD5 delivered in the authenticated MQTT command (HTTPS fetch uses `setInsecure`, relying on MD5 + command-channel auth).
- Wi-Fi credentials and BSEC state persisted in NVS (`Preferences`).

## 7. Key Dependencies

Heltec ESP32 Dev-Boards 1.1.2 · BSEC2 1.10.2610 · BME68x 1.3.40408 · Adafruit APDS9960 / Unified Sensor · ArduinoJson · PubSubClient (vendored).
