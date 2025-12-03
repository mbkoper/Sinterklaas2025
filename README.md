# Sinterklaas 2025 – Mental Model & Hardware Guide

## 0. Mental Model

This project is built around **two cooperating ESP32 boards**:

1. **Device A – TFT Storybook (`TFT_Sinterklaas`)**  
   “I am the storyteller.”
   - Shows illustrated story pages on a TFT display.
   - Uses a capacitive touch panel for navigation and control.
   - Decides which **music track** and **lighting mood** fits each page.
   - Sends simple HTTP requests over Wi‑Fi to Device B.

2. **Device B – MP3 + LED Controller (`Mp3LedS2Mini`)**  
   “I handle sound and light.”
   - Drives a DFPlayer Mini MP3 module (audio).
   - Drives a WS2812 / NeoPixel LED strip (effects and ambience).
   - Hosts a Wi‑Fi access point and a small web server.
   - Exposes a web UI and JSON/HTTP API that both:
     - The TFT Storybook, and
     - A regular web browser  
     can call.

**Flow in one sentence:**  
The TFT Storybook shows a page → decides the atmosphere (track + LED effect) → tells the MP3+LED Controller via HTTP → the MP3+LED Controller plays sound and animates lights accordingly.

---

## 1. Required Arduino Libraries

Install these via the Arduino Library Manager (`Sketch → Include Library → Manage Libraries…`) unless otherwise noted.

### Common / core

- **ESP32 board package**  
  - Install via **Boards Manager**:  
    `Tools → Board → Boards Manager…`  
    Search for and install: **“esp32 by Espressif Systems”**.

### For `Mp3LedS2Mini` (MP3 + LED controller)

- **DFMiniMp3** (by Makuna)  
  For controlling the DFPlayer Mini MP3 module.
- **FastLED**  
  For driving WS2812 / NeoPixel LED strips.
- **WiFi** and **WebServer**  
  Included with the ESP32 core.

### For `TFT_Sinterklaas` (TFT storybook)

- **TFT_eSPI** (by Bodmer)  
  For the TFT display.  
  You must configure `User_Setup.h` (or a custom setup) to match your exact TFT wiring.
- **FT6336U** touch library  
  For the FT6336U capacitive touch controller.  
  (If not in Library Manager, install via its GitHub repo or a vendor package.)
- **WiFi** and **HTTPClient**  
  Included with the ESP32 core.

---

## 2. Wiring – MP3 + LED Controller (`Mp3LedS2Mini`)

Board: ESP32 (e.g. ESP32‑S2 Mini or similar).

### 2.1 DFPlayer Mini

The sketch uses:

```cpp
dfmp3.begin(16, 17); // RX, TX pins for DFPlayer
```

So:

- **ESP32 GPIO 16** → DFPlayer **RX**  
- **ESP32 GPIO 17** → DFPlayer **TX**
- **GND** ↔ DFPlayer GND  
- **VCC** ↔ DFPlayer 5V (most DFPlayers are 5V; check your module)  
- Speaker(s) → DFPlayer **SPK+ / SPK−** (per DFPlayer documentation)

**MicroSD card:**

- Format as FAT32.
- Place your tracks so DFPlayer can see them, e.g.:
  - `/mp3/0001.mp3`, `/mp3/0002.mp3`, …  
  or
  - Root files named `0001.mp3`, `0002.mp3`, etc.  
  (Exact layout depends on your DFPlayer library expectations.)

### 2.2 WS2812 / NeoPixel LED strip

From the code:

```cpp
#define LED_PIN 18
#define NUM_LEDS 30
```

Connect:

- **ESP32 GPIO 18** → LED strip **DIN** (data in)
- **5V** → LED strip **5V**
- **GND** ↔ LED strip GND ↔ ESP32 GND

**Recommended extras:**

- 330–470 Ω resistor between ESP32 GPIO 18 and LED DIN.
- Large capacitor (e.g. 1000 µF, 6.3V or higher) across 5V and GND near the strip.

### 2.3 Power

- Use a stable 5V supply capable of handling:
  - DFPlayer + speaker load.
  - LED strip current (approx. `60 mA * number_of_lit_RGB_LEDs` at full white).
- **All grounds must be common** (ESP32, DFPlayer, LEDs, and 5V supply).

---

## 3. Wiring – TFT Storybook (`TFT_Sinterklaas`)

Board: ESP32‑S3 (or similar) with TFT + FT6336U touch.

### 3.1 I2C (touch + any I2C peripherals)

From the sketch:

```cpp
#define I2C_SDA 16
#define I2C_SCL 15
#define RST_N_PIN 18
#define INT_N_PIN 17
```

For the **FT6336U touch controller**:

- **ESP32 GPIO 16** → FT6336U **SDA**
- **ESP32 GPIO 15** → FT6336U **SCL**
- **ESP32 GPIO 18** → FT6336U **RST** (reset)
- **ESP32 GPIO 17** → FT6336U **INT** (interrupt)
- **3.3V** → FT6336U VCC
- **GND** ↔ FT6336U GND

### 3.2 TFT display

The exact pins depend on your particular TFT module and how you wire it.  
You must set these in **TFT_eSPI**’s `User_Setup.h` (or a custom setup):

Typical signals:

- `TFT_MOSI`  → TFT **MOSI / SDA**
- `TFT_MISO`  → TFT **MISO** (if used)
- `TFT_SCLK`  → TFT **SCK / SCL**
- `TFT_CS`    → TFT **CS**
- `TFT_DC`    → TFT **DC / RS**
- `TFT_RST`   → TFT **RST** (or tied to board reset if supported)
- `TFT_BL`    → TFT **Backlight** (may be driven via a GPIO or tied to 3.3V)

Make sure:

- The pin definitions in TFT_eSPI match *your* wiring.
- Voltages are 3.3V (or properly level‑shifted).

### 3.3 Power

- Power the ESP32 and TFT as specified by your dev board (often 5V input to USB or VIN).
- Ensure a **common ground** with the touch controller and any other peripherals.

---

## 4. Network / Runtime Behavior

### 4.1 MP3+LED Controller (Device B)

The `Mp3LedS2Mini` sketch:

- Starts a Wi‑Fi access point:

  ```cpp
  const char* ssid     = "ESP32_MP3_AP";
  const char* password = "12345678";
  ```

- Typical AP IP: `192.168.4.1`.
- Hosts a web server at `http://192.168.4.1/` with:

  - A browser UI at `/` to:
    - Start/stop tracks.
    - Set volume and LED colors/modes.
  - JSON/HTTP endpoints:
    - `/play?track=N`
    - `/stop`
    - `/volume?val=V`
    - `/led/on`, `/led/off`, `/led/rainbow`
    - `/led/setcolor?r=R&g=G&b=B`
    - `/led/intensity?val=B`
    - `/led/mode?mode=M`
    - `/led/pagewipe?color=rrggbb&dir=left|right`
    - `/status` (returns volume, brightness, current track)

### 4.2 TFT Storybook (Device A)

The `TFT_Sinterklaas` sketch:

- Connects as a **Wi‑Fi station** to `ESP32_MP3_AP` using the same SSID/password.
- Uses `AUDIO_BASE_URL = "http://192.168.4.1"`.

Behavior:

- On Wi‑Fi connect:
  - Calls `/volume?val=15` on Device B (default volume).
- On first page draw:
  - Starts playing track 1 (if `musicEnabled == true`).
- On page change:
  - Calls `/play?track=<pageIndex+1>`.
  - Calls `/led/mode?mode=<mapped LedFunction>`.
  - Optionally calls `/led/pagewipe?color=ffffff&dir=left|right` based on navigation direction.
- Top‑left touch region:
  - Toggles music ON/OFF (and sends `/stop` when turning OFF).
- Within the first 3 seconds after the first draw:
  - A touch changes volume to a preset (e.g. 6) without changing page.

---

## 5. Uploading and Running

1. **Clone or download** this repository.
2. Open the project in **Arduino IDE** (or PlatformIO).

### 5.1 Install libraries and board packages

- Install **“esp32 by Espressif Systems”** via Boards Manager.
- Install required libraries:
  - `DFMiniMp3`
  - `FastLED`
  - `TFT_eSPI`
  - `FT6336U` (from Library Manager or GitHub)
- Configure **TFT_eSPI**:
  - Edit `User_Setup.h` (or a custom setup file) to match your TFT pinout and driver.

### 5.2 Flash both sketches

- Select the correct board + port for each device.
- Flash:
  - `Mp3LedS2Mini/Mp3LedS2Mini.ino` to the MP3+LED ESP32.
  - `TFT_Sinterklaas/TFT_Sinterklaas.ino` to the TFT ESP32.

### 5.3 Run

1. Power the **MP3+LED** controller first:
   - It will start `ESP32_MP3_AP` and the web server.
2. Power the **TFT Storybook**:
   - It will connect to the AP.
   - Initial page + volume will be set.
   - Touch to navigate and control music.
3. Optionally, connect a phone or laptop to `ESP32_MP3_AP` and visit:
   - `http://192.168.4.1/` for manual control and debugging.

---

## 6. Customization Tips

- Change `NUM_LEDS` and `LED_PIN` to match your LED hardware.
- Adjust track numbering or page/track mapping in `TFT_Sinterklaas.ino`.
- Tweak page texts and images in `pictures.h` and the `pages[]` table.
- Adapt TFT_eSPI setup for different TFT modules or resolutions.

If you use different hardware (other ESP32 variant, TFT, or touch controller), adjust pin assignments and library setups accordingly.