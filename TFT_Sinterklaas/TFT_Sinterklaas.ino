#include <TFT_eSPI.h>
#include <FT6336U.h>
#include <Wire.h>
#include "pictures.h"
#include <WiFi.h>
#include <HTTPClient.h>

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define I2C_SDA 16
#define I2C_SCL 15
#define RST_N_PIN 18
#define INT_N_PIN 17

#define FIX_INVERTED_COLORS true

const char* WIFI_SSID     = "ESP32_MP3_AP";
const char* WIFI_PASSWORD = "12345678";
const char* AUDIO_BASE_URL = "http://192.168.4.1";

// animation types
enum AnimationType { ANIM_NONE, ANIM_SHAKE, ANIM_INVERT };

// global volume mode: affects all pages
enum VolumeMode { VOL_FULL = 0, VOL_HALF = 1, VOL_NONE = 2 };

// -------------------- LED endpoint mapping (matches Mp3LedS2Mini.ino) --------------------

struct LedActions {
  const char* led_color_orange;
  const char* led_off;
  const char* led_wipe_right;
  const char* led_wipe_left;
  const char* led_rainbow;
  const char* led_flash_red;
  const char* led_flash_white;
};

const LedActions LED_FUNCS = {
  "/led/setcolor?r=255&g=128&b=0", // led_color_orange
  "/led/off",                      // led_off
  "/led/wipe_right",               // led_wipe_right
  "/led/wipe_left",                // led_wipe_left
  "/led/rainbow",                  // led_rainbow
  "/led/flash?color=red",          // led_flash_red
  "/led/flash?color=white"         // led_flash_white
};

struct PageData {
  const uint16_t* image;
  const char* text;
  uint16_t color;
  AnimationType animation;
  int track;
  int volume;
  int maxPlaySeconds;
  const char* ledPathOnEnter;
};

#define NUM_PAGES 9
#define TEXT_FONT 2

const PageData pages[NUM_PAGES] = {
  {page0,"",TFT_BLACK,ANIM_NONE,1,12,-1, LED_FUNCS.led_off},
  {page1,"Een ie-rieder vervangt stapels papier,\nGewoon een handig dingetje, dat geeft plezier.\nGeen kreukels, geen ezelsoren meer,\nMaar strak digitaal, keer op keer.",TFT_BLACK,ANIM_NONE,2,14,-1, LED_FUNCS.led_rainbow},
  {page2,"Je kiest een roman of een spannend verhaal,\nEn leest het meteen, digitaal en ideaal,\nJe bladert nu licht, zo snel en fijn,\nMet honderden titels in een klein design.",TFT_BLACK,ANIM_NONE,6,10,-1, LED_FUNCS.led_rainbow},
  {page3,"De batterij houdt dagenlang stand,\nDus lezen kan overal in het land,\nDe pieten zagen je boeken verslinden,\nMaar soms was het lastig om ze te vinden",TFT_BLACK,ANIM_NONE,5,14,-1, LED_FUNCS.led_rainbow},
  {bomb,"",TFT_RED,ANIM_SHAKE,9,26,3, LED_FUNCS.led_flash_red},
  {jollyroger,"EEN PIRAAT!\nDIT KAN TOCH\nNIET WAAR ZIJN",TFT_RED,ANIM_INVERT,8,20,12, LED_FUNCS.led_flash_white},
  {page4,"Piraat zijn! Sint fronste zijn wenkbrauw,\nCorina, dit gedrag past echt niet bij jou,\nWant boeken “stelen”, goede vrind,\nVerdien je geen applaus van de Sint.",TFT_BLACK,ANIM_NONE,3,24,-1, LED_FUNCS.led_rainbow},
  {page5,"Zoek het cadeau dat je beter kan maken,\nZodat je piraat activiteiten kunt staken,\nMet dit geschenk lees je eerlijk en fijn,\nEn zal de Sint weer vast tevreden zijn.",TFT_BLACK,ANIM_NONE,4,14,-1, LED_FUNCS.led_color_orange},
  {page1,"En nu vlot naar de cadeaus op zoek,\nKijk ook in dit mooie boek.\nWe zijn nu echt wel klaar,\ngraag weer tot volgend jaar!\n\nTHE END",TFT_BLACK,ANIM_NONE,2,14,20, LED_FUNCS.led_off}
};

TFT_eSPI tft = TFT_eSPI();
FT6336U ft6336u(I2C_SDA, I2C_SCL, RST_N_PIN, INT_N_PIN);
FT6336U_TouchPointType tp;

// animation state
unsigned long lastFrameTime = 0;
int shakeX = 0;
int shakeY = 0;
bool isFlashed = false;

// page, audio and wifi state
int currentPage = 0;
bool needsUpdate = true;
bool firstDrawDone = false;
bool musicEnabled = true;
bool wifiConnected = false;

// audio timing and volume tracking
int currentAudioPage = -1;
unsigned long currentAudioStartMillis = 0;
bool audioPlaying = false;
int currentVolume = -1;

// global volume mode
VolumeMode currentVolumeMode = VOL_FULL;

// -------- HTTP helpers --------

void httpGetSync(const String& url) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] GET skipped, WiFi not connected");
    return;
  }
  HTTPClient http;
  Serial.print("[HTTP] GET (sync): ");
  Serial.println(url);
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.print("[HTTP] Response code: ");
    Serial.println(httpCode);
  } else {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

void httpGetFireAndForget(const String& url) {
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] GET skipped, WiFi not connected");
    return;
  }

  String u = url;
  if (u.startsWith("http://")) u.remove(0, 7);

  int slashIndex = u.indexOf('/');
  String hostPort = (slashIndex >= 0) ? u.substring(0, slashIndex) : u;
  String path     = (slashIndex >= 0) ? u.substring(slashIndex)    : "/";

  String host = hostPort;
  int port = 80;
  int colonIndex = hostPort.indexOf(':');
  if (colonIndex >= 0) {
    host = hostPort.substring(0, colonIndex);
    port = hostPort.substring(colonIndex + 1).toInt();
    if (port <= 0) port = 80;
  }

  WiFiClient client;
  if (!client.connect(host.c_str(), port)) {
    Serial.println("[HTTP] connect failed");
    return;
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n");
  client.print(String("Host: ") + host + "\r\n");
  client.print("Connection: close\r\n\r\n");
  client.stop();
  Serial.print("[HTTP] fire-and-forget GET: ");
  Serial.println(path);
}

void sendLedForPage(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= NUM_PAGES) return;

  const char* path = pages[pageIndex].ledPathOnEnter;
  if (!path || path[0] == '\0') {
    Serial.println("[LED] No LED action for this page");
    return;
  }

  String url = String(AUDIO_BASE_URL) + String(path);
  httpGetFireAndForget(url);
}

void remotePageWipeForPage(int oldPage, int newPage) {
  if (oldPage == newPage) return;

  if (oldPage == 4 || oldPage == 5 || newPage == 4 || newPage == 5) {
    Serial.println("[LED] Page wipe skipped for bomb/jollyroger pages");
    return;
  }

  String url;
  if (newPage > oldPage) {
    url = String(AUDIO_BASE_URL) + LED_FUNCS.led_wipe_right;
    Serial.println("[LED] Page wipe RIGHT");
  } else {
    url = String(AUDIO_BASE_URL) + LED_FUNCS.led_wipe_left;
    Serial.println("[LED] Page wipe LEFT");
  }
  httpGetFireAndForget(url);
}

// ------------- Volume helpers -------------

int computeVolumeForPage(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= NUM_PAGES) return 0;
  int base = pages[pageIndex].volume;
  if (base < 0) base = 0;

  switch (currentVolumeMode) {
    case VOL_FULL:
      return base;
    case VOL_HALF:
      if (base <= 1) return base > 0 ? 1 : 0;
      return base / 2;
    case VOL_NONE:
    default:
      return 0;
  }
}

void setVolume(int vol) {
  if (vol < 0) return;
  if (currentVolume == vol) {
    Serial.print("[AUDIO] Volume unchanged (");
    Serial.print(vol);
    Serial.println(")");
    return;
  }
  Serial.print("[AUDIO] Setting volume to ");
  Serial.println(vol);
  String url = String(AUDIO_BASE_URL) + "/volume?val=" + String(vol);
  httpGetFireAndForget(url);
  currentVolume = vol;
}

void sendStop() {
  Serial.println("[AUDIO] Stop playback");
  String url = String(AUDIO_BASE_URL) + "/stop";
  httpGetFireAndForget(url);
  audioPlaying = false;
  currentAudioPage = -1;
  currentAudioStartMillis = 0;
}

void playTrackForPage(int pageIndex) {
  if (pageIndex < 0 || pageIndex >= NUM_PAGES) {
    Serial.println("[AUDIO] Invalid page index, skipping play");
    return;
  }
  int trackNumber = pages[pageIndex].track;
  if (trackNumber <= 0) {
    Serial.print("[AUDIO] Page ");
    Serial.print(pageIndex);
    Serial.println(" has no track assigned, skipping play");
    return;
  }
  if (!musicEnabled) {
    Serial.println("[AUDIO] Music is disabled, play skipped");
    return;
  }

  int vol = computeVolumeForPage(pageIndex);
  setVolume(vol);

  Serial.print("[AUDIO] Request play for page ");
  Serial.print(pageIndex);
  Serial.print(" (track ");
  Serial.print(trackNumber);
  Serial.print(", vol ");
  Serial.print(vol);
  Serial.println(")");
  String url = String(AUDIO_BASE_URL) + "/play?track=" + String(trackNumber);
  httpGetFireAndForget(url);
  currentAudioPage = pageIndex;
  currentAudioStartMillis = millis();
  audioPlaying = true;
}

void updateAudioTimeout() {
  if (!audioPlaying) return;
  if (currentAudioPage < 0 || currentAudioPage >= NUM_PAGES) return;
  int limit = pages[currentAudioPage].maxPlaySeconds;
  if (limit < 0) return;
  unsigned long elapsedMs = millis() - currentAudioStartMillis;
  if (elapsedMs >= (unsigned long)limit * 1000UL) {
    Serial.print("[AUDIO] Auto stop after ");
    Serial.print(limit);
    Serial.println(" seconds");
    sendStop();
  }
}

// ------------- Drawing & animation -------------

void drawMultilineText(String text, int x, int y_center, int font, uint16_t color) {
  if (text == "") return;
  int fontHeight = tft.fontHeight(font);
  int lineSpacing = 6;
  int lineCount = 1;
  for (int i = 0; i < text.length(); i++) {
    if (text.charAt(i) == '\n') lineCount++;
  }
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

void drawCurrentPage() {
  Serial.print("[UI] Drawing page ");
  Serial.println(currentPage);
  isFlashed = false;
  tft.invertDisplay(FIX_INVERTED_COLORS);
  tft.setSwapBytes(true);
  tft.pushImage(0, 0, 320, 240, pages[currentPage].image);
  tft.setSwapBytes(false);
  if (pages[currentPage].animation != ANIM_SHAKE) {
    drawMultilineText(
      pages[currentPage].text,
      tft.width() / 2,
      tft.height() / 2,
      TEXT_FONT,
      pages[currentPage].color
    );
  }

  if (!firstDrawDone) {
    firstDrawDone = true;
    Serial.println("[UI] First draw done");

    // SOUND FIRST, THEN LED, ON INITIAL PAGE
    if (musicEnabled) {
      playTrackForPage(currentPage);
    }
    sendLedForPage(currentPage);
  }
}

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

// ------------- Setup & main loop -------------

void setup(void) {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("[BOOT] Booting...");

  Serial.print("[WiFi] Connecting to SSID: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long startAttemptTime = millis();
  const unsigned long wifiTimeout = 10000;
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("[WiFi] Connected!");
    Serial.print("[WiFi] IP address: ");
    Serial.println(WiFi.localIP());
    int initialVol = computeVolumeForPage(0);
    setVolume(initialVol);
  } else {
    wifiConnected = false;
    Serial.println("[WiFi] Connection timed out.");
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

  switch (pages[currentPage].animation) {
    case ANIM_SHAKE:  performShake();  break;
    case ANIM_INVERT: performInvert(); break;
    case ANIM_NONE:
    default:          break;
  }

  updateAudioTimeout();

  tp = ft6336u.scan();
  if (tp.touch_count > 0) {
    int touchX = tp.tp[0].y;
    int touchY = tp.tp[0].x;
    Serial.print("[TOUCH] x=");
    Serial.print(touchX);
    Serial.print(" y=");
    Serial.println(touchY);

    // top-left corner: global volume mode cycle FULL -> HALF -> NONE -> FULL ...
    if (touchX < 25 && touchY >= 210) {
      Serial.println("[TOUCH] Top-left corner tapped -> cycle global volume mode");

      if (currentVolumeMode == VOL_FULL) {
        currentVolumeMode = VOL_HALF;
        Serial.println("[AUDIO] Volume mode: HALF");
      } else if (currentVolumeMode == VOL_HALF) {
        currentVolumeMode = VOL_NONE;
        Serial.println("[AUDIO] Volume mode: NONE (mute)");
      } else {
        currentVolumeMode = VOL_FULL;
        Serial.println("[AUDIO] Volume mode: FULL");
      }

      int newVol = computeVolumeForPage(currentPage);
      setVolume(newVol);

      delay(300);
      return;
    }

    int nextPage = currentPage;
    if (touchX < SCREEN_WIDTH / 2) {
      nextPage--;
      Serial.println("[NAV] Touch on left half -> previous page");
    } else {
      nextPage++;
      Serial.println("[NAV] Touch on right half -> next page");
    }

    if (nextPage < 0) nextPage = 0;
    if (nextPage >= NUM_PAGES) nextPage = NUM_PAGES - 1;

    if (nextPage != currentPage) {
      Serial.print("[NAV] Changing page ");
      Serial.print(currentPage);
      Serial.print(" -> ");
      Serial.println(nextPage);
      int oldPage = currentPage;
      currentPage = nextPage;

      // Draw new page immediately
      drawCurrentPage();

      // ---- SOUND FIRST, THEN LED, ON PAGE CHANGE ----
      if (audioPlaying) {
        sendStop();
      }

      if (musicEnabled) {
        playTrackForPage(currentPage);
      } else {
        Serial.println("[AUDIO] Music disabled, not sending play for new page");
      }

      remotePageWipeForPage(oldPage, currentPage);
      sendLedForPage(currentPage);

      delay(250);
    } else {
      Serial.println("[NAV] Touch did not change page");
      delay(150);
    }
  }
}