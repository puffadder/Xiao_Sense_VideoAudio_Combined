#include "esp_camera.h"
#include <WiFi.h>
#include <Preferences.h>

// ===================
// Select camera model
// ===================
#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM and Microphone

#include "audio_server.h"

// ===========================
// Enter your WiFi credentials
// ===========================
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

framesize_t psram_framesize = FRAMESIZE_QVGA;
framesize_t no_psram_framesize = FRAMESIZE_QVGA;
framesize_t no_jpeg_framesize = FRAMESIZE_QVGA;

void startCameraServer();

#if defined(HAS_MICROPHONE)
void mic_i2s_init();
void startAudioServer();
void startVideoAudioServer();
#endif

void setupLedFlash();

// WiFi mode: 0 = STA (router), 1 = AP (XIAO-CAM)
enum WiFiMode { MODE_STA = 0, MODE_AP = 1 };
WiFiMode currentMode;

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
  prefs.end();
  Serial.printf("WiFi mode from NVS: %s\n", currentMode == MODE_STA ? "STA" : "AP");
}

void startSTAMode() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  
  Serial.print("Connecting to ");
  Serial.print(ssid);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected (STA)");
  Serial.print("IP: http://");
  Serial.println(WiFi.localIP());
  
  // Blink after IP is known - ready for client connection
  blinkLed(5); // 5 blinks = STA ready
  setLed(false);
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  const char *ap_ssid = "XIAO-CAM";
  const char *ap_password = "12345678"; // min 8 chars
  bool apStarted = WiFi.softAP(ap_ssid, ap_password);
  WiFi.setSleep(false);

  delay(500);
  IPAddress apIP = WiFi.softAPIP();
  Serial.println("");
  Serial.print("AP Started: ");
  Serial.println(apStarted ? "YES" : "FAILED");
  Serial.print("SSID: ");
  Serial.println(ap_ssid);
  Serial.print("IP: http://");
  Serial.println(apIP);
  Serial.print("MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  
  // Blink after AP is ready - ready for client connection
  blinkLed(10); // 10 blinks = AP ready
  setLed(false);
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

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
  config.frame_size = psram_framesize;
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
      config.frame_size = no_psram_framesize;
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
    s->set_framesize(s, config.frame_size);
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // Start WiFi based on mode
  if (currentMode == MODE_STA) {
    startSTAMode(); // blinks 5x after IP
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

  IPAddress ip = (currentMode == MODE_STA) ? WiFi.localIP() : WiFi.softAPIP();
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(ip);
  Serial.println("/combined' for video+audio");
}

void loop() {
#if defined(HAS_MICROPHONE)
  AudioServer.handleClient();
#endif

  // STA mode: check WiFi and reconnect
  if (currentMode == MODE_STA) {
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
