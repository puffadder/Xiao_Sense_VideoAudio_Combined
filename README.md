# XIAO ESP32S3 Sense — Camera + Audio Web Server

> Live MJPEG video + WAV audio streaming from a Seeed Studio XIAO ESP32S3 Sense over WiFi.

![ESP32 Camera](docs/img/video_thumbnail.jpg)

## Overview

This project streams live video (via OV2640 camera) and audio (via onboard ICS-43434 PDM microphone) from a **Seeed Studio XIAO ESP32S3 Sense** board to any web browser on the same network.

Built on the Arduino framework with ESP-IDF HTTP server and a custom responsive web UI for combined video + audio playback.

## Features

- 📸 **Live video streaming** — MJPEG at 640×480 (VGA)
- 🎙️ **Live audio streaming** — WAV PCM at 22050 Hz via PDM microphone
- 📱 **Responsive combined page** — fills portrait viewports, scrolls in landscape
- 🌐 **Three independent endpoints** — video, audio, and combined views

## Hardware

| Component | Detail |
|---|---|
| Board | [Seeed Studio XIAO ESP32S3 Sense](https://www.seeedstudio.com/XIAO-ESP32S3Sense-p-5234.html) |
| MCU | ESP32-S3 with 8 MB Flash + 8 MB OPI PSRAM |
| Camera | OV2640 (stock, QVGA–UXGA) |
| Microphone | ICS-43434 PDM (GPIO 42 = WS, GPIO 41 = SD) |

## Quick Start

### 1. Arduino IDE Setup

1. Install **ESP32 board support** via Arduino Boards Manager (search `esp32`).
2. **Important — use core version `3.0.0`**. Newer cores (3.1+) break the PDM I²S API and remove face-detection headers. [Learn more](#core-version-note).
3. Connect the XIAO Sense via USB.
4. Select the board and port:

   | Setting | Value |
   |---|---|
   | Board | **Seeed XIAO ESP32S3** |
   | PSRAM | **OPI PSRAM** |
   | Flash Mode | **QIO** |
   | Flash Frequency | **80 MHz** |
   | Partition Scheme | **8MB FLASH (3MB APP / 1MB SPIFFS)** |
   | Arduino runs on | **Core 1** (default) |

   ![Board Manager](docs/img/arduino_ide_board_manager_esp32.png)
   ![Board Select](docs/img/board_select.png)
   ![PSRAM](docs/img/enable_psram.png)

### 2. Configure WiFi

Edit `Xiao_Sense_CameraWebServer_Audio.ino` and set your credentials:

```cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
```

### 3. Upload

Click **Upload** in the Arduino IDE. Watch the Serial Monitor (115200 baud) for the IP address once WiFi connects.

### 4. Access the Streams

Open a browser on the same network:

| URL | Description |
|---|---|
| `http://<IP>/combined` | **Combined video + audio** (responsive page) |
| `http://<IP>:81/stream` | MJPEG video stream only |
| `http://<IP>:82/audio` | WAV audio stream only |

## Architecture

The project runs **two HTTP servers** across **three ports**:

| Port | Server | Protocol | Endpoints |
|---:|---|---|---|
| **80** | ESP-IDF `httpd` | HTTP | `/` → redirects to `/combined` |
| **81** | ESP-IDF `httpd` | HTTP + MJPEG | `/stream` — video at 640×480 |
| **82** | Arduino `WebServer` | HTTP + WAV | `/audio` — PCM audio stream |

### Init Order (critical)

```
1. Camera init  (esp_camera_init)
2. WiFi connect (WiFi.begin + wait)
3. Mic init     (ESP_I2S — MUST be after WiFi to avoid APLL conflict)
4. Camera HTTP server  → ports 80 + 81
5. Audio server        → port 82
```

> **APLL conflict**: Initializing the PDM microphone before WiFi connects causes I²S to return zero bytes. The init order above is mandatory.

### PDM Microphone

This project uses the Arduino `ESP_I2S` library (not raw ESP-IDF `i2s_driver_install`) for the PDM microphone. The raw driver does not route PDM GPIOs correctly on ESP32-S3.

```cpp
I2SClass i2sMic;
i2sMic.setPinsPdmRx(42, 41);  // GPIO 42 = WS (clock), 41 = SD (data)
i2sMic.begin(I2S_MODE_PDM_RX, 22050, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
```

## Project Structure

```
├── Xiao_Sense_CameraWebServer_Audio.ino   # Main sketch: init → WiFi → servers
├── app_httpd.cpp                          # ESP-IDF HTTP handlers (camera, stream, combined page)
├── audio_server.h                         # PDM mic init + Arduino WebServer on port 82
├── camera_pins.h                          # XIAO ESP32S3 camera + mic pin definitions
├── camera_index.h                         # Gzipped stock Espressif camera UI (HTML/JS)
├── board_config.h                         # Minimal partition scheme stub
├── partitions.csv                         # 3MB APP / 1MB SPIFFS partition layout
└── docs/img/                              # Documentation images
```

## Known Limitations

- **~4 s audio latency** in Chrome — caused by browser audio buffering on WAV streams. `preload="none"` and no-cache headers reduce but do not eliminate it.
- **`AudioServer.handleClient()` blocks** `loop()` while a client is connected. Acceptable for single-client streaming.
- **Port 81 is hardcoded** in `camera_index.h` JavaScript. Changing it requires regenerating the gzipped HTML.

## Future Enhancements

- **AV sync** — replace the separate WAV stream with a unified approach (e.g. WebRTC or a single WebSocket) to eliminate the ~4 s audio lag and lock audio to video.
- **Resizable video viewer** — make the MJPEG frame a draggable, resizable overlay instead of a fixed viewport fill.
- **Multi-client support** — refactor `AudioServer.handleClient()` to be non-blocking (e.g. async task per connection) so more than one browser can stream simultaneously.

## Core Version Note

⚠️ **This project has been tested with ESP32 Arduino core `3.0.0`.**

Core `3.1+` changes the `ESP_I2S` API (constructor signature, missing PDM constants) and removes face-detection headers, causing compile failures and runtime I²S misbehavior. It may be possible to port the code to newer cores in the future, but `3.0.0` is the only version confirmed working.

**Verify your version:** Arduino IDE → Tools → Boards Manager → search `esp32` → installed version should read `3.0.0`.

## Credits

Based on the Arduino `CameraWebServer` example from Espressif and the [Xiao_Sense_CameraWebServer_Audio](https://github.com/fabio-garavini/Xiao_Sense_CameraWebServer_Audio) project by Fabio Garavini. Cleaned up for minimal video + audio streaming without face detection.

## License

See the original Espressif examples for licensing. This fork follows the same terms.
