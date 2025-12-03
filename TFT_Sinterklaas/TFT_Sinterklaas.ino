/*
  TFT_Sinterklaas.ino

  Purpose
  - E-reader-style demo for an ESP32S3 with TFT and capacitive touch.
  - Connects as WiFi STA to ESP32_MP3_AP and controls a remote MP3+LED module at 192.168.4.1
    - On WiFi startup: set volume to DEFAULT_VOLUME (15)
    - When the initial screen is shown: start playing track 1 (if music enabled)
    - On page change: request play?track=<page>
    - Top-left touch toggles music -> sends /stop or /play
    - Touch within the first 3 seconds after first draw will set volume to 6 and
      that touch will not cause page forward/back navigation.
    - LED effects are offloaded to the MP3+LED ESP32 via HTTP calls.

  Hardware pins (summary)
  - TFT I2C SDA:        GPIO 16
  - TFT I2C SCL:        GPIO 15
  - Touch RST_N_PIN:    GPIO 18
  - Touch INT_N_PIN:    GPIO 17
  - Serial: 115200 for debug

  Notes
  - Touch controller mapping uses tp.tp[0].y as "x" and tp.tp[0].x as "y".
    "Top-left" region on your rotated panel corresponds to touchX > 225, small Y.
*/

#include <TFT_eSPI.h>
#include <FT6336U.h>
#include <Wire.h>
#include "pictures.h"
#include <WiFi.h>
#include <HTTPClient.h>

// Display / Touch pins and screen size
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 320
#define I2C_SDA 16
#define I2C_SCL 15
#define RST_N_PIN 18
#define INT_N_PIN 17

// WiFi and remote MP3+LED module
const char* WIFI_SSID      = "ESP32_MP3_AP";
const char* WIFI_PASSWORD  = "12345678";
const char* AUDIO_BASE_URL = "http://192.168.4.1";

// Volume and touch-window config
const int DEFAULT_VOLUME = 15;
const unsigned long TOUCH_VOLUME_WINDOW_MS = 3000UL;

// Display color inversion fix
#define FIX_INVERTED_COLORS true

// LED function for each page (mapped to remote LED effects)
enum LedFunction {
  LEDFUNC_OFF = 0,
  LEDFUNC_RAINBOW = 1,
  LEDFUNC_FLASHING_YELLOW = 2,
  LEDFUNC_FLASHING_RED = 3,
  LEDFUNC_THUNDER = 4
};

// TFT and touch objects
TFT_eSPI tft = TFT_eSPI();
FT6336U ft6336u(I2C_SDA, I2C_SCL, RST_N_PIN, INT_N_PIN);
FT6336U_TouchPointType tp;

// Page animation types
enum AnimationType {
  ANIM_NONE,
  ANIM_SHAKE,
  ANIM_INVERT
};

// Page descriptor with LED function
struct PageData {
  const uint16_t* image;
  const char* text;
  uint16_t color;
  AnimationType animation;
  LedFunction ledFunc;
};

#define NUM_PAGES 8
#define TEXT_FONT 2

// Story pages: last parameter selects remote LED behavior
const PageData pages[NUM_PAGES] = {
  { page0, "", TFT_WHITE, ANIM_NONE,   LEDFUNC_OFF },
  { page1, "Introduction\n\nSwipe to start.", TFT_WHITE, ANIM_NONE,   LEDFUNC_RAINBOW },
  { page2, "Chapter 1\n\nThe Story Begins\nIt was a dark night.", TFT_WHITE, ANIM_NONE,   LEDFUNC_RAINBOW },
  { page3, "Chapter 2\n\nSomething is wrong...", TFT_WHITE, ANIM_NONE,   LEDFUNC_FLASHING_YELLOW },
  { bomb, "", TFT_RED, ANIM_SHAKE,  LEDFUNC_THUNDER },
  { jollyroger, "DANGER!\nSYSTEM FAILURE\nEVACUATE", TFT_RED, ANIM_INVERT, LEDFUNC_FLASHING_RED },
  { page4, "Chapter 3\n\nThat was close.\nWe survived.", TFT_WHITE, ANIM_NONE,   LEDFUNC_RAINBOW },
  { page5, "The End\n\nThanks for reading!", TFT_BLACK, ANIM_NONE,   LEDFUNC_OFF }
};

// Animation / state variables
unsigned long lastFrameTime = 0;
int shakeX = 0, shakeY = 0;
bool isFlashed = false;

int currentPage = 0;
bool needsUpdate = true;
bool firstDrawDone = false;
bool musicEnabled = true;
bool wifiConnected = false;

// Touch-volume window state
unsigned long firstDrawTime = 0;
bool touchVolumeWindowActive = false;

// Touch debounce
unsigned long lastTouchHandled = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 200;

// ---------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------

void httpGet(const String& url) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[HTTP] GET skipped, WiFi not connected"));
    return;
  }
  HTTPClient http;
  Serial.print(F("[HTTP] GET: "));
  Serial.println(url);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.print(F("[HTTP] Response code: "));
    Serial.println(httpCode);
    String payload = http.getString();
    if (payload.length()) {
      Serial.print(F("[HTTP] Payload: "));
      Serial.println(payload);
    }
  } else {
    Serial.print(F("[HTTP] Request failed: "));
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

// Volume helpers on remote module
void setVolume(int val) {
  Serial.print(F("[AUDIO] Setting volume to "));
  Serial.println(val);
  char url[64];
  snprintf(url, sizeof(url), "%s/volume?val=%d", AUDIO_BASE_URL, val);
  httpGet(String(url));
}
void setInitialVolume() { setVolume(DEFAULT_VOLUME); }

// Play helpers on remote module
void playTrackForPage(int pageIndex) {
  int trackNumber = pageIndex + 1;
  Serial.print(F("[AUDIO] Request play track "));
  Serial.println(trackNumber);
  if (!musicEnabled) {
    Serial.println(F("[AUDIO] Music disabled - skipping play"));
    return;
  }
  char url[64];
  snprintf(url, sizeof(url), "%s/play?track=%d", AUDIO_BASE_URL, trackNumber);
  httpGet(String(url));
}
void sendGlobalPlay() { httpGet(String(AUDIO_BASE_URL) + "/play"); }
void sendStop()       { httpGet(String(AUDIO_BASE_URL) + "/stop"); }

// ---------------------------------------------------------
// Remote LED control (calls MP3+LED module)
// ---------------------------------------------------------
void remoteSetLedEffect(LedFunction f) {
  char url[64];
  // map LedFunction 0..4 to remote LED modes 0..7; direct mapping to 0..4 used
  snprintf(url, sizeof(url), "%s/led/mode?mode=%d", AUDIO_BASE_URL, (int)f);
  httpGet(String(url));
}

// Optional: when page changes, trigger a direction-based page-wipe on LEDs
void remotePageWipeForPage(int oldPage, int newPage) {
  if (oldPage == newPage) return;
  const char* dir = (newPage > oldPage) ? "right" : "left";
  char url[96];
  // White pagewipe for now
  snprintf(url, sizeof(url), "%s/led/pagewipe?color=ffffff&dir=%s", AUDIO_BASE_URL, dir);
  httpGet(String(url));
}

// ---------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------
void drawMultilineText(const char* text, int x, int y, int font, uint16_t color) {
  if (!text || !text[0]) return;
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextFont(font);

  String s(text);
  int lineHeight = tft.fontHeight(font) + 2;
  int yOffset = y;

  int start = 0;
  while (true) {
    int idx = s.indexOf('\n', start);
    String line;
    if (idx == -1) {
      line = s.substring(start);
    } else {
      line = s.substring(start, idx);
    }
    tft.drawCentreString(line, x, yOffset, font);
    yOffset += lineHeight;
    if (idx == -1) break;
    start = idx + 1;
  }
}

void drawCurrentPage() {
  Serial.print(F("[UI] Drawing page "));
  Serial.println(currentPage);

  isFlashed = false;
  tft.invertDisplay(FIX_INVERTED_COLORS);

  // Note: original used 320x240; keep that (image orientation)
  tft.setSwapBytes(true);
  tft.pushImage(0, 0, 320, 240, pages[currentPage].image);
  tft.setSwapBytes(false);

  if (pages[currentPage].animation != ANIM_SHAKE) {
    drawMultilineText(pages[currentPage].text, tft.width() / 2, tft.height() / 2, TEXT_FONT, pages[currentPage].color);
    String pageNum = String(currentPage + 1) + "/" + String(NUM_PAGES);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString(pageNum, tft.width() / 2, tft.height() - 20, 1);
  }

  // Set LED behavior for this page on the remote module
  remoteSetLedEffect(pages[currentPage].ledFunc);

  // On first draw: enable the touch-volume window and auto-play
  if (!firstDrawDone) {
    firstDrawDone = true;
    touchVolumeWindowActive = true;
    firstDrawTime = millis();
    Serial.println(F("[UI] First draw done - touch-volume window active"));
    playTrackForPage(0); // track 1
  }
}

// Simple shake and invert animations (unchanged logic, just rely on lastFrameTime)
void performShake() {
  const unsigned long interval = 80;
  unsigned long now = millis();
  if (now - lastFrameTime > interval) {
    lastFrameTime = now;
    shakeX = random(-3, 4);
    shakeY = random(-3, 4);
    tft.pushImage(shakeX, shakeY, 320, 240, pages[currentPage].image);
  }
}

void performInvert() {
  if (millis() - lastFrameTime > 150) {
    lastFrameTime = millis();
    isFlashed = !isFlashed;
    bool state = FIX_INVERTED_COLORS ? !isFlashed : isFlashed;
    tft.invertDisplay(state);
    uint16_t flashColor = isFlashed ? TFT_YELLOW : TFT_RED;
    drawMultilineText(pages[currentPage].text, tft.width() / 2, tft.height() / 2, 4, flashColor);
  }
}

// ---------------------------------------------------------
// Page navigation
// ---------------------------------------------------------
void gotoPage(int newPage) {
  if (newPage < 0) newPage = 0;
  if (newPage >= NUM_PAGES) newPage = NUM_PAGES - 1;
  if (newPage == currentPage) {
    Serial.println(F("[NAV] No page change"));
    return;
  }

  Serial.print(F("[NAV] Page "));
  Serial.print(currentPage);
  Serial.print(F(" -> "));
  Serial.println(newPage);

  int oldPage = currentPage;
  currentPage = newPage;
  needsUpdate = true;

  // Remote LED: start a page-wipe in direction of navigation
  remotePageWipeForPage(oldPage, currentPage);

  // Remote LED: set effect for the new page (runs after wipe)
  remoteSetLedEffect(pages[currentPage].ledFunc);

  if (musicEnabled) {
    playTrackForPage(currentPage);
  } else {
    Serial.println(F("[AUDIO] Music disabled; not sending play request"));
  }
}

// ---------------------------------------------------------
// Setup & main loop
// ---------------------------------------------------------
void setup(void) {
  Serial.begin(115200);
  delay(200);

  Serial.print(F("[WiFi] Connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startAttemptTime = millis();
  const unsigned long wifiTimeout = 10000UL;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print(F("[WiFi] Connected, IP: "));
    Serial.println(WiFi.localIP());
    setInitialVolume();
  } else {
    wifiConnected = false;
    Serial.println(F("[WiFi] Connection timed out"));
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(FIX_INVERTED_COLORS);
  ft6336u.begin();

  drawCurrentPage();
}

void loop() {
  unsigned long now = millis();

  if (needsUpdate) {
    drawCurrentPage();
    needsUpdate = false;
  }

  // Per-page screen animation
  switch (pages[currentPage].animation) {
    case ANIM_SHAKE:  performShake();  break;
    case ANIM_INVERT: performInvert(); break;
    case ANIM_NONE:
    default: break;
  }

  // Touch handling
  tp = ft6336u.scan();
  if (tp.touch_count > 0) {
    if (now - lastTouchHandled < TOUCH_DEBOUNCE_MS) {
      return; // debounce
    }
    lastTouchHandled = now;

    int touchX = tp.tp[0].y; // panel mapping
    int touchY = tp.tp[0].x;
    Serial.print(F("[TOUCH] x="));
    Serial.print(touchX);
    Serial.print(F(" y="));
    Serial.println(touchY);

    // Early 3s touch-window: volume shortcut
    if (touchVolumeWindowActive && (now - firstDrawTime) <= TOUCH_VOLUME_WINDOW_MS) {
      Serial.println(F("[TOUCH] initial 3s: set volume to 6, ignore navigation"));
      setVolume(6);
      touchVolumeWindowActive = false;
      return;
    } else if (touchVolumeWindowActive && (now - firstDrawTime) > TOUCH_VOLUME_WINDOW_MS) {
      touchVolumeWindowActive = false;
      Serial.println(F("[UI] Touch-volume window expired"));
    }

    // Top-left region: toggle music, no navigation
    if (touchX > 225 && touchY >= 0 && touchY < 60) {
      Serial.println(F("[TOUCH] Top-left region tapped - toggle music"));
      musicEnabled = !musicEnabled;
      Serial.print(F("[AUDIO] Music now: "));
      Serial.println(musicEnabled ? F("ON") : F("OFF"));
      if (musicEnabled) playTrackForPage(currentPage);
      else sendStop();
      return;
    }

    // Normal left/right navigation based on panel X
    int nextPage = currentPage;
    if (touchX < SCREEN_WIDTH / 2) {
      nextPage--;
      Serial.println(F("[NAV] Left half touched -> previous"));
    } else {
      nextPage++;
      Serial.println(F("[NAV] Right half touched -> next"));
    }

    gotoPage(nextPage);
  }
}