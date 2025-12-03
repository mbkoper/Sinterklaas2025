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
constexpr unsigned long RAINBOW_INTERVAL = 50;
constexpr unsigned long FLASH_INTERVAL   = 400;

// Page-wipe state
bool pageWipeActive = false;
int pageWipeIndex = 0;
unsigned long pageWipeLast = 0;
constexpr unsigned long PAGE_WIPE_INTERVAL = 60;
CRGB pageWipeColor = CRGB::White;
bool pageWipeLeftToRight = true;

// Thunder state
bool thunderInFlash = false;
unsigned long thunderNextAt = 0;
unsigned long thunderFlashEnd = 0;
int thunderBurstsRemaining = 0;
constexpr unsigned long THUNDER_MIN_GAP   = 300;
constexpr unsigned long THUNDER_MAX_GAP   = 3000;
constexpr unsigned long THUNDER_FLASH_MIN = 40;
constexpr unsigned long THUNDER_FLASH_MAX = 180;

// -------------------- WiFi + WebServer --------------------
WebServer server(80);
const char* ssid     = "ESP32_MP3_AP";
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
    <button onclick="setColor(255,0,0)">Red</button>
    <button onclick="setColor(0,255,0)">Green</button>
    <button onclick="setColor(0,0,255)">Blue</button>
    <br><br>
    <label>Brightness:</label>
    <input type="range" min="0" max="255" id="intSlider"
           oninput="setIntensity(this.value)">
  </div>

  <script>
    function setVolume(v) {
      fetch('/volume?val=' + v);
      document.getElementById('volVal').innerText = v;
    }
    function setColor(r,g,b) {
      fetch('/led/setcolor?r='+r+'&g='+g+'&b='+b);
    }
    function setIntensity(v) {
      fetch('/led/intensity?val=' + v);
    }
    function refreshStatus() {
      fetch('/status').then(r => r.json()).then(j => {
        document.getElementById('volVal').innerText = j.volume;
        document.getElementById('statusTrack').innerText = j.track;
        document.getElementById('volSlider').value = j.volume;
        document.getElementById('intSlider').value = j.brightness;
      }).catch(e => console.log(e));
    }
    function buildTrackButtons() {
      const container = document.getElementById('trackButtons');
      for (let i=1;i<=9;i++) {
        const b = document.createElement('button');
        b.innerText = 'Track ' + i;
        b.onclick = () => fetch('/play?track=' + i);
        container.appendChild(b);
      }
    }
    buildTrackButtons();
    refreshStatus();
    setInterval(refreshStatus, 1000);
  </script>
</body>
</html>
)rawliteral";

// -------------------- JSON helpers --------------------
const char JSON_ERR_MISSING_TRACK[]   PROGMEM = "{\"error\":\"missing track arg\"}";
const char JSON_ERR_INVALID_TRACK[]   PROGMEM = "{\"error\":\"invalid track (1..9)\"}";
const char JSON_ERR_MISSING_VAL[]     PROGMEM = "{\"error\":\"missing val arg\"}";
const char JSON_ERR_MISSING_RGB[]     PROGMEM = "{\"error\":\"missing r,g,b\"}";
const char JSON_ERR_MISSING_MODE[]    PROGMEM = "{\"error\":\"missing mode\"}";
const char JSON_ERR_INVALID_MODE[]    PROGMEM = "{\"error\":\"invalid mode\"}";
const char JSON_ERR_MISSING_COLOR[]   PROGMEM = "{\"error\":\"missing color\"}";
const char JSON_STATUS_PLAYING[]      PROGMEM = "{\"status\":\"playing\"}";
const char JSON_STATUS_STOPPED[]      PROGMEM = "{\"status\":\"stopped\"}";
const char JSON_STATUS_LEDS_ON[]      PROGMEM = "{\"status\":\"leds on\"}";
const char JSON_STATUS_LEDS_OFF[]     PROGMEM = "{\"status\":\"leds off\"}";
const char JSON_STATUS_RAINBOW[]      PROGMEM = "{\"status\":\"rainbow\"}";
const char JSON_STATUS_COLOR_SET[]    PROGMEM = "{\"status\":\"color set\"}";
const char JSON_STATUS_INTENSITY_SET[]PROGMEM = "{\"status\":\"intensity set\"}";
const char JSON_STATUS_MODE_SET[]     PROGMEM = "{\"status\":\"mode set\"}";
const char JSON_STATUS_PAGEWIPE[]     PROGMEM = "{\"status\":\"page wipe started\"}";

inline void sendJsonP200(const char* json) {
  server.send_P(200, "application/json", json);
}
inline void sendJsonP400(const char* json) {
  server.send_P(400, "application/json", json);
}

// -------------------- LED effect control --------------------
void setLedEffect(LedEffect effect) {
  currentLedEffect = effect;
  pageWipeActive = false;
  thunderInFlash = false;
  thunderBurstsRemaining = 0;

  switch (effect) {
    case LED_OFF:
      FastLED.clear(true);
      break;
    case LED_ON_WHITE:
      fill_solid(leds, NUM_LEDS, CRGB::White);
      FastLED.show();
      break;
    case LED_RAINBOW:
      // will animate in updateLeds
      break;
    case LED_FLASHING_YELLOW:
    case LED_FLASHING_RED:
      // will animate in updateLeds
      break;
    case LED_PAGE_WIPE_LEFT:
    case LED_PAGE_WIPE_RIGHT:
      pageWipeActive = true;
      pageWipeIndex = 0;
      pageWipeLast = millis();
      break;
    case LED_THUNDER:
      thunderBurstsRemaining = random(3, 7);
      thunderNextAt = millis() + random(THUNDER_MIN_GAP, THUNDER_MAX_GAP);
      break;
  }
}

void updateLeds() {
  const unsigned long now = millis();
  bool changed = false;

  switch (currentLedEffect) {
    case LED_OFF:
      // Nothing to do, already cleared when effect set
      break;

    case LED_ON_WHITE:
      // Static, no animation
      break;

    case LED_RAINBOW:
      if (now - lastLedUpdate >= RAINBOW_INTERVAL) {
        lastLedUpdate = now;
        fill_rainbow(leds, NUM_LEDS, rainbowHue++);
        changed = true;
      }
      break;

    case LED_FLASHING_YELLOW:
    case LED_FLASHING_RED:
      if (now - lastLedUpdate >= FLASH_INTERVAL) {
        lastLedUpdate = now;
        flashState = !flashState;
        CRGB color = (currentLedEffect == LED_FLASHING_YELLOW) ? CRGB::Yellow : CRGB::Red;
        fill_solid(leds, NUM_LEDS, flashState ? color : CRGB::Black);
        changed = true;
      }
      break;

    case LED_PAGE_WIPE_LEFT:
    case LED_PAGE_WIPE_RIGHT:
      if (pageWipeActive && now - pageWipeLast >= PAGE_WIPE_INTERVAL) {
        pageWipeLast = now;
        // clear all first
        FastLED.clear(false);
        if (pageWipeLeftToRight) {
          for (int i = 0; i <= pageWipeIndex && i < NUM_LEDS; ++i) {
            leds[i] = pageWipeColor;
          }
        } else {
          for (int i = 0; i <= pageWipeIndex && i < NUM_LEDS; ++i) {
            leds[NUM_LEDS - 1 - i] = pageWipeColor;
          }
        }
        pageWipeIndex++;
        if (pageWipeIndex >= NUM_LEDS) {
          pageWipeActive = false;
        }
        changed = true;
      }
      break;

    case LED_THUNDER:
      if (!thunderInFlash) {
        if (thunderBurstsRemaining > 0 && now >= thunderNextAt) {
          thunderInFlash = true;
          thunderFlashEnd = now + random(THUNDER_FLASH_MIN, THUNDER_FLASH_MAX);
          fill_solid(leds, NUM_LEDS, CRGB::White);
          changed = true;
        }
      } else {
        if (now >= thunderFlashEnd) {
          thunderInFlash = false;
          thunderBurstsRemaining--;
          FastLED.clear(false);
          changed = true;
          if (thunderBurstsRemaining > 0) {
            thunderNextAt = now + random(THUNDER_MIN_GAP, THUNDER_MAX_GAP);
          }
        }
      }
      break;
  }

  if (changed) {
    FastLED.show();
  }
}

// -------------------- API Handlers --------------------
void handleRoot() {
  server.send_P(200, "text/html", MAIN_page);
}

void handlePlay() {
  if (!server.hasArg("track")) {
    sendJsonP400(JSON_ERR_MISSING_TRACK);
    return;
  }
  uint16_t track = server.arg("track").toInt();
  if (track < 1 || track > 9) {
    sendJsonP400(JSON_ERR_INVALID_TRACK);
    return;
  }
  currentTrack = track;
  Serial.printf("[MP3] Playing track %u\n", currentTrack);
  dfmp3.playMp3FolderTrack(currentTrack);
  sendJsonP200(JSON_STATUS_PLAYING);
}

void handleStop() {
  dfmp3.stop();
  Serial.println(F("[MP3] Stopped playback"));
  sendJsonP200(JSON_STATUS_STOPPED);
}

void handleVolume() {
  if (!server.hasArg("val")) {
    sendJsonP400(JSON_ERR_MISSING_VAL);
    return;
  }
  int vol = constrain(server.arg("val").toInt(), 0, 30);
  dfmp3.setVolume(vol);
  Serial.printf("[MP3] Volume set to %d\n", vol);
  sendJsonP200(JSON_STATUS_PLAYING);
}

// Simple LED endpoints mapping to effects
void handleLedOn() {
  setLedEffect(LED_ON_WHITE);
  sendJsonP200(JSON_STATUS_LEDS_ON);
}
void handleLedOff() {
  setLedEffect(LED_OFF);
  sendJsonP200(JSON_STATUS_LEDS_OFF);
}
void handleLedRainbow() {
  setLedEffect(LED_RAINBOW);
  sendJsonP200(JSON_STATUS_RAINBOW);
}

// Static color (no animation)
void handleLedSetColor() {
  if (!(server.hasArg("r") && server.hasArg("g") && server.hasArg("b"))) {
    sendJsonP400(JSON_ERR_MISSING_RGB);
    return;
  }
  int r = constrain(server.arg("r").toInt(), 0, 255);
  int g = constrain(server.arg("g").toInt(), 0, 255);
  int b = constrain(server.arg("b").toInt(), 0, 255);
  setLedEffect(LED_OFF);
  fill_solid(leds, NUM_LEDS, CRGB(r,g,b));
  FastLED.show();
  sendJsonP200(JSON_STATUS_COLOR_SET);
}

void handleLedIntensity() {
  if (!server.hasArg("val")) {
    sendJsonP400(JSON_ERR_MISSING_VAL);
    return;
  }
  ledBrightness = constrain(server.arg("val").toInt(), 0, 255);
  FastLED.setBrightness(ledBrightness);
  FastLED.show();
  sendJsonP200(JSON_STATUS_INTENSITY_SET);
}

// /led/mode?mode=N  (0..7) used by TFT board
void handleLedModeApi() {
  if (!server.hasArg("mode")) {
    sendJsonP400(JSON_ERR_MISSING_MODE);
    return;
  }
  int m = server.arg("mode").toInt();
  if (m < LED_OFF || m > LED_THUNDER) {
    sendJsonP400(JSON_ERR_INVALID_MODE);
    return;
  }
  setLedEffect(static_cast<LedEffect>(m));
  sendJsonP200(JSON_STATUS_MODE_SET);
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
  sendJsonP200(JSON_STATUS_PAGEWIPE);
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
  Serial.println(F("[SYS] Initializing..."));

  dfmp3.begin(16,17); 
  dfmp3.reset(); 
  dfmp3.setVolume(12);
  Serial.println(F("[MP3] DFPlayer initialized"));

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(ledBrightness);
  FastLED.clear(true);
  setLedEffect(LED_OFF);
  Serial.println(F("[LED] FastLED initialized"));

  WiFi.softAP(ssid, password);
  Serial.print(F("[WiFi] AP started, IP: ")); 
  Serial.println(WiFi.softAPIP());

  server.on("/",           handleRoot);
  server.on("/play",       handlePlay);
  server.on("/stop",       handleStop);
  server.on("/volume",     handleVolume);

  server.on("/led/on",        handleLedOn);
  server.on("/led/off",       handleLedOff);
  server.on("/led/rainbow",   handleLedRainbow);
  server.on("/led/setcolor",  handleLedSetColor);
  server.on("/led/intensity", handleLedIntensity);
  server.on("/led/mode",      handleLedModeApi);
  server.on("/led/pagewipe",  handleLedPageWipe);

  server.on("/status", handleStatus);

  server.begin();
  Serial.println(F("[HTTP] Web server started"));
}

// -------------------- Loop --------------------
void loop() {
  server.handleClient();
  dfmp3.loop();
  updateLeds();
}