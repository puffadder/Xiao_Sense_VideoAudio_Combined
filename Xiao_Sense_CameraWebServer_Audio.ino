#include "esp_camera.h"
#include <WiFi.h>
#include <Preferences.h>

// ===================
// Select camera model
// ===================
#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM and Microphone

#include "audio_server.h"

// ===========================
// WiFi credentials
// ===========================
// Credentials are NOT stored in code. Configure via web UI:
//   - STA mode: http://<device-ip>/wifi
//   - AP mode (first boot / fallback): http://192.168.4.1/wifi
// Persisted in NVS namespace "wifi_config" (keys: ssid, pass, ap_ssid, ap_pass).
String staSsid, staPass;   // Station: your router
String apSsid, apPass;     // Access Point: the device's own hotspot
bool staActive = false;
int lastFailStatus = -1;

framesize_t psram_framesize = FRAMESIZE_QVGA;
framesize_t no_psram_framesize = FRAMESIZE_QVGA;
framesize_t no_jpeg_framesize = FRAMESIZE_QVGA;

void startCameraServer();
void startSTAMode();
void startAPMode();

#if defined(HAS_MICROPHONE)
void mic_i2s_init();
void startAudioServer();
void startVideoAudioServer();
#endif

void setupLedFlash();

// WiFi mode: 0 = STA (router), 1 = AP (XIAO-CAM)
enum WiFiMode { MODE_STA = 0, MODE_AP = 1 };
WiFiMode currentMode;

// Video quality settings (persisted in NVS)
extern const framesize_t vidResList[] = {
  FRAMESIZE_QQVGA,   // 160x120   - index 0
  FRAMESIZE_HQVGA,   // 240x176   - index 1
  FRAMESIZE_QVGA,    // 320x240   - index 2 (default)
  FRAMESIZE_CIF,     // 400x296   - index 3
  FRAMESIZE_VGA,     // 640x480   - index 4
  FRAMESIZE_SVGA,    // 800x600   - index 5 (max)
};
extern const int VID_RES_COUNT = sizeof(vidResList) / sizeof(vidResList[0]);
extern const int VID_RES_DEFAULT = 2; // QVGA
int currentVidResIdx = VID_RES_DEFAULT;

// Audio quality settings (persisted in NVS)
extern const int AUD_GAIN_MIN = 1;
extern const int AUD_GAIN_MAX = 8;
extern const int AUD_GAIN_DEFAULT = 4;
int currentAudGain = AUD_GAIN_DEFAULT;

Preferences prefs;

void setLed(bool on) {
#if defined(LED_GPIO_NUM)
  digitalWrite(LED_GPIO_NUM, on ? LOW : HIGH); // Active low: LOW=ON
#endif
}

void blinkLed(int times, int delayMs = 500) {
#if defined(LED_GPIO_NUM)
  Serial.printf("Blinking LED %d times...\n", times);
  // Detach from PWM if previously attached
  ledcDetach(LED_GPIO_NUM);
  pinMode(LED_GPIO_NUM, OUTPUT);
  for (int i = 0; i < times; i++) {
    setLed(true);
    delay(delayMs);
    setLed(false);
    delay(delayMs);
  }
#endif
}

void initPrefs() {
  prefs.begin("wifi_config", true); // read-only
  currentMode = (WiFiMode)prefs.getUChar("mode", MODE_STA);
  uint8_t savedRes = prefs.getUChar("vid_res", VID_RES_DEFAULT);
  if (savedRes < VID_RES_COUNT) currentVidResIdx = savedRes;
  uint8_t savedGain = prefs.getUChar("aud_gain", AUD_GAIN_DEFAULT);
  if (savedGain >= AUD_GAIN_MIN && savedGain <= AUD_GAIN_MAX) currentAudGain = savedGain;
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  apSsid = prefs.getString("ap_ssid", "XIAO-CAM");
  apPass = prefs.getString("ap_pass", "12345678");
  prefs.end();
  Serial.printf("WiFi mode from NVS: %s\n", currentMode == MODE_STA ? "STA" : "AP");
  Serial.printf("Video res idx %d, audio gain x%d\n", currentVidResIdx, currentAudGain);
}

void startSTAMode() {
  staActive = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  WiFi.setSleep(false);

  Serial.print("Connecting to ");
  Serial.print(staSsid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  if (WiFi.status() != WL_CONNECTED) {
    lastFailStatus = WiFi.status();
    Serial.printf("STA connect failed (20s), status=%d - falling back to AP mode\n", WiFi.status());
    Serial.printf("Attempted SSID: '%s' (len %d), pass len %d\n",
      staSsid.c_str(), staSsid.length(), staPass.length());
    int n = WiFi.scanNetworks();
    bool found = false;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == staSsid) {
        Serial.printf("SSID visible in scan: ch=%d rssi=%d auth=%d\n",
          WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i));
        found = true;
      }
    }
    if (!found) {
      Serial.printf("SSID '%s' NOT visible in scan (2.4GHz only; check name/band)\n", staSsid.c_str());
    }
    WiFi.scanDelete();
    Serial.println("Reconfigure at http://192.168.4.1/wifi");
    startAPMode();
    return;
  }
  Serial.println("WiFi connected (STA)");
  Serial.print("IP: http://");
  Serial.println(WiFi.localIP());
  staActive = true;

  setLed(false);
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  bool apStarted = WiFi.softAP(apSsid.c_str(), apPass.c_str());
  WiFi.setSleep(false);

  delay(500);
  IPAddress apIP = WiFi.softAPIP();
  Serial.println("");
  Serial.print("AP Started: ");
  Serial.println(apStarted ? "YES" : "FAILED");
  Serial.print("SSID: ");
  Serial.println(apSsid);
  Serial.print("IP: http://");
  Serial.println(apIP);
  Serial.print("MAC: ");
  Serial.println(WiFi.softAPmacAddress());

  setLed(false);
}

void setup() {
  Serial.begin(115200);
  // Don't block waiting for Serial connection
  // Serial.setDebugOutput(true); // Can block if USB not connected
  delay(100); // Brief delay for Serial to stabilize if connected

  // Init LED early for status
#if defined(LED_GPIO_NUM)
  pinMode(LED_GPIO_NUM, OUTPUT);
  setLed(false); // OFF initially
#endif

  // Boot button (GPIO 0) for mode toggle
  pinMode(0, INPUT_PULLUP);

  // Read mode from NVS
  initPrefs();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = vidResList[currentVidResIdx];
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if(config.pixel_format == PIXFORMAT_JPEG){
    if(psramFound()){
      config.jpeg_quality = 12;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size = vidResList[currentVidResIdx];
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = no_jpeg_framesize;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  if(config.pixel_format == PIXFORMAT_JPEG){
    s->set_framesize(s, vidResList[currentVidResIdx]);
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // Start WiFi based on mode
  if (currentMode == MODE_STA) {
    if (staSsid.length() > 0) {
      startSTAMode(); // 20s timeout, AP fallback on failure
    } else {
      Serial.println("No WiFi credentials configured - starting AP mode");
      Serial.println("Connect to XIAO-CAM, then open http://192.168.4.1/wifi");
      startAPMode();
    }
  } else {
    startAPMode();  // blinks 10x after AP ready
  }
  setLed(false); // OFF after blink

#if defined(HAS_MICROPHONE)
  mic_i2s_init();
#endif

  startCameraServer();

#if defined(HAS_MICROPHONE)
  startAudioServer();
#endif

  IPAddress ip = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)
    ? WiFi.softAPIP() : WiFi.localIP();
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(ip);
  Serial.println("/combined' for video+audio");
  if (currentMode == MODE_STA && !staActive) {
    Serial.printf("[wifi-diag] SSID='%s' (len %d) passLen=%d status=%d - not connected\n",
      staSsid.c_str(), staSsid.length(), staPass.length(), lastFailStatus);
  }

  // Blink after everything ready - camera, WiFi, mic, servers all started
  blinkLed((currentMode == MODE_STA && staActive) ? 5 : 10);
  setLed(false);
}

void loop() {
#if defined(HAS_MICROPHONE)
  AudioServer.handleClient();
#endif

  // STA mode: check WiFi and reconnect
  if (currentMode == MODE_STA && staActive) {
    static uint32_t last_wifi_check = 0;
    if (millis() - last_wifi_check > 10000) {
      last_wifi_check = millis();
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting...");
        WiFi.reconnect();
      }
    }
  }
  // AP mode: nothing needed

    // Boot button (GPIO 0) to toggle WiFi mode
  static bool lastButton = HIGH;
  static uint32_t pressStart = 0;
  bool button = digitalRead(0); // GPIO 0 = Boot button (active LOW)
  
  if (button == LOW && lastButton == HIGH) {
    // Button just pressed - start timing
    pressStart = millis();
  } else if (button == HIGH && lastButton == LOW) {
    // Button released - check hold duration
    uint32_t holdTime = millis() - pressStart;
    if (holdTime > 200 && holdTime < 2000) { // 200ms-2s = mode toggle
      WiFiMode newMode = (currentMode == MODE_STA) ? MODE_AP : MODE_STA;
      prefs.begin("wifi_config", false);
      prefs.putUChar("mode", newMode);
      prefs.end();
      Serial.printf("Mode toggled to %s, restarting\n", newMode == MODE_STA ? "STA" : "AP");
      blinkLed(15); // 15 blinks = mode changed
      ESP.restart();
    }
  }
  lastButton = button;

  delay(10);
}
