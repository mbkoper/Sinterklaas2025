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
enum AnimationType {
  ANIM_NONE,
  ANIM_SHAKE,
  ANIM_INVERT
};

// per-page configuration: picture, text, color, animation, audio track, volume, and optional max play time
struct PageData {
  const uint16_t* image;
  const char* text;
  uint16_t color;
  AnimationType animation;
  int track;           // audio track number; <=0 means no track
  int volume;          // volume for this page (always set, default 6)
  int maxPlaySeconds;  // -1 = endless, >0 = auto stop after given seconds
};

#define NUM_PAGES 8
#define TEXT_FONT 2

const PageData pages[NUM_PAGES] = {
  {page0,"",TFT_BLACK,ANIM_NONE,1,6,-1},
  {page1,"Een ie-rieder vervangt stapels papier,\nGewoon een handig dingetje, dat geeft plezier.\nGeen kreukels, geen ezelsoren meer,\nMaar strak digitaal, keer op keer.",TFT_BLACK,ANIM_NONE,2,6,-1},
  {page2,"Je kiest een roman of een spannend verhaal,\nEn leest het meteen, digitaal en ideaal,\nJe bladert nu licht, zo snel en fijn,\nMet honderden titels in een klein design.",TFT_BLACK,ANIM_NONE,6,2,-1},
  {page3,"De batterij houdt dagenlang stand,\nDus lezen kan overal in het land,\nDe pieten zagen je boeken verslinden,\nMaar soms was het lastig om ze te vinden",TFT_BLACK,ANIM_NONE,5,6,-1},
  {bomb,"",TFT_RED,ANIM_SHAKE,9,6,3},
  {jollyroger,"EEN PIRAAT!\nDIT KAN TOCH\nNIET WAAR ZIJN",TFT_RED,ANIM_INVERT,8,6,12},
  {page4,"Piraat zijn! Sint fronste zijn wenkbrauw,\nCorina, dit gedrag past echt niet bij jou,\nWant boeken “stelen”, goede vrind,\nVerdien je geen applaus van de Sint.",TFT_BLACK,ANIM_NONE,3,6,-1},
  {page5,"Zoek het cadeau dat je beter kan maken,\nZodat je piraat activiteiten kunt staken,\nMet dit geschenk lees je eerlijk en fijn,\nEn zal de Sint weer vast tevreden zijn.",TFT_BLACK,ANIM_NONE,4,6,-1}
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
int currentVolume = -1;  // last volume sent to the server; -1 = unknown/not set

// draw multi-line centered text with a small shadow for readability
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

// basic HTTP GET wrapper that respects wifiConnected
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
    if (payload.length() > 0) {
      Serial.print("[HTTP] Payload: ");
      Serial.println(payload);
    }
  } else {
    Serial.print("[HTTP] Request failed: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

// send a volume command to the audio server and remember it, only if changed
void setVolume(int vol) {
  if (vol <= 0) return;

  if (currentVolume == vol) {
    Serial.print("[AUDIO] Volume unchanged (");
    Serial.print(vol);
    Serial.println(")");
    return;
  }

  Serial.print("[AUDIO] Setting volume to ");
  Serial.println(vol);

  String url = String(AUDIO_BASE_URL) + "/volume?val=" + String(vol);
  httpGet(url);
  currentVolume = vol;
}

// set initial default volume (6) after WiFi is up
void setInitialVolume() {
  if (currentVolume > 0) {
    Serial.println("[AUDIO] Initial volume already set, skipping");
    return;
  }
  setVolume(6);
}

// stop playback on the audio server and clear audio state
void sendStop() {
  Serial.println("[AUDIO] Stop playback");
  String url = String(AUDIO_BASE_URL) + "/stop";
  httpGet(url);
  audioPlaying = false;
  currentAudioPage = -1;
  currentAudioStartMillis = 0;
}

// start playing track for a given page, updating volume if needed
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

  // always have a defined volume per page, only send if different
  setVolume(pages[pageIndex].volume);

  Serial.print("[AUDIO] Request play for page ");
  Serial.print(pageIndex);
  Serial.print(" (track ");
  Serial.print(trackNumber);
  Serial.println(")");

  String url = String(AUDIO_BASE_URL) + "/play?track=" + String(trackNumber);
  httpGet(url);

  currentAudioPage = pageIndex;
  currentAudioStartMillis = millis();
  audioPlaying = true;
}

// check whether the current page's audio should stop based on maxPlaySeconds
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

// draw the active page (image + text)
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
    Serial.println("[UI] First draw done, auto-playing track for page 0");
    playTrackForPage(0);
  }
}

// shake animation for bomb page
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

// invert animation for jollyroger page
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

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < wifiTimeout) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("[WiFi] Connected!");
    Serial.print("[WiFi] IP address: ");
    Serial.println(WiFi.localIP());
    setInitialVolume();
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
    case ANIM_SHAKE:
      performShake();
      break;
    case ANIM_INVERT:
      performInvert();
      break;
    case ANIM_NONE:
    default:
      break;
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

    // top-left corner toggles global music on/off
    if (touchX < 25 && touchY >= 210) {
      Serial.println("[TOUCH] Top-left corner tapped -> toggle music");

      musicEnabled = !musicEnabled;
      Serial.print("[AUDIO] MusicEnabled now: ");
      Serial.println(musicEnabled ? "ON" : "OFF");

      if (musicEnabled) {
        playTrackForPage(currentPage);
      } else {
        sendStop();
      }

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

      currentPage = nextPage;
      needsUpdate = true;

      // stop previous page's audio when navigating
      if (audioPlaying) {
        sendStop();
      }

      if (musicEnabled) {
        playTrackForPage(currentPage);
      } else {
        Serial.println("[AUDIO] Music disabled, not sending play for new page");
      }

      delay(250);
    } else {
      Serial.println("[NAV] Touch did not change page");
      delay(150);
    }
  }
}