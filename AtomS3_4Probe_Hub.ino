#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <stdarg.h>

// The hub and all four probes must join the same 2.4 GHz Wi-Fi network.
const char* WIFI_SSID = "McDonalds_Free_Wifi";
const char* WIFI_PASS = "similarshift507";

const char* HUB_HOSTNAME = "atoms3hub";  // http://atoms3hub.local/
const uint16_t SENSOR_PORT = 4210;
const uint32_t PROBE_TIMEOUT_MS = 3000;

const size_t PROBE_COUNT = 4;
const char* PROBE_IDS[PROBE_COUNT] = {
  "probe1", "probe2", "probe3", "probe4"
};

WebServer server(80);
WiFiUDP sensorUdp;

struct ProbeReading {
  bool received = false;
  uint32_t sequence = 0;
  uint32_t probeUptimeMs = 0;
  uint32_t receivedAtMs = 0;  // Common hub clock used by all four graphs.
  float ax = 0;
  float ay = 0;
  float az = 0;
  float gx = 0;
  float gy = 0;
  float gz = 0;
  int rssi = 0;
};

ProbeReading probes[PROBE_COUNT];
String ipString;
uint32_t lastDisplayMs = 0;

const char INDEX_HTML[] = R"ATOMHTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>AtomS3 Four-Probe Monitor</title>
  <style>
    :root{color-scheme:dark}
    *{box-sizing:border-box}
    body{font-family:monospace;background:#0b0b0b;color:#0f0;margin:12px}
    h2{margin:0 0 4px}
    #hub-status{color:#aaa;margin-bottom:10px}
    .legend{color:#aaa;margin:0 0 12px}
    .red{color:#f44}.green{color:#4f4}.blue{color:#48f}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
    .card{border:1px solid #333;background:#101010;padding:10px;min-width:0}
    .card h3{margin:0 0 6px;color:#0f0}
    .values{white-space:pre-wrap;line-height:1.35;min-height:38px}
    .status{color:#aaa;margin:6px 0;min-height:16px}
    .card.bad{border-color:#622}
    .card.bad h3,.status.bad{color:#f66}
    canvas{display:block;background:#000;border:1px solid #333;width:100%;height:190px}
    @media(max-width:800px){.grid{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <h2>AtomS3 Four-Probe Monitor</h2>
  <div id="hub-status">Connecting to hub...</div>
  <div class="legend">
    gyro: <span class="red">x</span> / <span class="green">y</span> /
    <span class="blue">z</span> &nbsp; shared 60-second gateway timeline
  </div>
  <main class="grid" id="probe-grid"></main>

  <script>
    const POLL_MS=250;
    const WINDOW_MS=60000;
    const MAX_SAMPLES=300;
    const GYRO_RANGE=150;
    const PROBE_IDS=['probe1','probe2','probe3','probe4'];
    const colors={gx:'#f44',gy:'#4f4',gz:'#48f'};
    const state={};

    document.getElementById('probe-grid').innerHTML=PROBE_IDS.map((id,index)=>`
      <section class="card" id="card-${id}">
        <h3>Probe ${index+1} <small>(${id})</small></h3>
        <div class="values" id="values-${id}">Waiting for first packet...</div>
        <div class="status" id="status-${id}">NO DATA</div>
        <canvas id="chart-${id}" width="640" height="190"></canvas>
      </section>`).join('');

    for(const id of PROBE_IDS){
      state[id]={samples:[],lastSequence:null,lastProbeUptime:null};
    }

    // millis() is a 32-bit counter. The unsigned conversion keeps age
    // calculations correct even when the counter wraps after about 49 days.
    function elapsed32(now,then){return (now-then)>>>0;}

    function draw(id,hubNow){
      const canvas=document.getElementById('chart-'+id);
      const ctx=canvas.getContext('2d');
      const width=canvas.width, height=canvas.height;
      const top=18, bottom=height-18, mid=(top+bottom)/2;

      ctx.fillStyle='#000';
      ctx.fillRect(0,0,width,height);
      ctx.font='11px monospace';
      ctx.fillStyle='#777';

      for(let i=0;i<=4;i++){
        const x=i*width/4;
        ctx.strokeStyle=i===4 ? '#555' : '#222';
        ctx.beginPath();ctx.moveTo(x,top);ctx.lineTo(x,bottom);ctx.stroke();
        const seconds=Math.round((4-i)*WINDOW_MS/4000);
        ctx.fillText(i===4?'now':'-'+seconds+'s',Math.min(x+3,width-30),height-4);
      }

      ctx.strokeStyle='#444';
      ctx.beginPath();ctx.moveTo(0,mid);ctx.lineTo(width,mid);ctx.stroke();
      ctx.fillStyle='#777';
      ctx.fillText('+'+GYRO_RANGE,3,11);
      ctx.fillText('0',3,mid-3);
      ctx.fillText('-'+GYRO_RANGE,3,height-21);

      for(const key of ['gx','gy','gz']){
        ctx.strokeStyle=colors[key];
        ctx.beginPath();
        let started=false;
        for(const sample of state[id].samples){
          const age=elapsed32(hubNow,sample.t);
          if(age>WINDOW_MS) continue;
          const x=width-(age/WINDOW_MS)*width;
          const value=Math.max(-GYRO_RANGE,Math.min(GYRO_RANGE,sample[key]));
          const y=mid-(value/GYRO_RANGE)*(bottom-top)/2;
          started ? ctx.lineTo(x,y) : ctx.moveTo(x,y);
          started=true;
        }
        if(started) ctx.stroke();
      }
    }

    function updateProbe(probe,hubNow,latency){
      const id=probe.id;
      const s=state[id];
      if(!s) return;

      const card=document.getElementById('card-'+id);
      const values=document.getElementById('values-'+id);
      const status=document.getElementById('status-'+id);

      if(!probe.received){
        values.textContent='Waiting for first packet...';
        status.textContent='NO DATA   browser latency '+latency+' ms';
        status.className='status bad';
        card.className='card bad';
        draw(id,hubNow);
        return;
      }

      // A lower uptime means this probe rebooted. Start its graph again rather
      // than connecting readings from two different runs.
      if(s.lastProbeUptime!==null && probe.probe_uptime_ms<s.lastProbeUptime){
        s.samples=[];
        s.lastSequence=null;
      }

      if(probe.sequence!==s.lastSequence){
        s.samples.push({
          t:probe.received_at_ms,
          gx:probe.gx,gy:probe.gy,gz:probe.gz
        });
        if(s.samples.length>MAX_SAMPLES) s.samples.shift();
        s.lastSequence=probe.sequence;
        s.lastProbeUptime=probe.probe_uptime_ms;
      }

      s.samples=s.samples.filter(sample=>elapsed32(hubNow,sample.t)<=WINDOW_MS);

      values.textContent=
        'accel  x '+probe.ax.toFixed(3)+'  y '+probe.ay.toFixed(3)+'  z '+probe.az.toFixed(3)+' g\n'+
        'gyro   x '+probe.gx.toFixed(1)+'  y '+probe.gy.toFixed(1)+'  z '+probe.gz.toFixed(1)+' deg/s';

      const label=probe.online?'ONLINE':'STALE / OFFLINE';
      status.textContent=label+
        '   seq '+probe.sequence+
        '   age '+probe.age_ms+' ms'+
        '   gateway t '+probe.received_at_ms+' ms'+
        '   WiFi '+probe.rssi+' dBm';
      status.className=probe.online?'status':'status bad';
      card.className=probe.online?'card':'card bad';
      draw(id,hubNow);
    }

    async function update(){
      try{
        const started=performance.now();
        const response=await fetch('/data',{cache:'no-store'});
        if(!response.ok) throw new Error('HTTP '+response.status);
        const data=await response.json();
        if(!Array.isArray(data.probes)) throw new Error('Invalid hub response');
        const latency=Math.round(performance.now()-started);

        document.getElementById('hub-status').textContent=
          'Hub online   '+data.online_count+'/4 probes online   browser latency '+latency+' ms';
        for(const probe of data.probes){
          updateProbe(probe,data.hub_uptime_ms,latency);
        }
      }catch(error){
        document.getElementById('hub-status').textContent=
          'Connection to hub lost - retrying...';
        for(const id of PROBE_IDS){
          document.getElementById('card-'+id).className='card bad';
        }
      }
      setTimeout(update,POLL_MS);
    }

    for(const id of PROBE_IDS) draw(id,0);
    update();
  </script>
</body>
</html>
)ATOMHTML";

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

void showHubSummary() {
  const uint32_t now = millis();
  size_t onlineCount = 0;

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Display.println("HUB ONLINE");
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.println(ipString);

  for (size_t i = 0; i < PROBE_COUNT; ++i) {
    const uint32_t ageMs = probes[i].received ? now - probes[i].receivedAtMs : 0;
    const bool online = probes[i].received && ageMs <= PROBE_TIMEOUT_MS;
    if (online) ++onlineCount;

    M5.Display.setTextColor(online ? TFT_GREEN : TFT_RED, TFT_BLACK);
    M5.Display.print(PROBE_IDS[i]);
    M5.Display.print(online ? " ON " : " -- ");
    if (probes[i].received) {
      M5.Display.print(ageMs);
      M5.Display.print("ms");
    } else {
      M5.Display.print("no data");
    }
    M5.Display.println();
  }

  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.printf("online %u/%u\n", (unsigned)onlineCount,
                    (unsigned)PROBE_COUNT);
}

void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

// Bounded appending prevents a future JSON field from silently overflowing
// the response buffer.
bool appendJson(char* buffer, size_t capacity, size_t& used,
                const char* format, ...) {
  if (used >= capacity) return false;

  va_list args;
  va_start(args, format);
  const int written = vsnprintf(buffer + used, capacity - used, format, args);
  va_end(args);

  if (written < 0 || (size_t)written >= capacity - used) return false;
  used += (size_t)written;
  return true;
}

void handleData() {
  const uint32_t now = millis();
  size_t onlineCount = 0;

  for (size_t i = 0; i < PROBE_COUNT; ++i) {
    const uint32_t ageMs = probes[i].received ? now - probes[i].receivedAtMs : 0;
    if (probes[i].received && ageMs <= PROBE_TIMEOUT_MS) ++onlineCount;
  }

  char json[1900];
  size_t used = 0;
  bool ok = appendJson(json, sizeof(json), used,
                       "{\"hub_uptime_ms\":%lu,\"online_count\":%u,\"probes\":[",
                       (unsigned long)now, (unsigned)onlineCount);

  for (size_t i = 0; ok && i < PROBE_COUNT; ++i) {
    const ProbeReading& probe = probes[i];
    const uint32_t ageMs = probe.received ? now - probe.receivedAtMs : 0;
    const bool online = probe.received && ageMs <= PROBE_TIMEOUT_MS;

    ok = appendJson(
      json, sizeof(json), used,
      "%s{\"received\":%s,\"online\":%s,\"id\":\"%s\","
      "\"sequence\":%lu,\"probe_uptime_ms\":%lu,"
      "\"received_at_ms\":%lu,\"age_ms\":%lu,"
      "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
      "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,\"rssi\":%d}",
      i ? "," : "",
      probe.received ? "true" : "false",
      online ? "true" : "false",
      PROBE_IDS[i],
      (unsigned long)probe.sequence,
      (unsigned long)probe.probeUptimeMs,
      (unsigned long)probe.receivedAtMs,
      (unsigned long)ageMs,
      probe.ax, probe.ay, probe.az,
      probe.gx, probe.gy, probe.gz, probe.rssi);
  }

  if (ok) ok = appendJson(json, sizeof(json), used, "]}");

  if (!ok) {
    Serial.println("JSON response buffer is too small.");
    server.send(500, "text/plain", "JSON response overflow");
    return;
  }

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

int findProbeIndex(const char* id) {
  for (size_t i = 0; i < PROBE_COUNT; ++i) {
    if (strcmp(id, PROBE_IDS[i]) == 0) return (int)i;
  }
  return -1;
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

  if (fields != 11 || strcmp(protocol, "S3P1") != 0) {
    Serial.printf("Rejected malformed UDP packet: %s\n", packet);
    return true;
  }

  const int probeIndex = findProbeIndex(probeId);
  if (probeIndex < 0) {
    Serial.printf("Rejected packet from unknown probe ID: %s\n", probeId);
    return true;
  }

  ProbeReading& probe = probes[probeIndex];
  probe.received = true;
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
                PROBE_IDS[probeIndex], (unsigned long)probe.sequence,
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
  showHubSummary();

  Serial.printf("Dashboard: http://%s/ or http://%s.local/\n",
                ipString.c_str(), HUB_HOSTNAME);
  Serial.printf("Listening for four probes on UDP port %u\n", SENSOR_PORT);
}

void loop() {
  M5.update();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("HUB WIFI LOST", "Restarting...", "", TFT_RED);
    delay(3000);
    ESP.restart();
  }

  // Drain every waiting datagram before serving browser requests.
  while (receiveProbePacket()) {}

  server.handleClient();

  if (millis() - lastDisplayMs >= 1000) {
    lastDisplayMs = millis();
    showHubSummary();
  }

  delay(1);
}
