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
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define I2C_SDA 16
#define I2C_SCL 15
#define RST_N_PIN 18
#define INT_N_PIN 17

// WiFi and remote MP3+LED module
const char* WIFI_SSID     = "ESP32_MP3_AP";
const char* WIFI_PASSWORD = "12345678";
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
  { page0, "", TFT_WHITE, ANIM_NONE, LEDFUNC_OFF },
  { page1, "Introduction\n\nSwipe to start.", TFT_WHITE, ANIM_NONE, LEDFUNC_RAINBOW },
  { page2, "Chapter 1\n\nThe Story Begins\nIt was a dark night.", TFT_WHITE, ANIM_NONE, LEDFUNC_RAINBOW },
  { page3, "Chapter 2\n\nSomething is wrong...", TFT_WHITE, ANIM_NONE, LEDFUNC_FLASHING_YELLOW },
  { bomb, "", TFT_RED, ANIM_SHAKE, LEDFUNC_THUNDER },
  { jollyroger, "DANGER!\nSYSTEM FAILURE\nEVACUATE", TFT_RED, ANIM_INVERT, LEDFUNC_FLASHING_RED },
  { page4, "Chapter 3\n\nThat was close.\nWe survived.", TFT_WHITE, ANIM_NONE, LEDFUNC_RAINBOW },
  { page5, "The End\n\nThanks for reading!", TFT_BLACK, ANIM_NONE, LEDFUNC_OFF }
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

// ---------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------

void httpGet(const String& url) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] GET skipped, WiFi not connected");
    return;
  }
  HTTPClient http;
  Serial.print("[HTTP] GET: ");
  Serial.println(url);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.print("[HTTP] Response code: ");
    Serial.println(httpCode);
    String payload = http.getString();
    if (payload.length()) {
      Serial.print("[HTTP] Payload: ");
      Serial.println(payload);
    }
  } else {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

// Volume helpers on remote module
void setVolume(int val) {
  Serial.print("[AUDIO] Setting volume to ");
  Serial.println(val);
  String url = String(AUDIO_BASE_URL) + "/volume?val=" + String(val);
  httpGet(url);
}
void setInitialVolume() { setVolume(DEFAULT_VOLUME); }

// Play helpers on remote module
void playTrackForPage(int pageIndex) {
  int trackNumber = pageIndex + 1;
  Serial.print("[AUDIO] Request play track ");
  Serial.println(trackNumber);
  if (!musicEnabled) {
    Serial.println("[AUDIO] Music disabled - skipping play");
    return;
  }
  String url = String(AUDIO_BASE_URL) + "/play?track=" + String(trackNumber);
  httpGet(url);
}
void sendGlobalPlay() { httpGet(String(AUDIO_BASE_URL) + "/play"); }
void sendStop() { httpGet(String(AUDIO_BASE_URL) + "/stop"); }

// ---------------------------------------------------------
// Remote LED control (calls MP3+LED module)
// ---------------------------------------------------------

// Map local LedFunction -> remote numeric LED_* mode
// (must match enum LedEffect in Mp3LedS2Mini.ino)
void remoteSetLedEffect(LedFunction func) {
  if (!wifiConnected) return;

  int mode = 0; // LED_OFF
  switch (func) {
    case LEDFUNC_OFF:             mode = 0; break; // LED_OFF
    case LEDFUNC_RAINBOW:         mode = 2; break; // LED_RAINBOW
    case LEDFUNC_FLASHING_YELLOW: mode = 3; break; // LED_FLASHING_YELLOW
    case LEDFUNC_FLASHING_RED:    mode = 4; break; // LED_FLASHING_RED
    case LEDFUNC_THUNDER:         mode = 7; break; // LED_THUNDER
    default:                      mode = 0; break;
  }
  String url = String(AUDIO_BASE_URL) + "/led/mode?mode=" + String(mode);
  httpGet(url);
}

// Hex string from 16-bit TFT color
void tftColorToHex(uint16_t c, char* outHex7) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5) & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  snprintf(outHex7, 7, "%02x%02x%02x", r, g, b);
}

// Trigger a page-wipe on remote LED module, direction based on from/to page
void remotePageWipeForPage(int fromPage, int toPage) {
  if (!wifiConnected) return;
  uint16_t c = pages[toPage].color;
  char hex[7];
  tftColorToHex(c, hex);

  String dir = (toPage > fromPage) ? "left" : "right";
  String url = String(AUDIO_BASE_URL) + "/led/pagewipe?color=" + String(hex) + "&dir=" + dir;
  httpGet(url);
}

// ---------------------------------------------------------
// Drawing & animations
// ---------------------------------------------------------

// centered multi-line text with shadow
void drawMultilineText(String text, int x, int y_center, int font, uint16_t color) {
  if (text == "") return;
  int fontHeight = tft.fontHeight(font);
  int lineSpacing = 6;
  int lineCount = 1;
  for (int i = 0; i < text.length(); ++i) if (text.charAt(i) == '\n') lineCount++;
  int totalBlockHeight = (lineCount * fontHeight) + ((lineCount - 1) * lineSpacing);
  int currentY = y_center - (totalBlockHeight / 2);

  int start = 0;
  int end = text.indexOf('\n');
  while (end != -1) {
    String line = text.substring(start, end);
    tft.setTextColor(TFT_BLACK);
    tft.drawCentreString(line, x + 1, currentY + 1, font);
    tft.setTextColor(color);
    tft.drawCentreString(line, x, currentY, font);
    currentY += (fontHeight + lineSpacing);
    start = end + 1;
    end = text.indexOf('\n', start);
  }
  String lastLine = text.substring(start);
  tft.setTextColor(TFT_BLACK);
  tft.drawCentreString(lastLine, x + 1, currentY + 1, font);
  tft.setTextColor(color);
  tft.drawCentreString(lastLine, x, currentY, font);
}

// Draw current page and send LED effect to remote module
void drawCurrentPage() {
  Serial.print("[UI] Drawing page ");
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

  // On first draw: enable the touch-volume window and optionally auto-play
  if (!firstDrawDone) {
    firstDrawDone = true;
    touchVolumeWindowActive = true;
    firstDrawTime = millis();
    Serial.println("[UI] First draw done - touch-volume window active");
    playTrackForPage(0); // track 1
  }
}

// Shake animation (bomb page)
void performShake() {
  if (millis() - lastFrameTime > 40) {
    lastFrameTime = millis();
    shakeX = random(-8, 9);
    shakeY = random(-8, 9);
    tft.setSwapBytes(true);
    tft.pushImage(shakeX, shakeY, 320, 240, pages[currentPage].image);
    tft.setSwapBytes(false);
  }
}

// Invert/flash animation (jollyroger page)
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
// Setup & main loop
// ---------------------------------------------------------

void setup(void) {
  Serial.begin(115200);
  delay(200);

  Serial.print("[WiFi] Connecting to ");
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
    Serial.print("[WiFi] Connected, IP: ");
    Serial.println(WiFi.localIP());
    setInitialVolume();
  } else {
    wifiConnected = false;
    Serial.println("[WiFi] Connection timed out");
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(FIX_INVERTED_COLORS);
  ft6336u.begin();

  drawCurrentPage();
}

void loop() {
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
    int touchX = tp.tp[0].y; // panel mapping
    int touchY = tp.tp[0].x;
    Serial.print("[TOUCH] x=");
    Serial.print(touchX);
    Serial.print(" y=");
    Serial.println(touchY);

    // Early 3s touch-window: volume shortcut
    if (touchVolumeWindowActive && (millis() - firstDrawTime) <= TOUCH_VOLUME_WINDOW_MS) {
      Serial.println("[TOUCH] initial 3s: set volume to 6, ignore navigation");
      setVolume(6);
      touchVolumeWindowActive = false;
      delay(300);
      return;
    } else if (touchVolumeWindowActive && (millis() - firstDrawTime) > TOUCH_VOLUME_WINDOW_MS) {
      touchVolumeWindowActive = false;
      Serial.println("[UI] Touch-volume window expired");
    }

    // Top-left region: toggle music, no navigation
    if (touchX > 225 && touchY >= 0 && touchY < 60) {
      Serial.println("[TOUCH] Top-left region tapped - toggle music");
      musicEnabled = !musicEnabled;
      Serial.print("[AUDIO] Music now: ");
      Serial.println(musicEnabled ? "ON" : "OFF");
      if (musicEnabled) playTrackForPage(currentPage);
      else sendStop();
      delay(300);
      return;
    }

    // Normal left/right navigation based on panel X
    int nextPage = currentPage;
    if (touchX < SCREEN_WIDTH / 2) {
      nextPage--;
      Serial.println("[NAV] Left half touched -> previous");
    } else {
      nextPage++;
      Serial.println("[NAV] Right half touched -> next");
    }

    if (nextPage < 0) nextPage = 0;
    if (nextPage >= NUM_PAGES) nextPage = NUM_PAGES - 1;

    if (nextPage != currentPage) {
      Serial.print("[NAV] Page ");
      Serial.print(currentPage);
      Serial.print(" -> ");
      Serial.println(nextPage);

      int oldPage = currentPage;
      currentPage = nextPage;
      needsUpdate = true;

      // Remote LED: start a page-wipe in direction of navigation
      remotePageWipeForPage(oldPage, currentPage);

      // Remote LED: set effect for the new page (runs after wipe)
      remoteSetLedEffect(pages[currentPage].ledFunc);

      if (musicEnabled) playTrackForPage(currentPage);
      else Serial.println("[AUDIO] Music disabled; not sending play request");

      delay(250);
    } else {
      Serial.println("[NAV] No page change");
      delay(150);
    }
  }
}