// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <WiFi.h>
#include <Preferences.h>

#ifndef CAMERA_MODEL_XIAO_ESP32S3
#define CAMERA_MODEL_XIAO_ESP32S3
#endif

#include "camera_pins.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp32-hal-ledc.h"
#include "sdkconfig.h"
#include "camera_index.h"
#include "board_config.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// LED FLASH setup
#if defined(LED_GPIO_NUM)
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 0;
bool isStreaming = false;

#endif

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

// Extern quality config from main sketch
extern int currentVidResIdx;
extern int currentAudGain;
extern const framesize_t vidResList[];
extern const int VID_RES_COUNT;
extern const int AUD_GAIN_MIN;
extern const int AUD_GAIN_MAX;
extern String staSsid;
extern String staPass;
extern String apSsid;
extern String apPass;

typedef struct {
  size_t size;   //number of values used for filtering
  size_t index;  //current value index
  size_t count;  //value count
  int sum;
  int *values;  //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
  memset(filter, 0, sizeof(ra_filter_t));

  filter->values = (int *)malloc(sample_size * sizeof(int));
  if (!filter->values) {
    return NULL;
  }
  memset(filter->values, 0, sample_size * sizeof(int));

  filter->size = sample_size;
  return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t *filter, int value) {
  if (!filter->values) {
    return value;
  }
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index++;
  filter->index = filter->index % filter->size;
  if (filter->count < filter->size) {
    filter->count++;
  }
  return filter->sum / filter->count;
}
#endif

#if defined(LED_GPIO_NUM)
void enable_led(bool en) {  // Turn LED On or Off
  int duty = en ? led_duty : 0;
  // Disable flash LED to avoid conflict with mode indicator LED
  // ledcWrite(LED_GPIO_NUM, duty);
  // log_i("Set LED intensity to %d", duty);
}
#endif

static esp_err_t bmp_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_start = esp_timer_get_time();
#endif
  fb = esp_camera_fb_get();
  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/x-windows-bmp");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.bmp");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

  uint8_t *buf = NULL;
  size_t buf_len = 0;
  bool converted = frame2bmp(fb, &buf, &buf_len);
  esp_camera_fb_return(fb);
  if (!converted) {
    log_e("BMP Conversion failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  res = httpd_resp_send(req, (const char *)buf, buf_len);
  free(buf);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  uint64_t fr_end = esp_timer_get_time();
#endif
  log_i("BMP: %llums, %uB", (uint64_t)((fr_end - fr_start) / 1000), buf_len);
  return res;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_start = esp_timer_get_time();
#endif

#if defined(LED_GPIO_NUM)
  enable_led(true);
  vTaskDelay(150 / portTICK_PERIOD_MS);
  fb = esp_camera_fb_get();
  enable_led(false);
#else
  fb = esp_camera_fb_get();
#endif

  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  size_t fb_len = 0;
#endif
  if (fb->format == PIXFORMAT_JPEG) {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = fb->len;
#endif
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    jpg_chunking_t jchunk = {req, 0};
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    fb_len = jchunk.len;
#endif
  }
  esp_camera_fb_return(fb);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
  int64_t fr_end = esp_timer_get_time();
#endif
  log_i("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  struct timeval _timestamp;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[128];

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

#if defined(LED_GPIO_NUM)
  isStreaming = true;
  enable_led(true);
#endif

  int noFb = 0;

  while (true) {
    // Check connectivity based on WiFi mode
    wifi_mode_t mode = WiFi.getMode();
    if ((mode == WIFI_STA || mode == WIFI_AP_STA) && WiFi.status() != WL_CONNECTED) {
      Serial.println("STA WiFi disconnected, stopping stream");
      break;
    }
    if ((mode == WIFI_AP || mode == WIFI_AP_STA) && WiFi.softAPgetStationNum() == 0) {
      // Optional: stop if no clients connected to AP
    }

    fb = esp_camera_fb_get();
    if (!fb) {
      log_e("Camera capture failed");
      // Frame-size switches can transiently starve the driver; hold the
      // connection open instead of tearing down the whole MJPEG stream.
      if (++noFb > 100) {
        Serial.println("Stream ending: camera not producing frames");
        break;
      }
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    noFb = 0;
    _timestamp.tv_sec = fb->timestamp.tv_sec;
    _timestamp.tv_usec = fb->timestamp.tv_usec;
    if (fb->format != PIXFORMAT_JPEG) {
      bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!jpeg_converted) {
        log_e("JPEG compression failed");
        res = ESP_FAIL;
      }
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      log_e("Send frame failed");
      break;
    }
    int64_t fr_end = esp_timer_get_time();

    int64_t frame_time = fr_end - last_frame;
    last_frame = fr_end;

    frame_time /= 1000;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
#endif
    log_i(
      "MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)",
      (uint32_t)(_jpg_buf_len), (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
      avg_frame_time, 1000.0 / avg_frame_time
    );
  }

#if defined(LED_GPIO_NUM)
  isStreaming = false;
  enable_led(false);
#endif

  return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
  char *buf = NULL;
  size_t buf_len = 0;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      *obuf = buf;
      return ESP_OK;
    }
    free(buf);
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf = NULL;
  char variable[32];
  char value[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
      httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int val = atoi(value);
  log_i("%s = %d", variable, val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(variable, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) {
      res = s->set_framesize(s, (framesize_t)val);
    }
  } else if (!strcmp(variable, "quality")) {
    res = s->set_quality(s, val);
  } else if (!strcmp(variable, "contrast")) {
    res = s->set_contrast(s, val);
  } else if (!strcmp(variable, "brightness")) {
    res = s->set_brightness(s, val);
  } else if (!strcmp(variable, "saturation")) {
    res = s->set_saturation(s, val);
  } else if (!strcmp(variable, "gainceiling")) {
    res = s->set_gainceiling(s, (gainceiling_t)val);
  } else if (!strcmp(variable, "colorbar")) {
    res = s->set_colorbar(s, val);
  } else if (!strcmp(variable, "awb")) {
    res = s->set_whitebal(s, val);
  } else if (!strcmp(variable, "agc")) {
    res = s->set_gain_ctrl(s, val);
  } else if (!strcmp(variable, "aec")) {
    res = s->set_exposure_ctrl(s, val);
  } else if (!strcmp(variable, "hmirror")) {
    res = s->set_hmirror(s, val);
  } else if (!strcmp(variable, "vflip")) {
    res = s->set_vflip(s, val);
  } else if (!strcmp(variable, "awb_gain")) {
    res = s->set_awb_gain(s, val);
  } else if (!strcmp(variable, "agc_gain")) {
    res = s->set_agc_gain(s, val);
  } else if (!strcmp(variable, "aec_value")) {
    res = s->set_aec_value(s, val);
  } else if (!strcmp(variable, "aec2")) {
    res = s->set_aec2(s, val);
  } else if (!strcmp(variable, "dcw")) {
    res = s->set_dcw(s, val);
  } else if (!strcmp(variable, "bpc")) {
    res = s->set_bpc(s, val);
  } else if (!strcmp(variable, "wpc")) {
    res = s->set_wpc(s, val);
  } else if (!strcmp(variable, "raw_gma")) {
    res = s->set_raw_gma(s, val);
  } else if (!strcmp(variable, "lenc")) {
    res = s->set_lenc(s, val);
  } else if (!strcmp(variable, "special_effect")) {
    res = s->set_special_effect(s, val);
  } else if (!strcmp(variable, "wb_mode")) {
    res = s->set_wb_mode(s, val);
  } else if (!strcmp(variable, "ae_level")) {
    res = s->set_ae_level(s, val);
  }
#if defined(LED_GPIO_NUM)
  else if (!strcmp(variable, "led_intensity")) {
    led_duty = val;
    if (isStreaming) {
      enable_led(true);
    }
  }
#endif
  else {
    log_i("Unknown command: %s", variable);
    res = -1;
  }

  if (res < 0) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char *p, sensor_t *s, uint16_t reg, uint32_t mask) {
  return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[1024];

  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;
  *p++ = '{';

  if (s->id.PID == OV5640_PID || s->id.PID == OV3660_PID) {
    for (int reg = 0x3400; reg < 0x3406; reg += 2) {
      p += print_reg(p, s, reg, 0xFFF);
    }
    p += print_reg(p, s, 0x3406, 0xFF);
    p += print_reg(p, s, 0x3500, 0xFFFF0);
    p += print_reg(p, s, 0x3503, 0xFF);
    p += print_reg(p, s, 0x350a, 0x3FF);
    p += print_reg(p, s, 0x350c, 0xFFFF);
    for (int reg = 0x5480; reg <= 0x5490; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }
    for (int reg = 0x5380; reg <= 0x538b; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }
    for (int reg = 0x5580; reg < 0x558a; reg++) {
      p += print_reg(p, s, reg, 0xFF);
    }
    p += print_reg(p, s, 0x558a, 0x1FF);
  } else if (s->id.PID == OV2640_PID) {
    p += print_reg(p, s, 0xd3, 0xFF);
    p += print_reg(p, s, 0x111, 0xFF);
    p += print_reg(p, s, 0x132, 0xFF);
  }

  p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
  p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
  p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
  p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
  p += sprintf(p, "\"awb\":%u,", s->status.awb);
  p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
  p += sprintf(p, "\"aec\":%u,", s->status.aec);
  p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
  p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
  p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
  p += sprintf(p, "\"agc\":%u,", s->status.agc);
  p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
  p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
  p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
  p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
  p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
  p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
  p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
  p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
  p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#if defined(LED_GPIO_NUM)
  p += sprintf(p, ",\"led_intensity\":%u", led_duty);
#else
  p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t xclk_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _xclk[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int xclk = atoi(_xclk);
  log_i("Set XCLK: %d MHz", xclk);

  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];
  char _val[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
      httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK ||
      httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  int val = atoi(_val);
  log_i("Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_reg(s, reg, mask, val);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req) {
  char *buf = NULL;
  char _reg[32];
  char _mask[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
      httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int reg = atoi(_reg);
  int mask = atoi(_mask);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->get_reg(s, reg, mask);
  if (res < 0) {
    return httpd_resp_send_500(req);
  }
  log_i("Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

  char buffer[20];
  const char *val = itoa(res, buffer, 10);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, val, strlen(val));
}

static int parse_get_var(char *buf, const char *key, int def) {
  char _int[16];
  if (httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK) {
    return def;
  }
  return atoi(_int);
}

static esp_err_t pll_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int bypass = parse_get_var(buf, "bypass", 0);
  int mul = parse_get_var(buf, "mul", 0);
  int sys = parse_get_var(buf, "sys", 0);
  int root = parse_get_var(buf, "root", 0);
  int pre = parse_get_var(buf, "pre", 0);
  int seld5 = parse_get_var(buf, "seld5", 0);
  int pclken = parse_get_var(buf, "pclken", 0);
  int pclk = parse_get_var(buf, "pclk", 0);
  free(buf);

  log_i("Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d",
        bypass, mul, sys, root, pre, seld5, pclken, pclk);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req) {
  char *buf = NULL;

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }

  int startX = parse_get_var(buf, "sx", 0);
  int startY = parse_get_var(buf, "sy", 0);
  int endX = parse_get_var(buf, "ex", 0);
  int endY = parse_get_var(buf, "ey", 0);
  int offsetX = parse_get_var(buf, "offx", 0);
  int offsetY = parse_get_var(buf, "offy", 0);
  int totalX = parse_get_var(buf, "tx", 0);
  int totalY = parse_get_var(buf, "ty", 0);
  int outputX = parse_get_var(buf, "ox", 0);
  int outputY = parse_get_var(buf, "oy", 0);
  bool scale = parse_get_var(buf, "scale", 0) == 1;
  bool binning = parse_get_var(buf, "binning", 0) == 1;
  free(buf);

  log_i("Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u",
        startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
  sensor_t *s = esp_camera_sensor_get();
  int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

// Redirect / to /combined — stock camera UI disabled (control/resolution is locked)
// Uncomment below and comment out redirect_handler to re-enable the stock camera UI
/*
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    if (s->id.PID == OV3660_PID) {
      return httpd_resp_send(req, (const char *)index_ov3660_html_gz, index_ov3660_html_gz_len);
    } else if (s->id.PID == OV5640_PID) {
      return httpd_resp_send(req, (const char *)index_ov5640_html_gz, index_ov5640_html_gz_len);
    } else {
      return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
    }
  } else {
    log_e("Camera sensor not found");
    return httpd_resp_send_500(req);
  }
}
*/

static esp_err_t redirect_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/combined");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static const char* framesize_to_str(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_QQVGA:  return "160x120 (QQVGA)";
    case FRAMESIZE_HQVGA:  return "240x176 (HQVGA)";
    case FRAMESIZE_QVGA:   return "320x240 (QVGA)";
    case FRAMESIZE_CIF:    return "400x296 (CIF)";
    case FRAMESIZE_VGA:    return "640x480 (VGA)";
    case FRAMESIZE_SVGA:   return "800x600 (SVGA)";
    default: return "Unknown";
  }
}

// Quality config handlers
static esp_err_t quality_get_handler(httpd_req_t *req) {
  // Get current video resolution string
  const char* vid_label = framesize_to_str(vidResList[currentVidResIdx]);
  char aud_label[16];
  snprintf(aud_label, sizeof(aud_label), "x%d", currentAudGain);
  
  char json[256];
  snprintf(json, sizeof(json),
    "{\"vid_idx\":%d,\"vid_label\":\"%s\",\"aud_gain\":%d,\"aud_label\":\"%s\"}",
    currentVidResIdx, vid_label, currentAudGain, aud_label);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t quality_vid_handler(httpd_req_t *req) {
  char buf[64];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    Serial.printf("quality_vid: recv failed ret=%d\n", ret);
    return ESP_FAIL;
  }
  buf[ret] = '\x00';
  Serial.printf("quality_vid: received body: %s\n", buf);

  // Parse JSON for "dir": "up" or "down"
  int new_idx = currentVidResIdx;
  if (strstr(buf, "\"dir\":\"up\"") || strstr(buf, "\"dir\": \"up\"")) {
    if (currentVidResIdx < VID_RES_COUNT - 1) new_idx = currentVidResIdx + 1;
  } else if (strstr(buf, "\"dir\":\"down\"") || strstr(buf, "\"dir\": \"down\"")) {
    if (currentVidResIdx > 0) new_idx = currentVidResIdx - 1;
  }
  Serial.printf("quality_vid: currentIdx=%d newIdx=%d\n", currentVidResIdx, new_idx);

  if (new_idx != currentVidResIdx) {
    currentVidResIdx = new_idx;
    sensor_t *s = esp_camera_sensor_get();
    s->set_framesize(s, (framesize_t)vidResList[currentVidResIdx]);
    Preferences prefs;
    prefs.begin("wifi_config", false);
    prefs.putUChar("vid_res", new_idx);
    prefs.end();
    Serial.printf("Video res applied live: idx %d (%s)\n",
      currentVidResIdx, framesize_to_str(vidResList[currentVidResIdx]));
  }

  char json[128];
  snprintf(json, sizeof(json),
    "{\"status\":\"ok\",\"vid_idx\":%d,\"vid_label\":\"%s\"}",
    currentVidResIdx, framesize_to_str(vidResList[currentVidResIdx]));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json, strlen(json));
  return ESP_OK;
}
static esp_err_t quality_aud_handler(httpd_req_t *req) {
  char buf[64];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    Serial.printf("quality_aud: recv failed ret=%d\n", ret);
    return ESP_FAIL;
  }
  buf[ret] = '\x00';
  Serial.printf("quality_aud: received body: %s\n", buf);

  // Parse JSON for "gain": N or "dir": "up"/"down"
  int new_gain = currentAudGain;
  char* gain_str = strstr(buf, "\"gain\":");
  if (gain_str) {
    int gain = atoi(gain_str + 7);
    if (gain >= AUD_GAIN_MIN && gain <= AUD_GAIN_MAX) {
      new_gain = gain;
    }
  } else if (strstr(buf, "\"dir\":\"up\"") || strstr(buf, "\"dir\": \"up\"")) {
    if (currentAudGain < AUD_GAIN_MAX) new_gain = currentAudGain + 1;
  } else if (strstr(buf, "\"dir\":\"down\"") || strstr(buf, "\"dir\": \"down\"")) {
    if (currentAudGain > AUD_GAIN_MIN) new_gain = currentAudGain - 1;
  }
  Serial.printf("quality_aud: currentGain=%d newGain=%d\n", currentAudGain, new_gain);

  if (new_gain != currentAudGain) {
    currentAudGain = new_gain;
    // Save to NVS (apply immediately, no restart needed)
    Preferences prefs;
    prefs.begin("wifi_config", false);
    prefs.putUChar("aud_gain", new_gain);
    prefs.end();
    Serial.printf("Audio gain changed to %d\n", new_gain);
  }

  char json[64];
  snprintf(json, sizeof(json), "{\"status\":\"ok\",\"gain\":%d}", currentAudGain);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json, strlen(json));
  return ESP_OK;
}

// WiFi credential configuration (GET/POST /wifi, form-urlencoded)
static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void urlDecode(char *s) {
  char *o = s;
  while (*s) {
    if (*s == '+') { *o++ = ' '; s++; }
    else if (*s == '%' && hexVal(s[1]) >= 0 && hexVal(s[2]) >= 0) {
      *o++ = (char)((hexVal(s[1]) << 4) | hexVal(s[2]));
      s += 3;
    } else {
      *o++ = *s++;
    }
  }
  *o = '\0';
}

// Extract key=value from a form-urlencoded body. Requires the key to start at
// the body or right after '&' so "pass" cannot match inside "ap_pass".
static bool getFormValue(const char *body, const char *key, char *out, size_t outLen) {
  size_t klen = strlen(key);
  const char *p = body;
  while ((p = strstr(p, key)) != NULL) {
    if ((p == body || p[-1] == '&') && p[klen] == '=') {
      p += klen + 1;
      const char *e = strchr(p, '&');
      size_t n = e ? (size_t)(e - p) : strlen(p);
      if (n >= outLen) n = outLen - 1;
      memcpy(out, p, n);
      out[n] = '\0';
      urlDecode(out);
      return true;
    }
    p += 1;
  }
  return false;
}

static void htmlEscape(const char *in, char *out, size_t outLen) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 7 < outLen; i++) {
    char c = in[i];
    if (c == '&') { memcpy(out + o, "&amp;", 5); o += 5; }
    else if (c == '<') { memcpy(out + o, "&lt;", 4); o += 4; }
    else if (c == '>') { memcpy(out + o, "&gt;", 4); o += 4; }
    else if (c == '"') { memcpy(out + o, "&quot;", 6); o += 6; }
    else if (c == '\'') { memcpy(out + o, "&#39;", 5); o += 5; }
    else out[o++] = c;
  }
  out[o] = '\0';
}

static esp_err_t wifi_page_handler(httpd_req_t *req) {
  char ip[16];
  bool apActive = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
  IPAddress ipAddr = apActive ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
    (ipAddr[0] & 0xFF), (ipAddr[1] & 0xFF),
    (ipAddr[2] & 0xFF), (ipAddr[3] & 0xFF));

  char ssidEsc[197];
  htmlEscape(staSsid.c_str(), ssidEsc, sizeof(ssidEsc));
  char apSsidEsc[197];
  htmlEscape(apSsid.c_str(), apSsidEsc, sizeof(apSsidEsc));
  const char* staPassHint = staPass.length() > 0
    ? "Password (leave blank to keep current)"
    : "Password (required, 8-63 chars)";

  static char page[2048];
  int len = snprintf(page, sizeof(page),
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>XIAO WiFi Setup</title><style>"
    "body{font-family:sans-serif;background:#111;color:#fff;display:flex;justify-content:center;padding-top:6vh;margin:0;}"
    "form{background:rgba(255,255,255,0.06);padding:24px;border-radius:12px;width:90%%;max-width:360px;}"
    "h2{margin:0 0 4px;font-size:1.2em;}p{margin:0 0 12px;color:#aaa;font-size:0.85em;}"
    ".sec{margin-top:18px;padding-top:14px;border-top:1px solid #333;}"
    ".sec h3{margin:0 0 2px;font-size:0.95em;}"
    ".sec p{margin:0 0 8px;}"
    "label{display:block;font-size:0.8em;color:#aaa;margin:12px 0 4px;}"
    "input{width:100%%;box-sizing:border-box;padding:10px;border-radius:6px;border:1px solid #444;background:#000;color:#fff;}"
    "button{width:100%%;margin-top:18px;padding:12px;background:#0066cc;color:#fff;border:none;border-radius:8px;font-size:1em;cursor:pointer;}"
    "a{color:#88f;font-size:0.85em;}"
    "</style></head><body><form method='POST' action='/wifi'>"
    "<h2>WiFi Setup</h2><p>Mode: %s &middot; IP: %s</p>"
    "<div class='sec'><h3>Station</h3><p>Connect to your router. Filling SSID reboots into STA mode.</p>"
    "<label>SSID</label><input name='ssid' maxlength='32' value='%s'>"
    "<label>%s</label>"
    "<input name='pass' type='password' maxlength='64' placeholder='&bull;&bull;&bull;&bull;&bull;&bull;'>"
    "</div>"
    "<div class='sec'><h3>Access Point</h3><p>This device's own hotspot (default XIAO-CAM).</p>"
    "<label>SSID</label><input name='ap_ssid' maxlength='32' value='%s'>"
    "<label>Password (min 8 chars, leave blank to keep current)</label>"
    "<input name='ap_pass' type='password' maxlength='64' placeholder='&bull;&bull;&bull;&bull;&bull;&bull;'>"
    "</div>"
    "<button type='submit'>Save &amp; Reboot</button>"
    "<p style='margin-top:14px;'><a href='/combined'>&larr; back to stream</a></p>"
    "</form></body></html>",
    apActive ? "AP" : "STA", ip, ssidEsc, staPassHint, apSsidEsc);
  if (len < 0 || (size_t)len >= sizeof(page)) {
    Serial.printf("wifi page truncated: need %d\n", len);
  }

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, page, strlen(page));
  return ESP_OK;
}

static void trimInPlace(char *s) {
  size_t len = strlen(s);
  while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
    s[--len] = '\0';
  }
  size_t start = 0;
  while (s[start] == ' ' || s[start] == '\t') start++;
  if (start) memmove(s, s + start, len - start + 1);
}

static esp_err_t wifi_save_handler(httpd_req_t *req) {
  char buf[768];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    Serial.printf("wifi_save: recv failed ret=%d\n", ret);
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  while (ret > 0 && (buf[ret - 1] == '\r' || buf[ret - 1] == '\n' || buf[ret - 1] == ' ')) {
    buf[--ret] = '\0';
  }

  char ssid[33] = "";
  char pass[65] = "";
  char apS[33] = "";
  char apP[65] = "";
  getFormValue(buf, "ssid", ssid, sizeof(ssid));
  getFormValue(buf, "pass", pass, sizeof(pass));
  getFormValue(buf, "ap_ssid", apS, sizeof(apS));
  getFormValue(buf, "ap_pass", apP, sizeof(apP));
  trimInPlace(ssid);
  trimInPlace(pass);
  trimInPlace(apS);
  trimInPlace(apP);
  Serial.printf("wifi_save rx: ssid=%u, pass=%u, ap_ssid=%u, ap_pass=%u chars\n",
    strlen(ssid), strlen(pass), strlen(apS), strlen(apP));

  size_t plen = strlen(pass);
  size_t aplen = strlen(apP);
  bool staProvided = ssid[0] != '\0';
  bool apProvided = apS[0] != '\0' || aplen > 0;
  if (!staProvided && !apProvided) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nothing to save");
    return ESP_FAIL;
  }
  if (plen > 0 && plen < 8) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Station password must be 8+ chars (or leave blank)");
    return ESP_FAIL;
  }
  if (aplen > 0 && aplen < 8) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP password must be 8+ chars (or leave blank)");
    return ESP_FAIL;
  }

  Preferences prefs;
  prefs.begin("wifi_config", false);
  if (staProvided) {
    String existingPass = prefs.getString("pass", "");
    if (plen == 0 && existingPass.length() == 0) {
      prefs.end();
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password required on first setup (no password stored yet)");
      return ESP_FAIL;
    }
    prefs.putString("ssid", ssid);
    if (plen > 0) prefs.putString("pass", pass);
    prefs.putUChar("mode", 0); // MODE_STA - providing station credentials implies STA intent
  }
  if (apS[0]) prefs.putString("ap_ssid", apS);
  if (aplen > 0) prefs.putString("ap_pass", apP);
  prefs.end();

  Serial.printf("WiFi saved: station ssid='%s' (%s), ap ssid='%s' (%s)\n",
    staProvided ? ssid : "(kept)",
    plen > 0 ? "pass updated" : (staProvided ? "pass kept" : "-"),
    apS[0] ? apS : "(kept)",
    aplen > 0 ? "pass updated" : "-");

  static const char okPage[] =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='4;url=/'>"
    "</head><body style='font-family:sans-serif;background:#111;color:#fff;text-align:center;padding-top:20vh;'>"
    "<h2>Saved &mdash; rebooting&hellip;</h2>"
    "<p>If the connection fails, the device falls back to AP mode (XIAO-CAM) after 20s.</p>"
    "</body></html>";
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, okPage, strlen(okPage));
  delay(300);
  ESP.restart();
  return ESP_OK;
}

esp_err_t combined_page_handler(httpd_req_t *req) {
  char ip[16];
  IPAddress ipAddr = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA)
    ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
    (ipAddr[0] & 0xFF), (ipAddr[1] & 0xFF),
    (ipAddr[2] & 0xFF), (ipAddr[3] & 0xFF));

  sensor_t *s = esp_camera_sensor_get();
  (void)s; // unused after removing resolution header

  const char* vidLabel = framesize_to_str(vidResList[currentVidResIdx]);
  char audLabel[16];
  snprintf(audLabel, sizeof(audLabel), "x%d", currentAudGain);

  static char page[8192];
  int pageLen = snprintf(page, sizeof(page),
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'><meta name='build' content='20260827a'><title>XIAO ESP32S3 - Live Stream</title>"
    "<style>"
    "html,body{width:100%%;min-height:100%%;margin:0;padding:0;background:#111;color:#fff;overflow:hidden;}"
    "#app{display:flex;flex-direction:column;align-items:center;width:100%%;min-height:100%%;}"
    "h1{font-size:clamp(0.9em,3vw,1.4em);margin:4px 0;color:#fff;}"
    "#stage{display:flex;align-items:center;justify-content:center;gap:12px;width:100%%;margin-bottom:18px;}"
    "#vid-wrap{position:relative;flex:1 1 auto;min-width:0;max-width:640px;}"
    "#vid{width:100%%;height:auto;object-fit:contain;background:#000;display:block;}"
    ".reconnect-overlay{position:absolute;top:0;left:0;right:0;bottom:0;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,0.6);color:#fff;font-size:1.2em;z-index:10;pointer-events:none;}"
    ".reconnect-overlay.hidden{display:none;}"
    ".quality-panel{display:flex;flex-direction:column;align-items:center;gap:7px;background:rgba(255,255,255,0.05);border-radius:10px;padding:11px 12px;flex:0 0 auto;}"
    ".quality-label{font-size:0.72em;color:#aaa;text-transform:uppercase;letter-spacing:0.5px;text-align:center;}"
    ".quality-value{font-size:1em;font-weight:600;color:#fff;margin-top:2px;}"
    ".quality-btn{background:#333;color:#fff;border:none;padding:9px 13px;font-size:1.15em;border-radius:7px;cursor:pointer;line-height:1;}"
    ".quality-btn:hover{background:#444;}"
    ".quality-btn:disabled{background:#222;color:#666;cursor:not-allowed;}"
    ".audio-wrap{width:95%%;max-width:640px;margin:16px 0;flex-shrink:0;display:flex;flex-direction:column;align-items:center;gap:24px;position:relative;}"
    ".audio-wrap audio{width:100%%;}"
    ".refresh-btn{background:#0066cc;color:#fff;border:none;padding:21px 48px;font-size:1.3em;border-radius:8px;cursor:pointer;font-weight:500;}"
    ".refresh-btn:hover{background:#0052a3;}"
    ".refresh-btn:disabled{background:#444;color:#888;cursor:not-allowed;}"
    "@media(orientation:portrait){.audio-wrap audio{transform:scale(1.4);transform-origin:center center;}}"
    "@media(orientation:landscape){html,body{overflow:auto;}}"
    "</style></head><body>"
    "<div id='app'>"
    "<a href='/wifi' style='position:fixed;top:6px;right:10px;color:#7aa7ff;font-size:0.85em;text-decoration:none;background:rgba(255,255,255,0.06);padding:6px 10px;border-radius:8px;z-index:30;'>&#9881; WiFi</a>"
    "<h1>XIAO ESP32S3 - Live Stream</h1>"
    "<div id='stage'>"
    "  <div class='quality-panel'>"
    "    <div class='quality-label'>Video</div>"
    "    <button class='quality-btn' id='vid-up' onclick='changeQuality(\"vid\",\"up\")' title='Higher resolution'>▲</button>"
    "    <button class='quality-btn' id='vid-down' onclick='changeQuality(\"vid\",\"down\")' title='Lower resolution'>▼</button>"
    "    <div class='quality-value' id='vid-val'>%s</div>"
    "  </div>"
    "  <div id='vid-wrap'>"
    "    <img id='vid' src='http://%s:81/stream' style='background:#000;'>"
    "    <div id='vid-reconnect' class='reconnect-overlay hidden'>Reconnecting video...</div>"
    "  </div>"
    "  <div class='quality-panel'>"
    "    <div class='quality-label'>Audio</div>"
    "    <button class='quality-btn' id='aud-up' onclick='changeQuality(\"aud\",\"up\")' title='Increase gain'>▲</button>"
    "    <button class='quality-btn' id='aud-down' onclick='changeQuality(\"aud\",\"down\")' title='Decrease gain'>▼</button>"
    "    <div class='quality-value' id='aud-val'>%s</div>"
    "  </div>"
    "</div>"
    "<div class='audio-wrap'>"
    "  <audio id='audio' controls preload='none' src='http://%s:82/audio'></audio>"
    "  <div id='audio-reconnect' class='reconnect-overlay hidden'>Reconnecting audio...</div>"
    "  <button class='refresh-btn' onclick='refreshStreams()' id='refresh-btn'>Refresh</button>"
    "</div>"
    "</div>"
    "<script>"
    "function showOverlay(id){document.getElementById(id).classList.remove('hidden');}"
    "function hideOverlay(id){document.getElementById(id).classList.add('hidden');}"
    "function initStreams(){"
    "  showOverlay('vid-reconnect');"
    "  showOverlay('audio-reconnect');"
    "  const vid=document.getElementById('vid');"
    "  const aud=document.getElementById('audio');"
    "  vid.onload=()=>hideOverlay('vid-reconnect');"
    "  vid.onerror=()=>hideOverlay('vid-reconnect');"
    "  aud.oncanplay=()=>hideOverlay('audio-reconnect');"
    "  aud.onerror=()=>hideOverlay('audio-reconnect');"
    "  aud.load();"
    "  aud.play().catch(()=>{});"
    "}"
    "function refreshStreams(){"
    "  const btn=document.getElementById('refresh-btn');"
    "  btn.disabled=true; btn.textContent='Refreshing...';"
    "  showOverlay('vid-reconnect');"
    "  showOverlay('audio-reconnect');"
    "  const vid=document.getElementById('vid');"
    "  const aud=document.getElementById('audio');"
    "  vid.src='';"
    "  aud.src='';"
    "  vid.src='http://%s:81/stream?t='+Date.now();"
    "  aud.src='http://%s:82/audio?t='+Date.now();"
    "  vid.onload=()=>hideOverlay('vid-reconnect');"
    "  vid.onerror=()=>hideOverlay('vid-reconnect');"
    "  if(vid.readyState>=2) hideOverlay('vid-reconnect');"
    "  aud.oncanplay=()=>hideOverlay('audio-reconnect');"
    "  aud.onerror=()=>hideOverlay('audio-reconnect');"
    "  aud.load();"
    "  aud.play().catch(()=>{});"
    "  setTimeout(()=>{btn.disabled=false; btn.textContent='Refresh'; hideOverlay('vid-reconnect'); hideOverlay('audio-reconnect');}, 10000);"
    "}"
    "async function changeQuality(type, dir){"
    "  const btnId = type + '-' + dir;"
    "  const btn = document.getElementById(btnId);"
    "  if(!btn) return;"
    "  btn.disabled = true;"
    "  btn.textContent = '...';"
    "  try {"
    "    if(type === 'vid') {"
    "      const res = await fetch('/quality/vid', {"
    "        method: 'POST',"
    "        headers: {'Content-Type': 'application/json'},"
    "        body: JSON.stringify({dir: dir})"
    "      });"
    "      await res.json();"
    "      const v = document.getElementById('vid');"
    "      v.src = v.src.split('?')[0] + '?t=' + Date.now();"
    "    } else {"
    "      const res = await fetch('/quality/aud', {"
    "        method: 'POST',"
    "        headers: {'Content-Type': 'application/json'},"
    "        body: JSON.stringify({dir: dir})"
    "      });"
    "      const data = await res.json();"
    "      document.getElementById('aud-val').textContent = 'x' + data.gain;"
    "    }"
    "    const q = await fetch('/quality');"
    "    const qd = await q.json();"
    "    document.getElementById('vid-val').textContent = qd.vid_label;"
    "    document.getElementById('aud-val').textContent = qd.aud_label;"
    "  } catch(e) {"
    "    document.getElementById(type === 'vid' ? 'vid-val' : 'aud-val').textContent = 'ERR';"
    "    console.error('Quality change failed:', e);"
    "  } finally {"
    "    btn.disabled = false;"
    "    btn.textContent = dir === 'up' ? '▲' : '▼';"
    "  }"
    "}"
    "initStreams();"
    "</script>"
    "</body></html>", vidLabel, ip, audLabel, ip, ip, ip);
  if (pageLen < 0 || (size_t)pageLen >= sizeof(page)) {
    Serial.printf("Combined page truncated: need %d bytes\n", pageLen);
  }

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  return httpd_resp_send(req, page, strlen(page));
}



void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 16;
  config.max_open_sockets = 8;

  httpd_uri_t index_uri = {
    .uri        = "/",
    .method     = HTTP_GET,
    .handler    = redirect_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t stream_uri = {
    .uri        = "/stream",
    .method     = HTTP_GET,
    .handler    = stream_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t combined_uri = {
    .uri        = "/combined",
    .method     = HTTP_GET,
    .handler    = combined_page_handler,
    .user_ctx   = NULL
  };

  httpd_uri_t quality_get_uri = {
    .uri        = "/quality",
    .method     = HTTP_GET,
    .handler    = quality_get_handler,
    .user_ctx   = NULL
  };

  httpd_uri_t quality_vid_uri = {
    .uri        = "/quality/vid",
    .method     = HTTP_POST,
    .handler    = quality_vid_handler,
    .user_ctx   = NULL
  };

  httpd_uri_t quality_aud_uri = {
    .uri        = "/quality/aud",
    .method     = HTTP_POST,
    .handler    = quality_aud_handler,
    .user_ctx   = NULL
  };

  httpd_uri_t wifi_get_uri = {
    .uri        = "/wifi",
    .method     = HTTP_GET,
    .handler    = wifi_page_handler,
    .user_ctx   = NULL
  };

  httpd_uri_t wifi_post_uri = {
    .uri        = "/wifi",
    .method     = HTTP_POST,
    .handler    = wifi_save_handler,
    .user_ctx   = NULL
  };

  // Stock camera UI control endpoints — disabled (resolution locked to 640x480)
  // Uncomment below + change index_uri.handler to index_handler to re-enable
  /*
  httpd_uri_t status_uri = {
    .uri        = "/status",
    .method     = HTTP_GET,
    .handler    = status_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t cmd_uri = {
    .uri        = "/control",
    .method     = HTTP_GET,
    .handler    = cmd_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t capture_uri = {
    .uri        = "/capture",
    .method     = HTTP_GET,
    .handler    = capture_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t bmp_uri = {
    .uri        = "/bmp",
    .method     = HTTP_GET,
    .handler    = bmp_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t xclk_uri = {
    .uri        = "/xclk",
    .method     = HTTP_GET,
    .handler    = xclk_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t reg_uri = {
    .uri        = "/reg",
    .method     = HTTP_GET,
    .handler    = reg_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t greg_uri = {
    .uri        = "/greg",
    .method     = HTTP_GET,
    .handler    = greg_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t pll_uri = {
    .uri        = "/pll",
    .method     = HTTP_GET,
    .handler    = pll_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };

  httpd_uri_t win_uri = {
    .uri        = "/resolution",
    .method     = HTTP_GET,
    .handler    = win_handler,
    .user_ctx   = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
    ,.is_websocket           = true
    ,.handle_ws_control_frames = false
    ,.supported_subprotocol  = NULL
#endif
  };
  */

  ra_filter_init(&ra_filter, 20);

  log_i("Starting camera web server on port: '%d'", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &combined_uri);
    httpd_register_uri_handler(camera_httpd, &quality_get_uri);
    httpd_register_uri_handler(camera_httpd, &quality_vid_uri);
    httpd_register_uri_handler(camera_httpd, &quality_aud_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_get_uri);
    httpd_register_uri_handler(camera_httpd, &wifi_post_uri);
    // httpd_register_uri_handler(camera_httpd, &cmd_uri);
    // httpd_register_uri_handler(camera_httpd, &status_uri);
    // httpd_register_uri_handler(camera_httpd, &capture_uri);
    // httpd_register_uri_handler(camera_httpd, &bmp_uri);
    // httpd_register_uri_handler(camera_httpd, &xclk_uri);
    // httpd_register_uri_handler(camera_httpd, &reg_uri);
    // httpd_register_uri_handler(camera_httpd, &greg_uri);
    // httpd_register_uri_handler(camera_httpd, &pll_uri);
    // httpd_register_uri_handler(camera_httpd, &win_uri);
  }

  // Stream server on port 81 (HTML page hardcodes :81 for /stream)
  config.server_port += 1;
  config.ctrl_port += 1;
  log_i("Starting stream server on port: '%d'", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setupLedFlash() {
#if defined(LED_GPIO_NUM)
  ledcAttach(LED_GPIO_NUM, 5000, 8);
#else
  log_i("LED flash is disabled -> LED_GPIO_NUM undefined");
#endif
}
