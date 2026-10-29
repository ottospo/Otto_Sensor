#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include <Wire.h>
#include <SPI.h>
#include <Arduino.h>
#include "Watchdog.h"
#include "Debug.h"
#include <HTTPClient.h>
#include "OTA.h"
#include <ArduinoJson.h>
#include <heltec.h>
#include <cozir.h>
#include <Preferences.h>
#include <bsec2.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_APDS9960.h>

#ifndef SEALEVELPRESSURE_HPA
#define SEALEVELPRESSURE_HPA (1013.25)
#endif
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>

Preferences preferences;

String wifiSSID;
String wifiPass;


const char firmwareVersion[] = "1.0.15";
unsigned long lastSensorPublish = 0;
uint32_t pagerTxAt = 0;

Watchdog wdt;
HTTPClient http;

#define BAND 915E6
#define LORA_BANDWIDTH 125
#define LORA_SPREADING_FACTOR 7
#define LORA_CODING_RATE 5
#define LORA_SYNC_WORD 0xCD

/*PWM on speaker for LED (on board) or Buzzer control*/

const int BUZZPin = 33;       //Buzzer on Board
const int BUZZ_CHANNEL = 3;

const int outputPwmPin = 25;
const int outputPwmChannel = 4;

const int powerPin = 12;

const int pwmFreq = 1000;     // 1 kHz
const int pwmResolution = 8;  // 8-bit resolution

bool buzzerOn = false;
unsigned long buzzerStartTime = 0;
const unsigned long buzzerDuration = 3000; // 3 seconds

/* BME680 measured data */
float tf_BME;   // Temperature, Â°F
float tc_BME;   // Temperature, Â°C
float rh_BME;   // Relative humidity, %
float ap_BME;   // Atmospheric pressure
float alt_BME;  // Calculated altitude
bool bmeReadingsValid = false;
float iaq_BME = 0.0f;
uint8_t iaqAccuracy = 0;
bool iaqValid = false;
bool bsecReady = false;

/*COZIR measured data*/
uint16_t co2_CZR;
float temp_CZR;

/*APDS9960*/
Adafruit_APDS9960 apds;
bool apdsReady = false;
uint16_t ambient_light = 0;
uint16_t red_light = 0;
uint16_t green_light = 0;
uint16_t blue_light = 0;

/*Analog input 0-3.3V*/
const int potPin = 37;
double potValue;
float InVoltage;

uint32_t pwm_freq = 1000;
uint8_t pwm_duty = 50;
uint8_t power = 0;       // 0 = OFF, 1 = ON

COZIR czr(&Serial2);

const char ssid[] = "...";
const char pass[] = "...";

WebServer wifiServer(80);
DNSServer dnsServer;
bool wifiSetupMode = false;

#define HOSTNAME "OttoSensors"

// AWS IoT Core (mutual TLS). Endpoint comes from the AWS_IOT_ENDPOINT build flag;
// the root CA, device certificate and private key are embedded from certs/.
#ifndef AWS_IOT_ENDPOINT
#error "Define AWS_IOT_ENDPOINT in platformio.ini"
#endif
constexpr uint16_t mqttPort = 8883;

extern const char aws_root_ca_start[] asm("_binary_certs_AmazonRootCA1_pem_start");
extern const char aws_device_cert_start[] asm("_binary_certs_device_cert_pem_start");
extern const char aws_device_key_start[] asm("_binary_certs_device_private_key_start");

char MQTT_SUB_TOPIC[100];
char MQTT_PUB_TOPIC[100];
char MQTT_PUB_TOPIC_DEVICE[100];


WiFiClientSecure net;
PubSubClient client(net);

String imei;

TwoWire I2C_SENSORS = TwoWire(1);
Bsec2 bme;

const uint8_t bsecConfig[] = {
#include <config/bme680/bme680_iaq_33v_3s_4d/bsec_iaq.txt>
};

Preferences bsecStorage;
bool bsecStorageReady = false;
bool bsecStateSaved = false;

uint32_t lastBsecStateSave = 0;
const uint32_t BSEC_STATE_SAVE_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
TwoWire &I2C_APDS = I2C_SENSORS;

void receivedCallback(char *topic, byte *payload, unsigned int length);
void newDataCallback(const bme68xData data, const bsecOutputs outputs, const Bsec2 bsec);
void startWiFiSetup();
String mdnsHostName()
{
  String m = imei;
  m.replace(":", "");
  m.toLowerCase();
  return "ottosensor-" + m.substring(m.length() - 6);
}
static bool isNewerVersion(String candidate, String current);
void handleUpdatePage();
void handleUpdateInstall();
void handleUpdateStatus();

void get_imei()
{
  /*The chip ID is essentially its MAC address(length: 6 bytes)*/
  imei = WiFi.macAddress();
  Serial.println(imei);
}

void mqtt_connect()
{
  snprintf(
    MQTT_SUB_TOPIC,
    sizeof(MQTT_SUB_TOPIC),
    "ottosensors/device/%s/command",
    imei.c_str()
);

snprintf(
    MQTT_PUB_TOPIC,
    sizeof(MQTT_PUB_TOPIC),
    "ottosensors/device/%s/update",
    imei.c_str()
);

snprintf(
    MQTT_PUB_TOPIC_DEVICE,
    sizeof(MQTT_PUB_TOPIC_DEVICE),
    "ottosensors/device/%s/state",
    imei.c_str()
);

char mqttClientId[40];

snprintf(
    mqttClientId,
    sizeof(mqttClientId),
    "OttoSensor-%s",
    imei.c_str()
);
  int retryCount = 0;
  while (!client.connected() && WiFi.status() == WL_CONNECTED)
  {
    wdt.reset();
    char jsonBuffer[128];
    DynamicJsonDocument ottoDataObj(128);

    jsonBuffer[0] = '\0';

    ottoDataObj["imei"] = imei;
    ottoDataObj["state"] = "INACTIVE";
    ottoDataObj["fw_version"] = firmwareVersion;

    serializeJson(ottoDataObj, jsonBuffer);

    Serial.println(jsonBuffer);   
    Serial.print("Attempting AWS IoT connection as: ");
    Serial.println(mqttClientId);

    if (client.connect(
          mqttClientId,
          nullptr,
          nullptr,
          MQTT_PUB_TOPIC_DEVICE,
          1,
          false,
          jsonBuffer))
    {
      Serial.println("Connected MQTT");
      ottoDataObj["state"] = "ACTIVE";
      jsonBuffer[0] = '\0';
      serializeJson(ottoDataObj, jsonBuffer);

      Serial.println(jsonBuffer);
      Serial.print("Publishing telemetry to: ");
      Serial.println(MQTT_PUB_TOPIC); 
      client.publish(MQTT_PUB_TOPIC, jsonBuffer);

      client.subscribe(MQTT_SUB_TOPIC);
      Serial.print("Subscribed to: ");
      Serial.println(MQTT_SUB_TOPIC);
      break;
    }
    else
    {
      Serial.print("failed, status code = ");
      Serial.println(client.state());
      retryCount++;

      if (retryCount >= 3)
      {
        Serial.println("MQTT connection failed 3 times. Please check credentials in setup portal.");
        if (!wifiSetupMode)
        {
          startWiFiSetup();
        }
        break;
      }

      Serial.println("try again in 1 second");
      
      delay(1000);  /* Wait 1 seconds before retrying */
    }
  }
  
}
const char PORTAL_CSS[] =
  "<style>"
  "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
  "background:#f0f2f5;font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;color:#1c1e21;padding:20px 0;box-sizing:border-box}"
  ".card{box-sizing:border-box;width:92%;max-width:400px;background:#fff;padding:26px 22px;"
  "border-radius:12px;box-shadow:0 4px 18px rgba(0,0,0,.15);margin:auto}"
  "h2{margin:0 0 6px;font-size:22px;text-align:center}"
  "h3{margin:0 0 4px;font-size:16px}"
  ".sub{margin:0 0 16px;text-align:center;color:#65676b;font-size:14px}"
  "label{display:block;margin:12px 0 4px;font-size:13px;font-weight:600;color:#444}"
  "input,select{box-sizing:border-box;width:100%;padding:10px 12px;font-size:15px;border:1px solid #ccd0d5;"
  "border-radius:8px;background:#fff}"
  "input:focus,select:focus{outline:none;border-color:#1a73e8;box-shadow:0 0 0 3px rgba(26,115,232,.2)}"
  "button{width:100%;margin-top:22px;padding:12px;font-size:16px;font-weight:600;color:#fff;"
  "background:#1a73e8;border:0;border-radius:8px;cursor:pointer}"
  "button:active{background:#1558b0}"
  ".show-pw{display:flex;align-items:center;margin-top:6px;font-size:13px;color:#555;cursor:pointer}"
  ".show-pw input{width:auto;margin:0 8px 0 0;cursor:pointer}"
  ".ssid{text-align:center;font-size:15px;font-weight:700;word-break:break-all;margin:6px 0}"
  ".note{text-align:center;color:#65676b;font-size:13px;margin-top:16px}"
  "hr{margin:20px 0 14px;border:none;border-top:1px solid #e4e6eb}"
  "</style>";

String ssidOptionsHtml;
String htmlEscape(const String &in)
{
  String out;
  for (size_t i = 0; i < in.length(); ++i)
  {
    char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

void handleWiFiSave()
{
  Serial.println("*** SETUP SAVE REQUEST RECEIVED ***");

  String newSSID = wifiServer.hasArg("custom_ssid") ? wifiServer.arg("custom_ssid") : "";
  newSSID.trim();
  if (newSSID.length() == 0 && wifiServer.hasArg("ssid"))
  {
    newSSID = wifiServer.arg("ssid");
    newSSID.trim();
  }
  String newPass = wifiServer.hasArg("password") ? wifiServer.arg("password") : "";

  // Save WiFi if valid SSID provided
  if (newSSID.length() > 0 && newSSID != "__other")
  {
    Serial.print("Saving WiFi SSID: ");
    Serial.println(newSSID);
    Serial.print("Saving WiFi password length: ");
    Serial.println(newPass.length());

    preferences.begin("wifi", false);
    preferences.putString("ssid", newSSID);
    if (newPass.length() > 0)
    {
      preferences.putString("pass", newPass);
    }
    preferences.end();
  }

  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>OttoSensor Setup</title>" +
    String(PORTAL_CSS) +
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h2>Settings Saved</h2>"
    "<p class='sub'>OttoSensor will restart and connect with new credentials.</p>";

  if (newSSID.length() > 0 && newSSID != "__other")
  {
    html += "<p class='ssid'>WiFi: " + htmlEscape(newSSID) + "</p>";
  }
  html +=
    "<p class='note'>In case your computer does not automatically reconnect, please reconnect manually.</p>"
    "</div>"
    "</body>"
    "</html>";

  wifiServer.send(200, "text/html", html);

  delay(2000);

  ESP.restart();
}

// Scan before the AP starts so the scan does not disturb connected clients
void scanNetworksForPortal()
{
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(200);
  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();

  ssidOptionsHtml = "";
  for (int i = 0; i < n; ++i)
  {
    String name = WiFi.SSID(i);
    if (name.length() == 0)
    {
      continue;
    }
    bool duplicate = false;
    for (int j = 0; j < i; ++j)
    {
      if (WiFi.SSID(j) == name)
      {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
    {
      continue;
    }
    // Results are sorted strongest first
    ssidOptionsHtml += "<option value=\"" + htmlEscape(name) + "\">" + htmlEscape(name) +
                       " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  Serial.print("Networks found: ");
  Serial.println(n);
}

void handleWiFiSetupPage()
{
  Serial.println("*** HTTP REQUEST RECEIVED ***");

  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>OttoSensor Setup</title>" +
    String(PORTAL_CSS) +
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h2>OttoSensor Setup</h2>"
    "<p class='sub'>Configure Wi-Fi and the AWS IoT endpoint.</p>"
    "<p class='sub'><b>Note:</b> Connect to Gateway Port at 2.4GHz</p>"
    "<form action='/save' method='POST'>"
    "<label for='ssidSelect'>WiFi network (gateway)</label>"
    "<select id='ssidSelect' name='ssid' required>"
    "<option value='' selected disabled>Select a network...</option>" + ssidOptionsHtml +
    "<option value='__other'>Other (type name)...</option></select>"
    "<input id='customSsid' type='text' name='custom_ssid' placeholder='Network name' "
    "autocapitalize='none' autocomplete='off' style='display:none;margin-top:8px'>"
    "<label for='pw'>WiFi Password</label>"
    "<input id='pw' type='password' name='password' placeholder='Enter Wi-Fi password' autocomplete='off'>"
    "<label class='show-pw'><input type='checkbox' id='togglePw'>Show Wi-Fi password</label>"
    "<hr>"
    "<h3>AWS IoT Core</h3>"
    "<p class='sub' style='margin-bottom:10px;'>The AWS IoT endpoint and device certificate are fixed in this firmware.</p>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form>"
    "<p style='text-align:center'><a href='/update'>Check firmware update</a></p>"
    "</div>"
    "<script>"
    "var s=document.getElementById('ssidSelect'),t=document.getElementById('customSsid'),p=document.getElementById('pw'),ck=document.getElementById('togglePw');"
    "function sync(){if(s.value=='__other'){t.style.display='block';t.required=true;t.focus();}"
    "else{t.value='';t.style.display='none';t.required=false;}}"
    "s.onchange=sync;sync();"
    "ck.onchange=function(){p.type=ck.checked?'text':'password';};"
    "</script>"
    "</body>"
    "</html>";

  wifiServer.send(200, "text/html", html);
}

void startWiFiSetup()
{
  WiFi.disconnect(true);
  delay(500);
  scanNetworksForPortal();
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  delay(500);
  String apName = "OttoSensor-";

  // Use last 5 characters of the device ID, removing ":"
  String suffix = imei;
  suffix.replace(":", "");

  if (suffix.length() > 6)
  {
    suffix = suffix.substring(suffix.length() - 6);
  }

  apName += suffix;
  String apPassword = "otto" + suffix;

  Serial.print("Starting WiFi setup AP: ");
  Serial.println(apName);
  // TEMPORARY (testing only): remove this print before release
  Serial.print("Setup AP password: ");
  Serial.println(apPassword);

 if (WiFi.softAP(apName.c_str(), apPassword.c_str()))
{
  Serial.print("Setup AP IP address: ");
  Serial.println(WiFi.softAPIP());

 wifiServer.on("/", HTTP_GET, handleWiFiSetupPage);
wifiServer.on("/save", HTTP_POST, handleWiFiSave);

// Captive portal: answer all DNS lookups with our IP and redirect
// unknown URLs (OS connectivity checks) to the setup page
dnsServer.start(53, "*", WiFi.softAPIP());
wifiServer.onNotFound([]() {
  wifiServer.sendHeader("Location", "http://192.168.4.1/", true);
  wifiServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  wifiServer.send(302, "text/html", "<p>Redirecting to <a href='http://192.168.4.1/'>setup</a>...</p>");
});

wifiServer.begin();

  wifiSetupMode = true;

  Serial.println("WiFi setup web server started");
}
else
{
  Serial.println("FAILED to start WiFi setup AP");
}

}

void setup_network()
{
  // Read WiFi credentials saved by the setup portal
  preferences.begin("wifi", false);

  wifiSSID = preferences.getString("ssid", "");
  wifiPass = preferences.getString("pass", "");

  preferences.end();

  // TEMPORARY fallback to the hard-coded credentials.
  // We will remove this after provisioning is proven.
  if (wifiSSID.length() == 0)
  {
    Serial.println("No saved WiFi credentials - using firmware defaults");

    wifiSSID = ssid;
    wifiPass = pass;
  }
  else
  {
    Serial.println("Using saved WiFi credentials");
  }

  Serial.print("Attempting to connect to SSID: ");
  Serial.println(wifiSSID);
  Serial.print("Password length: ");
  Serial.println(wifiPass.length());

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      Serial.print(" [WiFi Event] Disconnected, reason: ");
      Serial.println(info.wifi_sta_disconnected.reason);
    }
  });

  WiFi.disconnect(true);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // Configure Wi-Fi radio for compatibility with modern router transition modes (e.g. T-Mobile)
  // Reason 202 (AUTH_FAIL) often happens when router uses PMF (Protected Management Frames) or 802.11ax/ac features.
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  strncpy((char *)conf.sta.ssid, wifiSSID.c_str(), sizeof(conf.sta.ssid) - 1);
  strncpy((char *)conf.sta.password, wifiPass.c_str(), sizeof(conf.sta.password) - 1);
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;
  conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_config(WIFI_IF_STA, &conf);

  WiFi.begin(
    wifiSSID.c_str(),
    wifiPass.c_str()
  );

  unsigned long connectionStart = millis();
  unsigned long timeoutMs = (wifiPass.length() == 0) ? 5000 : 30000;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - connectionStart < timeoutMs)
  {
    Serial.print(".");
    delay(1000);
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("Connected WIFI ");
    Serial.println(wifiSSID);

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Allow accessing configuration web page directly on local network IP
    wifiServer.on("/", HTTP_GET, handleWiFiSetupPage);
    wifiServer.on("/save", HTTP_POST, handleWiFiSave);
    wifiServer.on("/update", HTTP_GET, handleUpdatePage);
    wifiServer.on("/update", HTTP_POST, handleUpdateInstall);
    wifiServer.on("/update/status", HTTP_GET, handleUpdateStatus);
    wifiServer.begin();
    Serial.println("Web configuration server started on local IP");
    if (MDNS.begin(mdnsHostName().c_str()))
    {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS: http://" + mdnsHostName() + ".local");
    }
  }
  else
  {
    Serial.print("WiFi connection failed, status=");
    Serial.println(WiFi.status());

    startWiFiSetup();
  }

  net.setCACert(aws_root_ca_start);
  net.setCertificate(aws_device_cert_start);
  net.setPrivateKey(aws_device_key_start);
  Serial.print("Connecting to AWS IoT endpoint: ");
  Serial.println(AWS_IOT_ENDPOINT);
  client.setServer(AWS_IOT_ENDPOINT, mqttPort);
  client.setCallback(receivedCallback);
}

volatile bool publishRequested = false;
bool otaPending = false;
String otaUrl;
String otaMd5;
String otaVersion;
bool updateAvailable = false;
bool updateCheckRequested = true;
String latestVersion;
String latestUrl;
StaticJsonDocument<512> jsonDocRx;
char jsonBufferRx[512];

void receivedCallback(char *topic, byte *payload, unsigned int length)
{
  const size_t copyLength = min(static_cast<size_t>(length), sizeof(jsonBufferRx) - 1);

  Serial.print("Received [");
  Serial.print(topic);
  Serial.print("]: ");
  memcpy(jsonBufferRx, payload, copyLength);
  jsonBufferRx[copyLength] = '\0';
  Serial.println(jsonBufferRx);

  deserializeJson(jsonDocRx, jsonBufferRx);
  String messageType = jsonDocRx["type"] | "";

  if (messageType == "pwm")
  {
    const uint32_t requestedFrequency = jsonDocRx["pwm_freq"] | pwm_freq;
    if (requestedFrequency > 0)
    {
      pwm_freq = requestedFrequency;
    }
    const int requestedDuty = jsonDocRx["pwm_duty"] | static_cast<int>(pwm_duty);
    pwm_duty = static_cast<uint8_t>(requestedDuty < 0 ? 0 : requestedDuty > 100 ? 100 : requestedDuty);
    ledcSetup(outputPwmChannel, pwm_freq, pwmResolution);
    ledcWrite(outputPwmChannel, static_cast<uint32_t>(pwm_duty) * 255U / 100U);
  }
  else if (messageType == "onoff")
  {
    JsonVariantConst value = jsonDocRx["power"];
    if (value.is<const char *>())
    {
      power = atoi(value.as<const char *>()) != 0 ? 1 : 0;
    }
    else
    {
      power = (value.is<bool>() ? value.as<bool>() : value.as<int>() != 0) ? 1 : 0;
    }
    digitalWrite(powerPin, power ? HIGH : LOW);
  }
  else if (messageType == "ota")
  {
    // {"type":"ota","fw_version":"1.0.3","url":"https://host/fw.bin","md5":"<32 hex>"}
    String ver = jsonDocRx["fw_version"] | "";
    String url = jsonDocRx["url"] | "";
    if (url.length() > 0 && ver != firmwareVersion && (isNewerVersion(ver, firmwareVersion) || (jsonDocRx["force"] | false)))
    {
      otaVersion = ver;
      otaUrl = url;
      otaMd5 = jsonDocRx["md5"] | "";
      otaPending = true;
    }
    else
    {
      Serial.println("OTA ignored: missing url or same version");
    }
    return;
  }
  else if (messageType == "ota_check")
  {
    updateCheckRequested = true;
    return;
  }
  else if (messageType == "ota_install")
  {
    if (updateAvailable)
    {
      otaVersion = latestVersion;
      otaUrl = latestUrl;
      otaMd5 = "";
      otaPending = true;
    }
    return;
  }
  publishRequested = true;
}

void publishOtaStatus(const char *status, int percent = -1)
{
  char buf[300];
  String host = mdnsHostName();
  snprintf(buf, sizeof(buf),
           "{\"imei\":\"%s\",\"state\":\"%s\",\"fw_version\":\"%s\",\"progress\":%d,\"new_version\":\"%s\",\"ip\":\"%s\",\"url\":\"http://%s.local/update\",\"url_ip\":\"http://%s/update\"}",
           imei.c_str(), status, firmwareVersion, percent, latestVersion.c_str(),
           WiFi.localIP().toString().c_str(), host.c_str(), WiFi.localIP().toString().c_str());
  client.publish(MQTT_PUB_TOPIC_DEVICE, buf);
  client.loop();
}

static int otaPercent = 0;
static const char *otaState = "idle"; // idle, running, done, failed
static String otaError;

void otaProgress(int percent)
{
  wdt.reset();
  otaPercent = percent;
  wifiServer.handleClient(); // keep the browser progress page responsive during the download
  static int lastShown = -1;
  if (percent / 5 == lastShown)
    return;
  lastShown = percent / 5;
  Serial.printf("OTA %d%%\n", percent);
  publishOtaStatus("OTA_PROGRESS", percent);
  Heltec.display->clear();
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->drawString(0, 0, "OTA update");
  Heltec.display->drawString(0, 12, String(percent) + "%");
  Heltec.display->drawProgressBar(0, 26, 62, 5, percent);
  Heltec.display->display();
}

void runOta()
{
  otaPending = false;
  Serial.println("Starting OTA to " + otaVersion);
  otaPercent = 0;
  otaError = "";
  otaState = "running";
  publishOtaStatus("OTA_START", 0);
  String err = firmware_ota(otaUrl, otaMd5, otaProgress);
  if (err.length() == 0)
  {
    otaPercent = 100;
    otaState = "done";
    publishOtaStatus("OTA_OK", 100);
    // Give the browser time to fetch the "done" status before restarting
    for (int i = 0; i < 30; i++)
    {
      wifiServer.handleClient();
      delay(100);
    }
    ESP.restart();
  }
  Serial.println("OTA failed: " + err);
  otaError = err;
  otaState = "failed";
  publishOtaStatus("OTA_FAILED");
}

static const char UPDATE_REPO[] = "ottospo/Otto_Sensor";

// Compares dotted versions ("1.0.10" > "1.0.9"); a leading 'v' is ignored.
static bool isNewerVersion(String candidate, String current)
{
  if (candidate.startsWith("v") || candidate.startsWith("V")) candidate.remove(0, 1);
  if (current.startsWith("v") || current.startsWith("V")) current.remove(0, 1);
  for (int i = 0; i < 3; i++)
  {
    int a = candidate.toInt();
    int b = current.toInt();
    if (a != b) return a > b;
    int ia = candidate.indexOf('.');
    int ib = current.indexOf('.');
    candidate = ia < 0 ? "0" : candidate.substring(ia + 1);
    current = ib < 0 ? "0" : current.substring(ib + 1);
  }
  return false;
}

void checkForUpdate()
{
  updateCheckRequested = false;
  if (WiFi.status() != WL_CONNECTED)
    return;

  WiFiClientSecure secure;
  secure.setInsecure();
  HTTPClient h;
  h.useHTTP10(true);
  h.setTimeout(10000);
  h.addHeader("User-Agent", "OttoSensor");
  String api = String("https://api.github.com/repos/") + UPDATE_REPO + "/releases/latest";
  if (!h.begin(secure, api))
    return;
  int code = h.GET();
  if (code != 200)
  {
    Serial.printf("Update check failed, http %d\n", code);
    h.end();
    return;
  }

  StaticJsonDocument<256> filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  DynamicJsonDocument doc(2048);
  DeserializationError e = deserializeJson(doc, h.getStream(), DeserializationOption::Filter(filter));
  h.end();
  if (e)
  {
    Serial.println("Update check parse error");
    return;
  }

  String tag = doc["tag_name"] | "";
  String url;
  for (JsonObject a : doc["assets"].as<JsonArray>())
  {
    if (String(a["name"] | "").endsWith(".bin"))
    {
      url = String(a["browser_download_url"] | "");
      break;
    }
  }
  if (tag.length() && url.length() && isNewerVersion(tag, firmwareVersion))
  {
    latestVersion = tag.startsWith("v") ? tag.substring(1) : tag;
    latestUrl = url;
    updateAvailable = true;
    Serial.println("Update available: " + latestVersion);
    publishOtaStatus("UPDATE_AVAILABLE");
  }
  else
  {
    updateAvailable = false;
    Serial.println("Firmware is up to date");
    publishOtaStatus("UP_TO_DATE");
  }
}

void handleUpdatePage()
{
  String body = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  body += PORTAL_CSS;
  body += "</head><body><div class='card'><h2>Otto Sensor firmware</h2><p>Installed: <b>";
  body += firmwareVersion;
  body += "</b></p>";
  if (updateAvailable)
  {
    body += "<p>New version available: <b>" + htmlEscape(latestVersion) + "</b></p>"
            "<form method='POST' action='/update'><button type='submit'>Install update</button></form>"
            "<p>The device restarts when finished. Do not power it off.</p>";
  }
  else
  {
    body += "<p>You are up to date.</p>";
  }
  // Remember where the user came from (e.g. a dashboard) so the progress page can return there
  body += "</div><script>if(document.referrer&&document.referrer.indexOf(location.host)<0)"
          "sessionStorage.setItem('otoRet',document.referrer);</script></body></html>";
  wifiServer.send(200, "text/html", body);
}

void handleUpdateStatus()
{
  String json = String("{\"state\":\"") + otaState + "\",\"percent\":" + otaPercent +
                ",\"error\":\"" + htmlEscape(otaError) + "\"}";
  wifiServer.sendHeader("Cache-Control", "no-store");
  wifiServer.send(200, "application/json", json);
}

void handleUpdateInstall()
{
  if (updateAvailable)
  {
    otaVersion = latestVersion;
    otaUrl = latestUrl;
    otaMd5 = "";
    otaPending = true;
    otaState = "running";
    otaPercent = 0;
    otaError = "";

    String body =
      "<html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>";

    body += PORTAL_CSS;

    body +=
      "<style>"
      ".status{text-align:center;margin-bottom:22px}"
      ".version{font-size:18px;margin:18px 0}"
      ".version strong{font-size:22px}"
      ".bar{height:18px;background:#e3e6ea;border-radius:10px;"
      "overflow:hidden;margin:22px 0 10px}"
      ".fill{height:100%;width:0;background:#2e7d32;"
      "transition:width .4s ease}"
      ".pct{font-size:28px;font-weight:700;margin:8px 0}"
      ".note{margin-top:24px;font-size:15px;opacity:.8}"
      ".success{font-size:22px;font-weight:700;color:#2e7d32}"
      ".return{display:inline-block;margin-top:20px;padding:12px 22px;"
      "background:#2878e5;color:white;text-decoration:none;"
      "border-radius:8px;font-weight:600}"
      "</style>"
      "</head><body>"

      "<div class='card'>"

      "<div class='status'>"
      "<h2>Otto Sensor Firmware Update</h2>"

      "<div class='version'>"
      "Installing <strong>" + otaVersion + "</strong>"
      "</div>"

      "<p id='msg'>Downloading firmware...</p>"

      "<div class='bar'>"
      "<div class='fill' id='f'></div>"
      "</div>"

      "<div class='pct' id='pct'>0%</div>"

      "<p class='note'>"
      "Keep the sensor powered on and connected to Wi-Fi.<br>"
      "The sensor will restart automatically when the update is complete."
      "</p>"

      "</div>"
      "</div>"

      "<script>"
      "var f=document.getElementById('f'),"
      "m=document.getElementById('msg'),"
      "p=document.getElementById('pct'),"
      "restarting=false,fails=0;"

      "function setP(v){"
      "if(v<0)v=0;"
      "if(v>100)v=100;"
      "f.style.width=v+'%';"
      "p.textContent=v+'%';"
      "}"

      "function finish(){"
      "setP(100);"
      "m.className='success';"
      "m.textContent='Update successful! Sensor restarted.';"
      "p.textContent='100%';"

      "var r=sessionStorage.getItem('otoRet');"
      "if(r){"
      "var a=document.createElement('a');"
      "a.href=r;"
      "a.textContent='Return to dashboard';"
      "a.className='return';"
      "document.querySelector('.status').appendChild(a);"
      "}"
      "}"

      "function tick(){"
      "fetch('/update/status',{cache:'no-store'})"
      ".then(function(r){return r.json();})"
      ".then(function(s){"

      "fails=0;"

      "if(restarting){"
      "finish();"
      "return;"
      "}"

      "if(s.state=='failed'){"
      "m.textContent='Update failed: '+s.error;"
      "m.style.color='#c62828';"
      "f.style.background='#c62828';"
      "return;"
      "}"

      "setP(s.percent);"

      "if(s.state=='done'){"
      "setP(100);"
      "m.textContent='Firmware installed. Restarting sensor...';"
      "restarting=true;"
      "}"
      "else if(s.state=='idle'){"
      "finish();"
      "return;"
      "}"
      "else{"
      "m.textContent='Installing firmware...';"
      "}"

      "setTimeout(tick,restarting?2000:700);"
      "})"
      ".catch(function(){"
      "if(restarting||++fails>3){"
      "restarting=true;"
      "setP(100);"
      "m.textContent='Firmware installed. Waiting for sensor to restart...';"
      "}"
      "setTimeout(tick,2000);"
      "});"
      "}"

      "tick();"
      "</script>"
      "</body></html>";

    wifiServer.send(200, "text/html", body);
  }
  else
  {
    wifiServer.sendHeader("Location", "/update");
    wifiServer.send(303);
  }
}

void initSensorCOZIR()
{
  Serial2.begin(9600);
  czr.SetOperatingMode(CZR_POLLING);
  delay(100);
  czr.SetOutputFields(CZR_RAWTEMP | CZR_RAWCO2);
  czr.SetDigiFilter(32);
}

void scanSensorI2CBus()
{
  DBG_PRINTLN("Scanning sensor I2C bus...");
  for (uint8_t address = 1; address < 127; ++address)
  {
    I2C_SENSORS.beginTransmission(address);
    if (I2C_SENSORS.endTransmission() == 0)
    {
      DBG_PRINTF("I2C device found at 0x%02X\n", address);
    }
  }
}

void ReadSensorCozir(void)
{
  uint32_t c = czr.CO2(); // most sensors report PPM directly (factor = 1)
  co2_CZR = c;
  DBG_PRINT("CO2 =\t");
  DBG_PRINTLN(c);
  temp_CZR = czr.Celsius();
  DBG_PRINT("czr.temp =\t");
  DBG_PRINTLN(temp_CZR,1);
}

void initSensorBME680()
{
  Serial.println("Init-I BME680 with BSEC2");
  bool sensorFound = false;
  const uint8_t addresses[] = {BME68X_I2C_ADDR_HIGH, BME68X_I2C_ADDR_LOW};
  for (uint8_t address : addresses)
  {
    if (bme.begin(address, I2C_SENSORS))
    {
      sensorFound = true;
      Serial.print("BME680 found at I2C address 0x");
      Serial.println(address, HEX);
      break;
    }
    Serial.print("No BME680/BSEC2 sensor at I2C address 0x");
    Serial.print(address, HEX);
    Serial.print("; BSEC status=");
    Serial.print(bme.status);
    Serial.print(", sensor status=");
    Serial.println(bme.sensor.status);
  }

  if (!sensorFound)
  {
    Serial.println("BSEC AQI unavailable: BME680 was not found at 0x76 or 0x77");
    return;
  }

  if (!bme.setConfig(bsecConfig))
  {
    Serial.print("Could not load BME680 3.3V / 3-second BSEC configuration; status=");
    Serial.println(bme.status);
    return;
  }

  if (bsecStorage.begin("ottoBsec", false))
  {
    bsecStorageReady = true;
    if (bsecStorage.isKey("state") &&
        bsecStorage.getBytesLength("state") == BSEC_MAX_STATE_BLOB_SIZE)
    {
      uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
      if (bsecStorage.getBytes("state", state, sizeof(state)) == sizeof(state) &&
          bme.setState(state))
      {
        Serial.println("Restored BSEC calibration state");
      }
      else
      {
        Serial.println("Could not restore saved BSEC calibration state");
      }
    }
    else
    {
      Serial.println("No saved BSEC calibration state; calibration will start fresh");
    }
  }
  else
  {
    Serial.println("Could not open BSEC state storage; calibration will not persist");
  }

  bsecSensor sensorList[] = {
      BSEC_OUTPUT_IAQ,
      BSEC_OUTPUT_RAW_TEMPERATURE,
      BSEC_OUTPUT_RAW_HUMIDITY,
      BSEC_OUTPUT_RAW_PRESSURE};

  if (!bme.updateSubscription(sensorList, ARRAY_LEN(sensorList), BSEC_SAMPLE_RATE_LP))
  {
    if (bme.status < BSEC_OK)
    {
      Serial.print("Could not subscribe to BSEC AQI outputs; status=");
      Serial.println(bme.status);
      return;
    }
    Serial.print("BSEC subscription warning (continuing): ");
    Serial.println(bme.status);
  }

  bme.attachCallback(newDataCallback);
  bsecReady = true;
  Serial.println("BME680/BSEC2 initialized");
}

void saveBsecState(Bsec2 sensor)
{
  if (!bsecStorageReady || iaqAccuracy < 2)
  {
    return;
  }

  const uint32_t now = millis();
  if (bsecStateSaved && now - lastBsecStateSave < BSEC_STATE_SAVE_INTERVAL_MS)
  {
    return;
  }

  uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
  if (!sensor.getState(state))
  {
    Serial.println("Could not read BSEC calibration state");
    return;
  }

  if (bsecStorage.putBytes("state", state, sizeof(state)) != sizeof(state))
  {
    Serial.println("Could not save BSEC calibration state");
    return;
  }

  bsecStateSaved = true;
  lastBsecStateSave = now;
  Serial.println("Saved BSEC calibration state");
}

void newDataCallback(const bme68xData data, const bsecOutputs outputs, const Bsec2 sensor)
{
  (void)data;
  bool gotTemperature = false;
  bool gotHumidity = false;
  for (uint8_t i = 0; i < outputs.nOutputs; ++i)
  {
    const bsecData &output = outputs.output[i];
    switch (output.sensor_id)
    {
    case BSEC_OUTPUT_IAQ:
      iaq_BME = output.signal;
      iaqAccuracy = output.accuracy;
      iaqValid = iaqAccuracy > 0 && isfinite(iaq_BME) && iaq_BME >= 0.0f && iaq_BME <= 500.0f;
      Serial.print("BSEC AQI=");
      Serial.print(iaq_BME);
      Serial.print(", accuracy=");
      Serial.println(iaqAccuracy);
      if (iaqAccuracy == 0)
      {
        Serial.println("BSEC AQI is not calibrated; value will not be published as VOC");
      }
      saveBsecState(sensor);
      break;
    case BSEC_OUTPUT_RAW_TEMPERATURE:
      tc_BME = output.signal;
      gotTemperature = true;
      break;
    case BSEC_OUTPUT_RAW_HUMIDITY:
      rh_BME = output.signal;
      gotHumidity = true;
      break;
    case BSEC_OUTPUT_RAW_PRESSURE:
      ap_BME = output.signal;
      alt_BME = 44330.0f * (1.0f - powf(ap_BME / SEALEVELPRESSURE_HPA, 0.1903f));
      break;
    default:
      break;
    }
  }

  if (gotTemperature && gotHumidity)
  {
    bmeReadingsValid = isfinite(tc_BME) && tc_BME >= -40.0f && tc_BME <= 85.0f &&
                       isfinite(rh_BME) && rh_BME >= 0.0f && rh_BME <= 100.0f;
    Serial.print("BME680 sample: temperature=");
    Serial.print(tc_BME);
    Serial.print(" C, humidity=");
    Serial.print(rh_BME);
    Serial.println(" %");
  }
}

void runBSEC()
{
  if (bsecReady && !bme.run())
  {
    Serial.print("BSEC run failed; BSEC status=");
    Serial.print(bme.status);
    Serial.print(", sensor status=");
    Serial.println(bme.sensor.status);
    if (bme.status < BSEC_OK)
    {
      Serial.print("BSEC processing error: ");
      Serial.println(bme.status);
    }
    if (bme.sensor.status < BME68X_OK)
    {
      Serial.print("BME680 sensor error: ");
      Serial.println(bme.sensor.status);
    }
  }
}

void initSensorAPDS9960()
{ Serial.println(F("Init APDS-9960"));
  
  delay(500);
  apdsReady = apds.begin(10,APDS9960_AGAIN_4X,0x39,&I2C_APDS);
  if (apdsReady)
  {
    Serial.println(F("APDS-9960 initialization complete"));
  }
  else
  {
    Serial.println(F("Something went wrong during APDS-9960 init!"));
  }

  // Start running the APDS-9960 light sensor (no interrupts)
  if (apdsReady)
  {
    apds.enableColor(true);
    Serial.println(F("Light sensor is now running"));
  }
}

void ReadSensorAPDS9960()
{
  if (!apdsReady)
  {
    return;
  }

  if (!apds.colorDataReady())
  {
    return;
  }
    uint16_t rawClear = 0;
    apds.getColorData(&red_light,&green_light,&blue_light,&rawClear);

    // Scale in 32-bit and clamp; the 16-bit multiply wrapped around in bright light
    const uint32_t scaled = (uint32_t)rawClear * 134 / 100;
    ambient_light = scaled > 65535 ? 65535 : (uint16_t)scaled;
    static uint32_t lastAmbientLog = 0;
    static bool hasLoggedAmbient = false;
    const uint32_t now = millis();
    if (!hasLoggedAmbient || now - lastAmbientLog >= 5000)
    {
      DBG_PRINT("Ambient: ");
      DBG_PRINT(ambient_light);
      DBG_PRINT(" Red: ");
      DBG_PRINT(red_light);
      DBG_PRINT(" Green: ");
      DBG_PRINT(green_light);
      DBG_PRINT(" Blue: ");
      DBG_PRINTLN(blue_light);
      lastAmbientLog = now;
      hasLoggedAmbient = true;
    }
}

void setupADC()
{
  analogReadResolution(10);
  analogSetPinAttenuation(potPin, ADC_11db);
}

void ReadVoltageBattery() //from ADC1 connected to PAR Input
{ 
  uint32_t rawSum = 0;
  uint32_t mvSum = 0;
  for (int i = 0; i < 8; ++i)
  {
    rawSum += analogRead(potPin);
    mvSum += analogReadMilliVolts(potPin);
    delay(1);
  }
  potValue = rawSum / 8.0f;
  DBG_PRINT("ADC1raw  =   ");
  DBG_PRINTLN(potValue);
  InVoltage = (mvSum / 8.0f) / 1000.0f;
  DBG_PRINT("ADC1 = ");
  DBG_PRINT(InVoltage,3);
  DBG_PRINTLN("V");
}
void DisplayMeasurementData(void)
{
  static unsigned long screenStarted = 0;
  static uint8_t screenIndex = 0;
  static uint8_t lastScreenIndex = 255;
  const unsigned long now = millis();

  if (screenStarted == 0)
  {
    screenStarted = now;
  }

  // Rotate screen strictly once every 3000 ms
  if (now - screenStarted >= 3000)
  {
    const bool wifiOffline = (WiFi.status() != WL_CONNECTED);
    const uint8_t screenCount = (wifiOffline ? 6 : 5) + (updateAvailable ? 1 : 0);
    screenIndex = (screenIndex + 1) % screenCount;
    screenStarted = now;
  }

  // The update notice is the last screen index (7) so it never shifts the others
  const bool showUpdate = updateAvailable && screenIndex == ((WiFi.status() != WL_CONNECTED) ? 6 : 5);
  if (showUpdate)
  {
    if (lastScreenIndex == 7)
    {
      return;
    }
    lastScreenIndex = 7;
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->clear();
    Heltec.display->drawString(0, 0, "Update " + latestVersion);
    Heltec.display->drawString(0, 10, "Open in browser:");
    Heltec.display->drawString(0, 20, WiFi.localIP().toString());
    Heltec.display->display();
    return;
  }

  // Draw ONLY once when transitioning to a new screen.
  // Never redraw during the 3-second display period.
  if (screenIndex == lastScreenIndex)
  {
    return;
  }

  lastScreenIndex = screenIndex;

  String s1;
  String s2;
  String s3;

  if (screenIndex == 0)
  {
    s1 = String("TC = " + String(tc_BME, 1) + "*C");
    s2 = String("RH = " + String(rh_BME, 1) + "%");
    s3 = "";
  }
  else if (screenIndex == 1)
  {
    s1 = "Color Space";
    s2 = String(red_light) + "," + String(green_light) + "," + String(blue_light);
    s3 = "RGB";
  }
  else if (screenIndex == 2)
  {
    s1 = "Vext";
    s2 = String(InVoltage, 3) + "V";
    s3 = "";
  }
  else if (screenIndex == 3)
  {
    s1 = "A-Light  " + String(ambient_light);
    s2 = "";
    s3 = "";
  }
  else if (screenIndex == 4)
  {
    s1 = "Air Quality";
    s2 = "CO2: " + String(co2_CZR);
    s3 = iaqValid ? "AQI: " + String((int)roundf(iaq_BME)) : "AQI: N/A";
  }
  else
  {
    s1 = "WiFi is Offline";
    s2 = "Reconnecting...";
    s3 = "";
  }

  Heltec.display->setFont(ArialMT_Plain_10);
  // Erase only the three text rows instead of clear()ing the whole frame:
  // clear() marks the entire buffer dirty, so display() pushes a full frame
  // and the screen visibly blanks for a few ms on every rotation (flicker).
  Heltec.display->setColor(BLACK);
  Heltec.display->fillRect(0, 0, 64, 32);
  Heltec.display->setColor(WHITE);
  if (screenIndex == 1)
  {
    Heltec.display->drawString(0, 0, s1);
    Heltec.display->drawString(0, 10, "R");
    Heltec.display->drawString(22, 10, "G");
    Heltec.display->drawString(44, 10, "B");
    Heltec.display->drawString(0, 20, String(red_light));
    Heltec.display->drawString(22, 20, String(green_light));
    Heltec.display->drawString(44, 20, String(blue_light));
  }
  else
  {
    Heltec.display->drawString(0, 0, s1);
    Heltec.display->drawString(0, 10, s2);
    Heltec.display->drawString(0, 20, s3);
  }

  Heltec.display->display();
}

void OTA_http()
{
  Serial.print("firmware current version: ");
  Serial.println(firmwareVersion);
  http.begin("http://dev.combros.tech:1880/api/ottosensors/newFirmware");
  int httpCode = http.GET();
  Serial.print("statusCode :");
  Serial.println(httpCode);
  if (httpCode == 200)
  {
    String payload = http.getString();
    Serial.println(payload);
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    const char *fw_version = doc["fw_version"];
    Serial.print("firmware new: ");
    Serial.println(fw_version);
    if (strcmp(fw_version, firmwareVersion))
    {
      const char *link = doc["link"];
      firmware_ota(link);
    }
  }
  else
  {
    Serial.println("Error on HTTP request");
  }
  http.end();
}
//------------------------------------------setup----------------------------------------------------------
void setup()
{
  get_imei();
  
  Heltec.begin(false /*DisplayEnable Disable*/, true /*Heltec.LoRa Enable*/, true /*Serial Enable*/, true /*PABOOST Enable*/, BAND /*long BAND*/);
  Heltec.LoRa.setSignalBandwidth(LORA_BANDWIDTH * 1000L);
  Heltec.LoRa.setSpreadingFactor(LORA_SPREADING_FACTOR);
  Heltec.LoRa.setCodingRate4(LORA_CODING_RATE);
  Heltec.LoRa.setSyncWord(LORA_SYNC_WORD);
  Heltec.display->init();
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->clear();
  Heltec.display->display();

  Serial2.begin(9600, SERIAL_8N1, 32, 17);
  delay(10);

  setupADC();
  initSensorCOZIR();
  delay(1000);
  czr.CalibrateFreshAir();

  pinMode(Vext, OUTPUT);
  delay(50);
  I2C_SENSORS.begin(13, 21);
  I2C_SENSORS.setClock(100000);
  pinMode(13, INPUT_PULLUP);
  pinMode(21, INPUT_PULLUP);
  delay(50);

  scanSensorI2CBus();
  initSensorAPDS9960();
  delay(50);

  initSensorBME680();
 
  pinMode(BUZZPin, OUTPUT);  
  ledcSetup(BUZZ_CHANNEL, pwmFreq, pwmResolution);
  ledcAttachPin(BUZZPin, BUZZ_CHANNEL);
  ledcWrite(BUZZ_CHANNEL, 0);

  pinMode(outputPwmPin, OUTPUT);
  ledcSetup(outputPwmChannel, pwm_freq, pwmResolution);
  ledcAttachPin(outputPwmPin, outputPwmChannel);
  ledcWrite(outputPwmChannel, static_cast<uint32_t>(pwm_duty) * 255U / 100U);

  pinMode(powerPin, OUTPUT);
  digitalWrite(powerPin, LOW);

  setup_network();
  if (WiFi.status() == WL_CONNECTED)
  {
    mqtt_connect();
  }
  else
  {
    Serial.println("Skipping MQTT connection: WiFi is not connected");
  }
}

const char deviceId[] = "unique_string";

void populateMeasurementJson(JsonDocument &document, const char *measurementImei)
{
  document["id"] = deviceId;
  document["imei"] = measurementImei;
  document["temperature"] = bmeReadingsValid ? roundf(tc_BME * 10.0f) / 10.0f : NAN;
  document["humidity"] = bmeReadingsValid ? roundf(rh_BME * 10.0f) / 10.0f : NAN;
  document["co2"] = co2_CZR;
  if (iaqValid)
  {
    document["voc"] = static_cast<int>(lroundf(iaq_BME));
  }
  else
  {
    document["voc"] = 0;
  }
  document["red"] = red_light;
  document["green"] = green_light;
  document["blue"] = blue_light;
  document["clear"] = ambient_light;
  document["pressure"] = roundf(ap_BME * 10.0f) / 10.0f;
  document["altitude"] = roundf(alt_BME * 10.0f) / 10.0f;
  document["voltage"] = roundf(InVoltage * 1000.0f) / 1000.0f;
  document["pwm_freq"] = pwm_freq;
  document["pwm_duty"] = static_cast<unsigned int>(pwm_duty);
  document["power"] = static_cast<int>(power);
}

void sendCo2ToPager()
{
  static uint32_t fakeHitCount = 0;
  ++fakeHitCount;

  int beginStatus = Heltec.LoRa.beginPacket();
  Heltec.LoRa.print("Hit count: ");
  Heltec.LoRa.print(fakeHitCount);
  int transmitStatus = Heltec.LoRa.endPacket();
  DBG_PRINT("LoRa packet: ");
  DBG_PRINT("Hit count: ");
  DBG_PRINT(fakeHitCount);
  DBG_PRINT(" begin=");
  DBG_PRINT(beginStatus);
  DBG_PRINT(" end=");
  DBG_PRINTLN(transmitStatus);
}

void get_data_to_send()
{
  char jsonBuffer[512];
  DynamicJsonDocument ottoDataObj(512);

  populateMeasurementJson(ottoDataObj, imei.c_str());

  serializeJson(ottoDataObj, jsonBuffer);
  DBG_PRINTLN(jsonBuffer);
  client.publish(MQTT_PUB_TOPIC, jsonBuffer);
}

// Periodically verify WiFi and MQTT are up and restore them if not (non-blocking)
void checkConnections()
{
  static uint32_t lastCheck = 0;
  static uint32_t lastWifiAttempt = 0;
  const uint32_t checkIntervalMs = 10000;
  const uint32_t wifiRetryMs = 15000;

  // While provisioning, service the web page
  // and do NOT try normal WiFi reconnection.
  if (wifiSetupMode)
  {
    dnsServer.processNextRequest();
    wifiServer.handleClient();
    return;
  }

  const uint32_t now = millis();

  if (now - lastCheck < checkIntervalMs)
  {
    return;
  }

  lastCheck = now;

  if (WiFi.status() != WL_CONNECTED)
  {
    if (now - lastWifiAttempt >= wifiRetryMs || lastWifiAttempt == 0)
    {
      Serial.println("WiFi down; reconnecting...");

      WiFi.disconnect();
      WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

      lastWifiAttempt = now;
    }

    return;
  }

  lastWifiAttempt = 0;

  if (!client.connected())
  {
    Serial.println("MQTT down; reconnecting...");
    mqtt_connect();
  }
}

//--------------------------------loop-------------------------------------------------
void loop()
{ 
  // Keep sensors, display, LoRa and watchdog running while the setup portal is active
  if (wifiSetupMode)
  {
    dnsServer.processNextRequest();
    wifiServer.handleClient();
  }
  else
  {
    wifiServer.handleClient();
    checkConnections();
    client.loop();
    if (otaPending)
    {
      runOta();
    }
    static uint32_t lastUpdateCheck = 0;
    if (updateCheckRequested || millis() - lastUpdateCheck > 6UL * 3600UL * 1000UL)
    {
      lastUpdateCheck = millis();
      checkForUpdate();
    }
    if (publishRequested)
    {
      publishRequested = false;
      get_data_to_send();
    }
  }
  runBSEC();
  ReadSensorAPDS9960();
  DisplayMeasurementData();
  if (!wifiSetupMode)
  {
    client.loop();
  }

  const uint32_t now = millis();
  const bool publishSensor = now - lastSensorPublish > 5000;
  if (publishSensor)
  {
    ReadSensorCozir();
    ReadVoltageBattery();

    if (!wifiSetupMode)
    {
      get_data_to_send();
    }
    // LoRa TX 1 s after the MQTT publish so the WiFi and LoRa current
    // spikes don't coincide and sag the OLED supply (visible flicker)
    pagerTxAt = now + 1000;
    lastSensorPublish = millis();

    if (!buzzerOn && co2_CZR >= 800) {
      ledcWrite(BUZZ_CHANNEL, 128);
      buzzerOn = true;
      buzzerStartTime = millis();
    }
  }

  if (pagerTxAt != 0 && (int32_t)(now - pagerTxAt) >= 0)
  {
    pagerTxAt = 0;
    sendCo2ToPager();
  }

  if (buzzerOn && (millis() - buzzerStartTime >= buzzerDuration)) {
    ledcWrite(BUZZ_CHANNEL, 0);
    buzzerOn = false;
  }

  wdt.reset();
}
