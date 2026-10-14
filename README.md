# Otto_Sensor

ESP32 (Heltec Wireless Stick v2) multi-sensor firmware: CO2, BME680/BSEC2, light, battery, LoRa, OLED, MQTT telemetry.

## Provisioning
On first boot (or when credentials fail) the device starts a setup AP `OttoSensor-<MAC>`; browse to `http://192.168.4.1` to enter Wi-Fi and MQTT credentials (stored in NVS, none in the firmware).

## OTA update
Attach `firmware.bin` to a GitHub release, then publish to `/ottosensors-dev/device/<imei>/command`:

```json
{"type":"ota","fw_version":"1.0.4","url":"https://github.com/<user>/Otto_Sensor/releases/download/v1.0.4/firmware.bin","md5":"<md5>"}
```

Build: `platformio run`
