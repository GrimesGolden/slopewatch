#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// Both AtomS3 devices must join the same 2.4 GHz Wi-Fi network.
const char* WIFI_SSID = "McDonalds_Free_Wifi";
const char* WIFI_PASS = "similarshift507";

const char* HUB_HOSTNAME = "atoms3hub";  // http://atoms3hub.local/
const uint16_t SENSOR_PORT = 4210;
const uint32_t PROBE_TIMEOUT_MS = 3000;

WebServer server(80);
WiFiUDP sensorUdp;

struct ProbeReading {
  bool received = false;
  char id[17] = "";
  uint32_t sequence = 0;
  uint32_t probeUptimeMs = 0;
  uint32_t receivedAtMs = 0;
  float ax = 0;
  float ay = 0;
  float az = 0;
  float gx = 0;
  float gy = 0;
  float gz = 0;
  int rssi = 0;
};

ProbeReading probe;
String ipString;
uint32_t lastDisplayMs = 0;

const char INDEX_HTML[] = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>AtomS3 Remote Probe</title>
  <style>
    body{font-family:monospace;background:#0b0b0b;color:#0f0;margin:12px}
    canvas{background:#000;border:1px solid #333;width:100%;max-width:720px;height:240px}
    #status{color:#aaa;margin:8px 0}
    .bad{color:#f66}
  </style>
</head>
<body>
  <h3>AtomS3 Remote Probe Monitor</h3>
  <div id="probe">Waiting for probe data...</div>
  <div id="acc"></div>
  <div id="gyr"></div>
  <div id="status"></div>
  <canvas id="chart" width="720" height="240"></canvas>
  <script>
    const POLL_MS=250, HISTORY=240;
    const canvas=document.getElementById('chart');
    const ctx=canvas.getContext('2d');
    const history={gx:[],gy:[],gz:[]};
    let lastSequence=-1;

    function draw(){
      ctx.fillStyle='#000';
      ctx.fillRect(0,0,canvas.width,canvas.height);
      ctx.strokeStyle='#444';
      ctx.beginPath();
      ctx.moveTo(0,120);
      ctx.lineTo(canvas.width,120);
      ctx.stroke();

      const colors={gx:'#f44',gy:'#4f4',gz:'#48f'};
      for(const key in history){
        ctx.strokeStyle=colors[key];
        ctx.beginPath();
        history[key].forEach((value,index)=>{
          const x=index*(canvas.width/(HISTORY-1));
          const y=120-value;
          index ? ctx.lineTo(x,y) : ctx.moveTo(x,y);
        });
        ctx.stroke();
      }
    }

    async function update(){
      try{
        const started=performance.now();
        const response=await fetch('/data',{cache:'no-store'});
        const data=await response.json();
        const latency=Math.round(performance.now()-started);

        if(!data.received){
          document.getElementById('probe').textContent='Waiting for first probe packet...';
          document.getElementById('status').textContent='Hub is online; remote probe has not reported yet.';
        }else{
          document.getElementById('probe').textContent='probe  '+data.id+'   sequence '+data.sequence;
          document.getElementById('acc').textContent=
            'accel  x '+data.ax.toFixed(3)+'  y '+data.ay.toFixed(3)+'  z '+data.az.toFixed(3)+' g';
          document.getElementById('gyr').textContent=
            'gyro   x '+data.gx.toFixed(1)+'  y '+data.gy.toFixed(1)+'  z '+data.gz.toFixed(1)+' deg/s';

          const state=data.online ? 'ONLINE' : 'STALE / OFFLINE';
          document.getElementById('status').textContent=
            state+'   packet age '+data.age_ms+' ms   probe WiFi '+data.rssi+
            ' dBm   browser latency '+latency+' ms';
          document.getElementById('status').className=data.online ? '' : 'bad';

          if(data.online && data.sequence!==lastSequence){
            lastSequence=data.sequence;
            for(const key in history){
              history[key].push(data[key]);
              if(history[key].length>HISTORY) history[key].shift();
            }
            draw();
          }
        }
      }catch(error){
        document.getElementById('status').textContent='Connection to hub lost - retrying...';
        document.getElementById('status').className='bad';
      }
      setTimeout(update,POLL_MS);
    }

    draw();
    update();
  </script>
</body>
</html>
)HTML";

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

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  const uint32_t ageMs = probe.received ? millis() - probe.receivedAtMs : 0;
  const bool online = probe.received && ageMs <= PROBE_TIMEOUT_MS;

  char json[420];
  snprintf(json, sizeof(json),
           "{\"received\":%s,\"online\":%s,\"id\":\"%s\","
           "\"sequence\":%lu,\"probe_uptime_ms\":%lu,\"age_ms\":%lu,"
           "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
           "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,\"rssi\":%d}",
           probe.received ? "true" : "false",
           online ? "true" : "false",
           probe.id,
           (unsigned long)probe.sequence,
           (unsigned long)probe.probeUptimeMs,
           (unsigned long)ageMs,
           probe.ax, probe.ay, probe.az,
           probe.gx, probe.gy, probe.gz, probe.rssi);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "text/plain", "not found");
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HUB_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  showStatus("HUB CONNECTING", WIFI_SSID);

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("HUB WIFI FAILED", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
    return false;
  }

  return true;
}

bool receiveProbePacket() {
  const int packetSize = sensorUdp.parsePacket();
  if (packetSize <= 0) return false;

  char packet[256];
  const int bytesRead = sensorUdp.read(packet, sizeof(packet) - 1);
  if (bytesRead <= 0) return true;
  packet[bytesRead] = '\0';

  char protocol[8];
  char probeId[17];
  unsigned long sequence;
  unsigned long probeUptime;
  float ax, ay, az, gx, gy, gz;
  int rssi;

  const int fields = sscanf(packet,
                            "%7[^,],%16[^,],%lu,%lu,%f,%f,%f,%f,%f,%f,%d",
                            protocol, probeId, &sequence, &probeUptime,
                            &ax, &ay, &az, &gx, &gy, &gz, &rssi);

  // Ignore malformed or unknown packets rather than publishing partial data.
  if (fields != 11 || strcmp(protocol, "S3P1") != 0) {
    Serial.printf("Rejected UDP packet: %s\n", packet);
    return true;
  }

  probe.received = true;
  snprintf(probe.id, sizeof(probe.id), "%s", probeId);
  probe.sequence = (uint32_t)sequence;
  probe.probeUptimeMs = (uint32_t)probeUptime;
  probe.receivedAtMs = millis();
  probe.ax = ax;
  probe.ay = ay;
  probe.az = az;
  probe.gx = gx;
  probe.gy = gy;
  probe.gz = gz;
  probe.rssi = rssi;

  Serial.printf("Probe %s packet %lu received from %s:%u\n",
                probe.id, (unsigned long)probe.sequence,
                sensorUdp.remoteIP().toString().c_str(), sensorUdp.remotePort());
  return true;
}

void setup() {
  auto config = M5.config();
  M5.begin(config);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(120);
  Serial.begin(115200);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);

  if (!connectToWiFi()) return;

  ipString = WiFi.localIP().toString();

  if (MDNS.begin(HUB_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("atoms3-sensor", "udp", SENSOR_PORT);
  } else {
    Serial.println("Warning: mDNS failed; use the displayed IP address.");
  }

  if (!sensorUdp.begin(SENSOR_PORT)) {
    showStatus("UDP START FAILED", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
  }

  server.begin();
  showStatus("HUB ONLINE", ipString, "atoms3hub.local", TFT_GREEN);

  Serial.printf("Dashboard: http://%s/ or http://%s.local/\n",
                ipString.c_str(), HUB_HOSTNAME);
  Serial.printf("Listening for probe packets on UDP port %u\n", SENSOR_PORT);
}

void loop() {
  M5.update();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("HUB WIFI LOST", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
  }

  // Drain every waiting datagram so browser requests cannot leave old sensor
  // packets accumulating in the UDP receive buffer.
  while (receiveProbePacket()) {}

  server.handleClient();

  if (millis() - lastDisplayMs >= 1000) {
    lastDisplayMs = millis();
    if (!probe.received) {
      showStatus("HUB ONLINE", ipString, "Waiting for probe", TFT_YELLOW);
    } else {
      const uint32_t ageMs = millis() - probe.receivedAtMs;
      const bool online = ageMs <= PROBE_TIMEOUT_MS;
      showStatus(online ? "PROBE ONLINE" : "PROBE STALE",
                 probe.id,
                 "age " + String(ageMs) + " ms",
                 online ? TFT_GREEN : TFT_RED);
    }
  }

  delay(1);
}
