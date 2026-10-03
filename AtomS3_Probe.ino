#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>

// Both AtomS3 devices must join the same 2.4 GHz Wi-Fi network.
const char* WIFI_SSID = "McDonalds_Free_Wifi";
const char* WIFI_PASS = "similarshift507";

const char* PROBE_ID = "probe1";
const char* PROBE_HOSTNAME = "atoms3probe1";
const char* HUB_HOSTNAME = "atoms3hub";
const uint16_t HUB_SENSOR_PORT = 4210;
const uint32_t SEND_INTERVAL_MS = 250;
const uint32_t RESOLVE_INTERVAL_MS = 5000;

WiFiUDP sensorUdp;
IPAddress hubAddress;
uint32_t sequenceNumber = 0;
uint32_t lastSendMs = 0;
uint32_t lastResolveAttemptMs = 0;
uint32_t lastDisplayMs = 0;

void showStatus(const String& first, const String& second = "",
                const String& third = "", uint16_t color = TFT_WHITE) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.println(first);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  if (second.length()) M5.Display.println(second);
  if (third.length()) M5.Display.println(third);
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(PROBE_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  showStatus("PROBE CONNECTING", WIFI_SSID);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("PROBE WIFI FAIL", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
    return false;
  }

  return true;
}

bool resolveHub() {
  lastResolveAttemptMs = millis();
  showStatus("FINDING HUB", "atoms3hub.local");

  hubAddress = MDNS.queryHost(HUB_HOSTNAME, 2000);
  if ((uint32_t)hubAddress == 0) {
    Serial.println("Hub not found; another lookup will be attempted.");
    return false;
  }

  Serial.printf("Hub resolved to %s\n", hubAddress.toString().c_str());
  return true;
}

bool sendReading() {
  auto reading = M5.Imu.getImuData();

  // S3P1 is the protocol/version tag. UDP preserves this complete message as
  // one datagram, so the hub can validate it before publishing the values.
  char packet[220];
  const int length = snprintf(packet, sizeof(packet),
                              "S3P1,%s,%lu,%lu,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%d",
                              PROBE_ID,
                              (unsigned long)sequenceNumber,
                              (unsigned long)millis(),
                              reading.accel.x, reading.accel.y, reading.accel.z,
                              reading.gyro.x, reading.gyro.y, reading.gyro.z,
                              WiFi.RSSI());

  if (length <= 0 || length >= (int)sizeof(packet)) {
    Serial.println("Sensor packet formatting failed.");
    return false;
  }

  if (!sensorUdp.beginPacket(hubAddress, HUB_SENSOR_PORT)) {
    Serial.println("Could not begin UDP packet.");
    return false;
  }

  sensorUdp.write((const uint8_t*)packet, (size_t)length);
  const bool sent = sensorUdp.endPacket() == 1;

  if (sent) {
    sequenceNumber++;
  } else {
    Serial.println("UDP send failed; hub address will be resolved again.");
    hubAddress = IPAddress();
  }

  return sent;
}

void setup() {
  auto config = M5.config();
  M5.begin(config);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(120);
  Serial.begin(115200);

  if (!connectToWiFi()) return;

  if (!MDNS.begin(PROBE_HOSTNAME)) {
    showStatus("PROBE MDNS FAIL", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
  }

  sensorUdp.begin(0);  // Let the network stack choose the probe's source port.
  resolveHub();
}

void loop() {
  M5.update();
  M5.Imu.update();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("PROBE WIFI LOST", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
  }

  if ((uint32_t)hubAddress == 0) {
    if (millis() - lastResolveAttemptMs >= RESOLVE_INTERVAL_MS) {
      resolveHub();
    }
  } else if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    sendReading();
  }

  if (millis() - lastDisplayMs >= 1000) {
    lastDisplayMs = millis();
    if ((uint32_t)hubAddress == 0) {
      showStatus("PROBE ONLINE", PROBE_ID, "Searching for hub", TFT_YELLOW);
    } else {
      showStatus("SENDING DATA", PROBE_ID,
                 "packet " + String(sequenceNumber), TFT_GREEN);
    }
  }

  delay(1);
}
