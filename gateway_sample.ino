#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

const char* WIFI_SSID  = "McDonalds_Free_Wifi";      // 2.4 GHz only
const char* WIFI_PASS  = "similarshift507";
const char* HOSTNAME   = "atoms3r";             // -> http://atoms3r.local
const uint32_t POLL_MS = 250;                   // raise this if you add more viewers

WebServer server(80);
String ipStr;

const char INDEX_HTML[] = R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AtomS3R IMU</title><style>
body{font-family:monospace;background:#0b0b0b;color:#0f0;margin:12px}
canvas{background:#000;border:1px solid #333;width:100%;max-width:640px;height:200px}
#st{color:#888;margin-top:6px}</style></head><body>
<h3>AtomS3R IMU</h3><div id="acc"></div><div id="gyr"></div>
<canvas id="c" width="640" height="200"></canvas><div id="st"></div>
<script>
const POLL=250, N=200;                    // keep POLL in sync with POLL_MS
const c=document.getElementById('c'),x=c.getContext('2d');
const hist={gx:[],gy:[],gz:[]}; let n=0;
function draw(){x.fillStyle='#000';x.fillRect(0,0,c.width,c.height);
const col={gx:'#f44',gy:'#4f4',gz:'#48f'};
for(const k in hist){x.strokeStyle=col[k];x.beginPath();
hist[k].forEach((v,i)=>{const px=i*(c.width/N),py=100-v*0.9;
i?x.lineTo(px,py):x.moveTo(px,py);});x.stroke();}
x.strokeStyle='#444';x.beginPath();x.moveTo(0,100);x.lineTo(c.width,100);x.stroke();}
async function tick(){try{
const t0=performance.now();
const d=await (await fetch('/data',{cache:'no-store'})).json();
const rtt=Math.round(performance.now()-t0);
document.getElementById('acc').textContent='accel  x '+d.ax.toFixed(3)+'  y '+d.ay.toFixed(3)+'  z '+d.az.toFixed(3)+' g';
document.getElementById('gyr').textContent='gyro   x '+d.gx.toFixed(1)+'  y '+d.gy.toFixed(1)+'  z '+d.gz.toFixed(1)+' deg/s';
document.getElementById('st').textContent='WiFi '+d.rssi+' dBm   latency '+rtt+' ms   samples '+(++n);
for(const k in hist){hist[k].push(d[k]);if(hist[k].length>N)hist[k].shift();}draw();
}catch(e){document.getElementById('st').textContent='connection lost - retrying...';}
setTimeout(tick,POLL);}
tick();
</script></body></html>
)HTML";

void show(const String& a, const String& b = "", const String& c = "",
          uint16_t col = TFT_WHITE) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(col, TFT_BLACK);
  M5.Display.println(a);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  if (b.length()) M5.Display.println(b);
  if (c.length()) M5.Display.println(c);
}

void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

void handleData() {
  auto d = M5.Imu.getImuData();
  char json[220];
  snprintf(json, sizeof(json),
           "{\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
           "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,\"rssi\":%d}",
           d.accel.x, d.accel.y, d.accel.z,
           d.gyro.x,  d.gyro.y,  d.gyro.z, WiFi.RSSI());
  server.send(200, "application/json", json);
}

void handleNotFound() { server.send(404, "text/plain", "not found"); }

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(120);
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);          // must be before begin()

  // Optional: fixed IP so the URL never changes.
  // A DHCP reservation in your router is usually the safer choice.
  // IPAddress ip(192,168,1,60), gw(192,168,1,1), mask(255,255,255,0), dns(192,168,1,1);
  // WiFi.config(ip, gw, mask, dns);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  show("Connecting...", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(300);

  if (WiFi.status() != WL_CONNECTED) {
    show("WiFi FAILED", "Check 2.4GHz", "SSID / password", TFT_RED);
    return;
  }

  ipStr = WiFi.localIP().toString();
  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();

  show("LAN SERVER UP", ipStr, "atoms3r.local", TFT_GREEN);
  Serial.printf("Open http://%s/  or  http://%s.local/\n",
                ipStr.c_str(), HOSTNAME);
}

void loop() {
  M5.update();
  M5.Imu.update();       // full-speed sensor refresh
  server.handleClient(); // must be called often
  delay(1);
}
