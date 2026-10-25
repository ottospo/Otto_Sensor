# Otto_Sensor

ESP32 (Heltec Wireless Stick v2) multi-sensor firmware: CO2, BME680/BSEC2, light, battery, LoRa, OLED, MQTT telemetry.

## Provisioning
On first boot (or when Wi-Fi credentials are missing or the connection fails) the device starts a setup AP `OttoSensor-<MAC>`; browse to `http://192.168.4.1` to enter Wi-Fi credentials. MQTT connects only to the AWS IoT endpoint configured by `AWS_IOT_ENDPOINT` in `platformio.ini`, using the device certificate and key embedded from `certs/`.

## OTA update
Attach `firmware.bin` to a GitHub release, then publish to `ottosensors/device/<imei>/command` in AWS IoT Core:

```json
{"type":"ota","fw_version":"1.0.14","url":"https://github.com/ottospo/Otto_Sensor/releases/download/1.0.14/firmware.bin","md5":"<md5>"}
```

Build: `platformio run`

The optional desktop update notifier also connects to AWS IoT Core with mutual TLS:

```powershell
python tools\update_notifier.py
```

It uses the PEM files in `certs/` by default. Set `AWS_IOT_CA_FILE`, `AWS_IOT_CERT_FILE`, and `AWS_IOT_KEY_FILE` to override their locations.
