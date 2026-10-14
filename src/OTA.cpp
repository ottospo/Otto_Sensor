#include "OTA.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

// Downloads a firmware image over HTTP/HTTPS and flashes it to the next OTA slot.
// Returns an empty string on success (caller should reboot) or an error message.
String firmware_ota(String url, String md5, void (*progress)(int percent))
{
  if (WiFi.status() != WL_CONNECTED)
    return "wifi not connected";

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  bool begun;
  if (url.startsWith("https://"))
  {
    // Image integrity is checked with the MD5 sent in the authenticated MQTT command.
    secureClient.setInsecure();
    begun = http.begin(secureClient, url);
  }
  else
  {
    begun = http.begin(plainClient, url);
  }
  if (!begun)
    return "bad url";

  Serial.print("OTA from: ");
  Serial.println(url);

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    http.end();
    return "http " + String(code);
  }

  int total = http.getSize();
  if (total <= 0 || !Update.begin(total))
  {
    http.end();
    return "no space / unknown size";
  }
  if (md5.length() == 32)
    Update.setMD5(md5.c_str());

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  int lastPct = -1;
  uint32_t lastData = millis();
  while (written < total)
  {
    size_t avail = stream->available();
    if (avail)
    {
      int n = stream->readBytes(buf, min(avail, sizeof(buf)));
      if (Update.write(buf, n) != (size_t)n)
      {
        Update.abort();
        http.end();
        return "flash write failed";
      }
      written += n;
      lastData = millis();
      int pct = (int)((int64_t)written * 100 / total);
      if (pct != lastPct)
      {
        lastPct = pct;
        if (progress)
          progress(pct);
      }
    }
    else if (!stream->connected() || millis() - lastData > 15000)
    {
      Update.abort();
      http.end();
      return "download interrupted";
    }
    delay(1);
  }
  http.end();

  if (!Update.end())
    return String("verify failed: ") + Update.errorString();
  return "";
}
