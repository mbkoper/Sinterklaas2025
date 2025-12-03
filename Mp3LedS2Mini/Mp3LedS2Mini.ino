#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>
#include <DFMiniMp3.h>

// -------------------- MP3 Player --------------------
class Mp3Notify;
typedef DFMiniMp3<HardwareSerial, Mp3Notify> DfMp3;
DfMp3 dfmp3(Serial1);

volatile uint16_t currentTrack = 0;  // track currently playing

class Mp3Notify {
public:
  static void OnError(DfMp3& mp3, uint16_t errorCode) {
    Serial.printf("[MP3] Com Error %u\n", errorCode);
  }

  // Replay the same track when finished
  static void OnPlayFinished(DfMp3& mp3, DfMp3_PlaySources source, uint16_t track) {
    Serial.printf("[MP3] Finished track #%u, replaying track #%u\n", track, currentTrack);
    // Do NOT change currentTrack here — it is set when /play is called
    dfmp3.playMp3FolderTrack(currentTrack);  // replay exactly the same track
  }
  
  static void OnPlaySourceOnline(DfMp3& mp3, DfMp3_PlaySources source) {
    Serial.printf("[MP3] Source online: %u\n", (uint16_t)source);
  }
  static void OnPlaySourceInserted(DfMp3& mp3, DfMp3_PlaySources source) {
    Serial.printf("[MP3] Source inserted: %u\n", (uint16_t)source);
  }
  static void OnPlaySourceRemoved(DfMp3& mp3, DfMp3_PlaySources source) {
    Serial.printf("[MP3] Source removed: %u\n", (uint16_t)source);
  }
};

// -------------------- LED Setup --------------------
#define LED_PIN 18
#define NUM_LEDS 30
CRGB leds[NUM_LEDS];
uint8_t ledBrightness = 128;

// -------------------- WiFi + WebServer --------------------
WebServer server(80);
const char* ssid = "ESP32_MP3_AP";
const char* password = "12345678";

// -------------------- Web UI --------------------
const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>ESP32 MP3 + LED Controller</title>
  <style>
    body { font-family: Arial; margin: 20px; background: #f4f4f4; }
    h2 { margin-top: 30px; }
    button { margin: 5px; padding: 10px 15px; font-size: 14px; }
    input[type=range] { width: 250px; vertical-align: middle; }
    .section { background: #fff; padding: 15px; margin-bottom: 20px; border-radius: 8px; box-shadow: 0 0 5px rgba(0,0,0,0.1); }
    .inline { display: inline-block; margin-left: 10px; min-width: 40px; text-align: right; }
  </style>
</head>
<body>
  <h1>ESP32 MP3 + LED Controller</h1>

  <div class="section">
    <h2>MP3 Controls</h2>
    <div id="trackButtons"></div>
    <button onclick="fetch('/stop')">Stop</button>
    <br><br>
    <label>Volume:</label>
    <input type="range" min="0" max="30" id="volSlider"
           oninput="setVolume(this.value)">
    <span class="inline" id="volVal">-</span>
    <div style="margin-top:8px;">Current track: <span id="statusTrack">-</span></div>
  </div>

  <div class="section">
    <h2>LED Controls</h2>
    <button onclick="fetch('/led/on')">On (White)</button>
    <button onclick="fetch('/led/off')">Off</button>
    <button onclick="fetch('/led/rainbow')">Rainbow</button>
    <br><br>
    <label>Color:</label>
    <input type="color" id="colorPicker" value="#ff0000"
           onchange="setColor(this.value)">
    <br><br>
    <label>Brightness:</label>
    <input type="range" min="0" max="255" id="brightSlider"
           oninput="setBrightness(this.value)">
    <span class="inline" id="brightVal">-</span>
  </div>

  <div class="section">
    <h2>Status</h2>
    <p>Volume: <span id="statusVol">-</span></p>
    <p>Brightness: <span id="statusBright">-</span></p>
  </div>

<script>
function setColor(hex) {
  let r = parseInt(hex.substr(1,2),16);
  let g = parseInt(hex.substr(3,2),16);
  let b = parseInt(hex.substr(5,2),16);
  fetch(`/led/setcolor?r=${r}&g=${g}&b=${b}`);
}
function setVolume(v) {
  fetch('/volume?val='+v).then(()=>{ document.getElementById('volVal').innerText = v; });
}
function setBrightness(v) {
  fetch('/led/intensity?val='+v).then(()=>{ document.getElementById('brightVal').innerText = v; });
}
// Generate track buttons 0001–0009
let trackDiv = document.getElementById("trackButtons");
for (let i=1;i<=9;i++) {
  let btn = document.createElement("button");
  btn.innerText = "Play " + ("000"+i).slice(-4);
  btn.onclick = ()=>fetch('/play?track='+i);
  trackDiv.appendChild(btn);
}
// Poll status every 2s
function refreshStatus() {
  fetch('/status').then(r=>r.json()).then(data=>{
    document.getElementById("statusVol").innerText = data.volume;
    document.getElementById("statusBright").innerText = data.brightness;
    document.getElementById("statusTrack").innerText = ("000"+data.track).slice(-4);
    document.getElementById("volVal").innerText = data.volume;
    document.getElementById("brightVal").innerText = data.brightness;
    document.getElementById("volSlider").value = data.volume;
    document.getElementById("brightSlider").value = data.brightness;
  }).catch(()=>{});
}
refreshStatus();
setInterval(refreshStatus, 2000);
</script>
</body>
</html>
)rawliteral";

// -------------------- API Handlers --------------------
void handleRoot() { server.send_P(200, "text/html", MAIN_page); }

void handlePlay() {
  if (server.hasArg("track")) {
    uint16_t track = server.arg("track").toInt();
    if (track < 1 || track > 9) {
      server.send(400, "application/json", "{\"error\":\"invalid track (1..9)\"}");
      return;
    }
    currentTrack = track;
    Serial.printf("[MP3] Playing track %u\n", currentTrack);
    dfmp3.playMp3FolderTrack(currentTrack);
    server.send(200, "application/json", "{\"status\":\"playing\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing track arg\"}");
  }
}

void handleStop() {
  dfmp3.stop();
  Serial.println("[MP3] Stopped playback");
  server.send(200, "application/json", "{\"status\":\"stopped\"}");
}

void handleVolume() {
  if (server.hasArg("val")) {
    int vol = constrain(server.arg("val").toInt(), 0, 30);
    dfmp3.setVolume(vol);
    Serial.printf("[MP3] Volume set to %d\n", vol);
    server.send(200, "application/json", "{\"status\":\"volume set\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing val arg\"}");
  }
}

void handleLedOn() {
  fill_solid(leds, NUM_LEDS, CRGB::White);
  FastLED.show();
  server.send(200, "application/json", "{\"status\":\"leds on\"}");
}
void handleLedOff() {
  FastLED.clear();
  FastLED.show();
  server.send(200, "application/json", "{\"status\":\"leds off\"}");
}
void handleLedRainbow() {
  for (int i=0;i<NUM_LEDS;i++) leds[i]=CHSV((i*255)/NUM_LEDS,255,ledBrightness);
  FastLED.show();
  server.send(200, "application/json", "{\"status\":\"rainbow\"}");
}
void handleLedSetColor() {
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    int r = constrain(server.arg("r").toInt(), 0, 255);
    int g = constrain(server.arg("g").toInt(), 0, 255);
    int b = constrain(server.arg("b").toInt(), 0, 255);
    fill_solid(leds, NUM_LEDS, CRGB(r,g,b));
    FastLED.show();
    server.send(200, "application/json", "{\"status\":\"color set\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing r,g,b\"}");
  }
}

void handleLedIntensity() {
  if (server.hasArg("val")) {
    ledBrightness = constrain(server.arg("val").toInt(), 0, 255);
    FastLED.setBrightness(ledBrightness);
    FastLED.show();
    server.send(200, "application/json", "{\"status\":\"intensity set\"}");
  } else {
    server.send(400, "application/json", "{\"error\":\"missing val\"}");
  }
}

void handleStatus() {
  int vol = dfmp3.getVolume();
  int bright = ledBrightness;
  String json = "{\"volume\":" + String(vol) +
                ",\"brightness\":" + String(bright) +
                ",\"track\":" + String(currentTrack) + "}";
  server.send(200, "application/json", json);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  Serial.println("[SYS] Initializing...");

  // MP3 init
  dfmp3.begin(16,17); 
  dfmp3.reset(); 
  dfmp3.setVolume(12);   // initial volume set to 12
  Serial.println("[MP3] DFPlayer initialized");

  // LED init
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(ledBrightness);
  FastLED.clear(true);
  Serial.println("[LED] FastLED initialized");

  // WiFi AP
  WiFi.softAP(ssid,password);
  Serial.print("[WiFi] AP started, IP: "); 
  Serial.println(WiFi.softAPIP());

  // WebServer routes
  server.on("/", handleRoot);
  server.on("/play", handlePlay);
  server.on("/stop", handleStop);
  server.on("/volume", handleVolume);
  server.on("/led/on", handleLedOn);
  server.on("/led/off", handleLedOff);
  server.on("/led/rainbow", handleLedRainbow);
  server.on("/led/setcolor", handleLedSetColor);
  server.on("/led/intensity", handleLedIntensity);
  server.on("/status", handleStatus);

  server.begin();
  Serial.println("[HTTP] Web server started");
}

// -------------------- Loop --------------------
void loop() {
  server.handleClient();
  dfmp3.loop(); // process DFPlayer notifications
}