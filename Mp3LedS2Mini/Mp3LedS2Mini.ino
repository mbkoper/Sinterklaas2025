#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>
#include <DFMiniMp3.h>

// -------------------- MP3 Player --------------------
class Mp3Notify;
typedef DFMiniMp3<HardwareSerial, Mp3Notify> DfMp3;
DfMp3 dfmp3(Serial1);

volatile uint16_t currentTrack = 0;

class Mp3Notify {
public:
  static void OnError(DfMp3& mp3, uint16_t errorCode) {
    Serial.printf("[MP3] Com Error %u\n", errorCode);
  }

  static void OnPlayFinished(DfMp3& mp3, DfMp3_PlaySources source, uint16_t track) {
    Serial.printf("[MP3] Finished track #%u, replaying track #%u\n", track, currentTrack);
    dfmp3.playMp3FolderTrack(currentTrack);
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

// LED effects controlled over HTTP
enum LedEffect {
  LED_OFF = 0,
  LED_ON_WHITE = 1,
  LED_RAINBOW = 2,
  LED_FLASHING_YELLOW = 3,
  LED_FLASHING_RED = 4,
  LED_PAGE_WIPE_LEFT = 5,
  LED_PAGE_WIPE_RIGHT = 6,
  LED_THUNDER = 7
};

LedEffect currentLedEffect = LED_OFF;

// Shared animation state
uint8_t rainbowHue = 0;
bool flashState = false;
unsigned long lastLedUpdate = 0;
const unsigned long RAINBOW_INTERVAL = 50;
const unsigned long FLASH_INTERVAL = 400;

// Page-wipe state
bool pageWipeActive = false;
int pageWipeIndex = 0;
unsigned long pageWipeLast = 0;
const unsigned long PAGE_WIPE_INTERVAL = 60;
CRGB pageWipeColor = CRGB::White;
bool pageWipeLeftToRight = true;

// Thunder state
bool thunderInFlash = false;
unsigned long thunderNextAt = 0;
unsigned long thunderFlashEnd = 0;
int thunderBurstsRemaining = 0;
const unsigned long THUNDER_MIN_GAP = 300;
const unsigned long THUNDER_MAX_GAP = 3000;
const unsigned long THUNDER_FLASH_MIN = 40;
const unsigned long THUNDER_FLASH_MAX = 180;

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
    <button onclick="fetch('/led/mode?mode=3')">Flash Yellow</button>
    <button onclick="fetch('/led/mode?mode=4')">Flash Red</button>
    <button onclick="fetch('/led/mode?mode=7')">Thunder</button>
    <button onclick="fetch('/led/pagewipe?dir=left')">PageWipe Left</button>
    <button onclick="fetch('/led/pagewipe?dir=right')">PageWipe Right</button>
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
let trackDiv = document.getElementById("trackButtons");
for (let i=1;i<=9;i++) {
  let btn = document.createElement("button");
  btn.innerText = "Play " + ("000"+i).slice(-4);
  btn.onclick = ()=>fetch('/play?track='+i);
  trackDiv.appendChild(btn);
}
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

// -------------------- LED effect helpers --------------------
void setLedEffect(LedEffect e) {
  currentLedEffect = e;
  Serial.printf("[LED] Effect set to %d\n", (int)e);

  rainbowHue = 0;
  flashState = false;
  lastLedUpdate = millis();
  pageWipeActive = false;

  switch (e) {
    case LED_OFF:
      FastLED.clear(true);
      break;
    case LED_ON_WHITE:
      fill_solid(leds, NUM_LEDS, CRGB::White);
      FastLED.show();
      break;
    case LED_RAINBOW:
      for (int i = 0; i < NUM_LEDS; ++i)
        leds[i] = CHSV((i * 255) / NUM_LEDS, 255, ledBrightness);
      FastLED.show();
      break;
    case LED_FLASHING_YELLOW:
    case LED_FLASHING_RED:
      FastLED.clear(true);
      break;
    case LED_PAGE_WIPE_LEFT:
    case LED_PAGE_WIPE_RIGHT:
      pageWipeActive = true;
      pageWipeIndex = 0;
      pageWipeLast = millis();
      pageWipeLeftToRight = (e == LED_PAGE_WIPE_LEFT);
      FastLED.clear(true);
      break;
    case LED_THUNDER:
      FastLED.clear(true);
      thunderInFlash = false;
      thunderBurstsRemaining = 0;
      thunderNextAt = millis() + random(500, 1500);
      break;
  }
}

// animate LEDs
void updateLeds() {
  unsigned long now = millis();

  // Page wipe
  if ((currentLedEffect == LED_PAGE_WIPE_LEFT || currentLedEffect == LED_PAGE_WIPE_RIGHT) && pageWipeActive) {
    if (now - pageWipeLast >= PAGE_WIPE_INTERVAL) {
      pageWipeLast = now;
      if (pageWipeIndex < NUM_LEDS) {
        for (int i = 0; i < NUM_LEDS; ++i) {
          bool lit = false;
          if (pageWipeLeftToRight) {
            // 0 -> NUM_LEDS-1
            lit = (i <= pageWipeIndex);
          } else {
            // right -> left
            int idxFromRight = NUM_LEDS - 1 - i;
            lit = (idxFromRight <= pageWipeIndex);
          }
          leds[i] = lit ? pageWipeColor : CRGB::Black;
        }
        FastLED.show();
        pageWipeIndex++;
      } else {
        pageWipeActive = false;
      }
    }
    return;
  }

  // Thunder effect
  if (currentLedEffect == LED_THUNDER) {
    if (thunderInFlash) {
      if (now >= thunderFlashEnd) {
        thunderInFlash = false;
        FastLED.clear(true);
        if (thunderBurstsRemaining > 0) {
          thunderInFlash = true;
          thunderBurstsRemaining--;
          unsigned long d = random(THUNDER_FLASH_MIN, THUNDER_FLASH_MAX);
          thunderFlashEnd = now + d;
          for (int i = 0; i < NUM_LEDS; ++i)
            leds[i] = CRGB(200, 220, 255);
          FastLED.show();
        } else {
          thunderNextAt = now + random(THUNDER_MIN_GAP, THUNDER_MAX_GAP);
        }
      }
    } else {
      if (now >= thunderNextAt) {
        thunderBurstsRemaining = random(1, 4) - 1;
        thunderInFlash = true;
        unsigned long d = random(THUNDER_FLASH_MIN, THUNDER_FLASH_MAX);
        thunderFlashEnd = now + d;
        for (int i = 0; i < NUM_LEDS; ++i)
          leds[i] = CRGB(200, 220, 255);
        FastLED.show();
      }
    }
    return;
  }

  // Rainbow
  if (currentLedEffect == LED_RAINBOW) {
    if (now - lastLedUpdate >= RAINBOW_INTERVAL) {
      lastLedUpdate = now;
      rainbowHue++;
      for (int i = 0; i < NUM_LEDS; ++i)
        leds[i] = CHSV(rainbowHue + (i * 255) / NUM_LEDS, 255, ledBrightness);
      FastLED.show();
    }
    return;
  }

  // Flashing yellow / red
  if (currentLedEffect == LED_FLASHING_YELLOW || currentLedEffect == LED_FLASHING_RED) {
    if (now - lastLedUpdate >= FLASH_INTERVAL) {
      lastLedUpdate = now;
      flashState = !flashState;
      if (flashState) {
        CRGB c = (currentLedEffect == LED_FLASHING_YELLOW) ? CRGB::Yellow : CRGB::Red;
        fill_solid(leds, NUM_LEDS, c);
      } else {
        FastLED.clear();
      }
      FastLED.show();
    }
    return;
  }

  // LED_ON_WHITE / LED_OFF: static
}

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

// Simple LED endpoints mapping to effects
void handleLedOn() {
  setLedEffect(LED_ON_WHITE);
  server.send(200, "application/json", "{\"status\":\"leds on\"}");
}
void handleLedOff() {
  setLedEffect(LED_OFF);
  server.send(200, "application/json", "{\"status\":\"leds off\"}");
}
void handleLedRainbow() {
  setLedEffect(LED_RAINBOW);
  server.send(200, "application/json", "{\"status\":\"rainbow\"}");
}

// Static color (no animation)
void handleLedSetColor() {
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    int r = constrain(server.arg("r").toInt(), 0, 255);
    int g = constrain(server.arg("g").toInt(), 0, 255);
    int b = constrain(server.arg("b").toInt(), 0, 255);
    setLedEffect(LED_OFF);
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

// /led/mode?mode=N  (0..7) used by TFT board
void handleLedModeApi() {
  if (!server.hasArg("mode")) {
    server.send(400, "application/json", "{\"error\":\"missing mode\"}");
    return;
  }
  int m = server.arg("mode").toInt();
  if (m < LED_OFF || m > LED_THUNDER) {
    server.send(400, "application/json", "{\"error\":\"invalid mode\"}");
    return;
  }
  setLedEffect((LedEffect)m);
  server.send(200, "application/json", "{\"status\":\"mode set\"}");
}

// /led/pagewipe?color=rrggbb&dir=left|right
void handleLedPageWipe() {
  String hex = "ffffff";
  if (server.hasArg("color")) {
    hex = server.arg("color");
    hex.replace("#", "");
  }
  if (hex.length() != 6) hex = "ffffff";
  long rgb = strtol(hex.c_str(), NULL, 16);
  int r = (rgb >> 16) & 0xFF;
  int g = (rgb >> 8) & 0xFF;
  int b = rgb & 0xFF;
  pageWipeColor = CRGB(r,g,b);

  bool leftToRight = true;
  if (server.hasArg("dir")) {
    String d = server.arg("dir");
    d.toLowerCase();
    if (d == "right") leftToRight = false;
  }
  pageWipeLeftToRight = leftToRight;
  setLedEffect(leftToRight ? LED_PAGE_WIPE_LEFT : LED_PAGE_WIPE_RIGHT);

  server.send(200, "application/json", "{\"status\":\"page wipe started\"}");
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

  dfmp3.begin(16,17); 
  dfmp3.reset(); 
  dfmp3.setVolume(12);
  Serial.println("[MP3] DFPlayer initialized");

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(ledBrightness);
  FastLED.clear(true);
  setLedEffect(LED_OFF);
  Serial.println("[LED] FastLED initialized");

  WiFi.softAP(ssid,password);
  Serial.print("[WiFi] AP started, IP: "); 
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/play", handlePlay);
  server.on("/stop", handleStop);
  server.on("/volume", handleVolume);

  server.on("/led/on", handleLedOn);
  server.on("/led/off", handleLedOff);
  server.on("/led/rainbow", handleLedRainbow);
  server.on("/led/setcolor", handleLedSetColor);
  server.on("/led/intensity", handleLedIntensity);
  server.on("/led/mode", handleLedModeApi);
  server.on("/led/pagewipe", handleLedPageWipe);

  server.on("/status", handleStatus);

  server.begin();
  Serial.println("[HTTP] Web server started");
}

// -------------------- Loop --------------------
void loop() {
  server.handleClient();
  dfmp3.loop();
  updateLeds();
}