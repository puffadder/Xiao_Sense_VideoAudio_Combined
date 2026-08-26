#include "esp_camera.h"
#include <WiFi.h>

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

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

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
  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if(config.pixel_format == PIXFORMAT_JPEG){
    if(psramFound()){
      config.jpeg_quality = 12;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = no_psram_framesize;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Non-JPEG streaming
    config.frame_size = no_jpeg_framesize;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1); // flip it back
    s->set_brightness(s, 1); // up the brightness just a bit
    s->set_saturation(s, -2); // lower the saturation
  }

  // Use configured framesize (psram_framesize / no_psram_framesize)
  if(config.pixel_format == PIXFORMAT_JPEG){
    s->set_framesize(s, config.frame_size);
  }

// Setup LED Flash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

#if defined(HAS_MICROPHONE)
  // Init mic AFTER WiFi connects to avoid APLL conflict
  mic_i2s_init();
#endif

  // Start servers: camera on 80, stream on 81, audio on 82, combined page on 83
  startCameraServer();

#if defined(HAS_MICROPHONE)
  startAudioServer();
#endif

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("/combined' for video+audio");
}


void loop() {
#if defined(HAS_MICROPHONE)
  AudioServer.handleClient();
#endif
  delay(10);
}
