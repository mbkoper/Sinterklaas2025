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
#define NUM_LEDS 8        // 8 LEDs total
CRGB leds[NUM_LEDS];

// Global base brightness (0..255). Normal effects run at 50% of this.
// Flash effects run at full 255 regardless.
uint8_t ledBrightness = 128;

// For wipe animations, assume 4 LEDs left and 4 LEDs right, wired serially.
const uint8_t LEFT_COL[]  = { 0, 1, 2, 3 };
const uint8_t RIGHT_COL[] = { 4, 5, 6, 7 };
const uint8_t COL_COUNT = 4;

// Maximum total wipe duration (ms) for both columns combined.
const uint16_t WIPE_TOTAL_MS = 300;

// -------------------- Rainbow animation state --------------------
bool rainbowActive = false;
uint8_t rainbowHueBase = 0;
unsigned long lastRainbowUpdate = 0;
const uint16_t RAINBOW_INTERVAL_MS = 30;  // update every 30 ms

// -------------------- Continuous flash state --------------------
bool flashActive = false;
CRGB flashColor = CRGB::White;
bool flashOn = false;
unsigned long lastFlashToggle = 0;
const uint16_t FLASH_INTERVAL_MS = 150;  // 150 ms ON, 150 ms OFF

// -------------------- WiFi + WebServer --------------------
WebServer server(80);
const char* ssid = "ESP32_MP3_AP";
const char* password = "12345678";

// -------------------- Helper functions --------------------

// Stop all ongoing animated modes (rainbow, flash)
void stopAnimations() {
  rainbowActive = false;
  flashActive = false;
}

// Effective value for normal effects (50% of user brightness) or full (255) for flash.
static uint8_t effectiveValue(bool full = false) {
  if (full) return 255;
  return (uint8_t)(ledBrightness / 2);
}

// Animate a column sequentially (used for wipes).
void animateColumnSequential(const uint8_t* indices, uint8_t count, uint8_t maxV, int steps, int stepDelayMs) {
  for (uint8_t i = 0; i < count; ++i) {
    // Previous LEDs in column at full brightness
    for (uint8_t p = 0; p < i; ++p) {
      leds[indices[p]] = CRGB(0, maxV, 0);
    }
    // Fade current LED
    for (int s = 0; s <= steps; ++s) {
      uint8_t g = (uint32_t)maxV * s / steps;
      leds[indices[i]] = CRGB(0, g, 0);
      FastLED.show();
      delay(stepDelayMs);
    }
    leds[indices[i]] = CRGB(0, maxV, 0);
  }
}

// -------------------- API Handlers --------------------

// Root handler – no web page, just a simple JSON info message.
void handleRoot() {
  server.send(200, "application/json", "{\"status\":\"ok\",\"info\":\"ESP32 MP3 + LED API\"}");
}

void handlePlay() {
  if (!server.hasArg("track")) {
    server.send(400, "application/json", "{\"error\":\"missing track arg\"}");
    return;
  }

  uint16_t track = server.arg("track").toInt();
  if (track < 1 || track > 9) {
    server.send(400, "application/json", "{\"error\":\"invalid track (1..9)\"}");
    return;
  }

  currentTrack = track;
  Serial.printf("[MP3] Playing track %u\n", currentTrack);

  // Immediately acknowledge (non-blocking to caller)
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  // Perform action after responding
  dfmp3.playMp3FolderTrack(currentTrack);
}

void handleStop() {
  // Immediately acknowledge
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  dfmp3.stop();
  Serial.println("[MP3] Stopped playback");
}

void handleVolume() {
  if (!server.hasArg("val")) {
    server.send(400, "application/json", "{\"error\":\"missing val arg\"}");
    return;
  }

  int vol = constrain(server.arg("val").toInt(), 0, 30);
  Serial.printf("[MP3] Volume set to %d\n", vol);

  // Immediately acknowledge
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  dfmp3.setVolume(vol);
}

// /led/on – simple solid white (no long animation)
void handleLedOn() {
  server.send(200, "application/json", "{\"status\":\"ok\"}");
  stopAnimations();
  fill_solid(leds, NUM_LEDS, CRGB::White);
  FastLED.show();
}

// /led/off – clear all LEDs
void handleLedOff() {
  server.send(200, "application/json", "{\"status\":\"ok\"}");
  stopAnimations();
  FastLED.clear();
  FastLED.show();
}

// /led/rainbow – start animated rainbow at 50% of base brightness
void handleLedRainbow() {
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  stopAnimations();
  rainbowActive = true;
  rainbowHueBase = 0;
  lastRainbowUpdate = 0;
}

// /led/setcolor?r=R&g=G&b=B – solid color scaled to 50% of base brightness
void handleLedSetColor() {
  if (!server.hasArg("r") || !server.hasArg("g") || !server.hasArg("b")) {
    server.send(400, "application/json", "{\"error\":\"missing r,g,b\"}");
    return;
  }

  server.send(200, "application/json", "{\"status\":\"ok\"}");

  stopAnimations();

  int r = constrain(server.arg("r").toInt(), 0, 255);
  int g = constrain(server.arg("g").toInt(), 0, 255);
  int b = constrain(server.arg("b").toInt(), 0, 255);

  uint8_t v = effectiveValue(false);
  uint8_t sr = (uint32_t)r * v / 255;
  uint8_t sg = (uint32_t)g * v / 255;
  uint8_t sb = (uint32_t)b * v / 255;

  fill_solid(leds, NUM_LEDS, CRGB(sr, sg, sb));
  FastLED.show();
}

// /led/intensity?val=N – update base brightness (doesn't change patterns immediately)
void handleLedIntensity() {
  if (!server.hasArg("val")) {
    server.send(400, "application/json", "{\"error\":\"missing val\"}");
    return;
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  ledBrightness = constrain(server.arg("val").toInt(), 0, 255);
  FastLED.setBrightness(255);  // keep hardware full; we scale in software
  // Current pattern (rainbow/flash/solid) will adapt on next update
}

// /led/wipe_right – left 4 LEDs then right 4 LEDs wipe in green at 50%,
// with total wipe duration ≈ WIPE_TOTAL_MS (300 ms) for both columns.
void handleLedWipeRight() {
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  stopAnimations();

  uint8_t v = effectiveValue(false);
  const int steps = 20;
  uint32_t perColumnMs = WIPE_TOTAL_MS / 2;  // 150 ms per column
  int32_t stepDelayMs = perColumnMs / (COL_COUNT * (steps + 1));
  if (stepDelayMs < 1) stepDelayMs = 1;

  animateColumnSequential(LEFT_COL, COL_COUNT, v, steps, stepDelayMs);
  animateColumnSequential(RIGHT_COL, COL_COUNT, v, steps, stepDelayMs);

  for (uint8_t i = 0; i < COL_COUNT; ++i) {
    leds[LEFT_COL[i]]  = CRGB(0, v, 0);
    leds[RIGHT_COL[i]] = CRGB(0, v, 0);
  }
  FastLED.show();
}

// /led/wipe_left – right 4 LEDs then left 4 LEDs wipe in green at 50%,
// with total wipe duration ≈ WIPE_TOTAL_MS (300 ms) for both columns.
void handleLedWipeLeft() {
  server.send(200, "application/json", "{\"status\":\"ok\"}");

  stopAnimations();

  uint8_t v = effectiveValue(false);
  const int steps = 20;
  uint32_t perColumnMs = WIPE_TOTAL_MS / 2;  // 150 ms per column
  int32_t stepDelayMs = perColumnMs / (COL_COUNT * (steps + 1));
  if (stepDelayMs < 1) stepDelayMs = 1;

  animateColumnSequential(RIGHT_COL, COL_COUNT, v, steps, stepDelayMs);
  animateColumnSequential(LEFT_COL, COL_COUNT, v, steps, stepDelayMs);

  for (uint8_t i = 0; i < COL_COUNT; ++i) {
    leds[LEFT_COL[i]]  = CRGB(0, v, 0);
    leds[RIGHT_COL[i]] = CRGB(0, v, 0);
  }
  FastLED.show();
}

// /led/flash?color=red|white – start continuous flash at 100% intensity
// in requested color, 150 ms ON / 150 ms OFF, until next LED command.
void handleLedFlash() {
  String color = "white";
  if (server.hasArg("color")) {
    color = server.arg("color");
    color.toLowerCase();
  }

  server.send(200, "application/json", "{\"status\":\"ok\"}");

  stopAnimations();

  if (color == "red") {
    flashColor = CRGB::Red;
  } else {
    flashColor = CRGB::White;
  }

  flashActive = true;
  flashOn = false;              // start from OFF, will toggle to ON in loop()
  lastFlashToggle = 0;          // force immediate toggle
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
  dfmp3.setVolume(12);
  Serial.println("[MP3] DFPlayer initialized");

  // LED init
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255);
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
  server.on("/led/wipe_right", handleLedWipeRight);
  server.on("/led/wipe_left", handleLedWipeLeft);
  server.on("/led/flash", handleLedFlash);
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

  unsigned long now = millis();

  // Animate rainbow non-blockingly if active, evenly across NUM_LEDS (=8)
  if (rainbowActive) {
    if (now - lastRainbowUpdate >= RAINBOW_INTERVAL_MS) {
      lastRainbowUpdate = now;
      rainbowHueBase++;

      uint8_t v = effectiveValue(false);
      for (int i = 0; i < NUM_LEDS; i++) {
        // Even spacing around the full hue circle: 0,32,64,...,224
        uint8_t hue = rainbowHueBase + (i * 256 / NUM_LEDS);
        leds[i] = CHSV(hue, 255, v);
      }
      FastLED.show();
    }
  }

  // Animate continuous flash non-blockingly if active
  if (flashActive) {
    if (now - lastFlashToggle >= FLASH_INTERVAL_MS) {
      lastFlashToggle = now;
      flashOn = !flashOn;

      if (flashOn) {
        fill_solid(leds, NUM_LEDS, flashColor);
        FastLED.setBrightness(255);
      } else {
        FastLED.clear();
      }
      FastLED.show();
    }
  }
}