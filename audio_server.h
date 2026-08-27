#include <ESP_I2S.h>
#include <WebServer.h>
#include <WiFi.h>

#ifndef CAMERA_MODEL_XIAO_ESP32S3
#define CAMERA_MODEL_XIAO_ESP32S3
#endif

#include "camera_pins.h"

#if defined(HAS_MICROPHONE)

inline I2SClass i2sMic;
inline WebServer AudioServer(82);

static const int sampleRate = SAMPLE_RATE;
static const int bitsPerSample = SAMPLE_BITS;
static const int numChannels = 1;
static const int bufferSize = DMA_BUF_LEN;

// Digital gain multiplier (1 = unity, 2-8 = boost). Start with 4.
static const int AUDIO_GAIN = 4;

struct WAVHeader {
  char chunkId[4];
  uint32_t chunkSize;
  char format[4];
  char subchunk1Id[4];
  uint32_t subchunk1Size;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char subchunk2Id[4];
  uint32_t subchunk2Size;
};

inline void initializeWAVHeader(WAVHeader &header, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels) {
  strncpy(header.chunkId, "RIFF", 4);
  strncpy(header.format, "WAVE", 4);
  strncpy(header.subchunk1Id, "fmt ", 4);
  strncpy(header.subchunk2Id, "data", 4);

  header.chunkSize = 0xFFFFFFFF; // Unknown size = streaming mode
  header.subchunk1Size = 16;
  header.audioFormat = 1;
  header.numChannels = numChannels;
  header.sampleRate = sampleRate;
  header.bitsPerSample = bitsPerSample;
  header.byteRate = (sampleRate * bitsPerSample * numChannels) / 8;
  header.blockAlign = (bitsPerSample * numChannels) / 8;
  header.subchunk2Size = 0;
}

inline void mic_i2s_init() {
  i2sMic.setPinsPdmRx(I2S_WS, I2S_SD);
  i2sMic.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  delay(100);
  Serial.printf("ESP_I2S PDM RX init done, available: %d\n", i2sMic.available());
}

inline void handleAudioStream() {
  WAVHeader wavHeader;
  initializeWAVHeader(wavHeader, sampleRate, bitsPerSample, numChannels);

  WiFiClient audioclient = AudioServer.client();

  audioclient.print("HTTP/1.1 200 OK\r\n");
  audioclient.print("Content-Type: audio/wav\r\n");
  audioclient.print("Access-Control-Allow-Origin: *\r\n");
  audioclient.print("Cache-Control: no-cache\r\n");
  audioclient.print("Pragma: no-cache\r\n");
  audioclient.print("Connection: keep-alive\r\n");
  audioclient.print("\r\n");

  audioclient.write(reinterpret_cast<const uint8_t*>(&wavHeader), sizeof(wavHeader));

  char buffer[512];

  while (true) {
    if (!audioclient.connected()) {
      Serial.println("Audioclient disconnected");
      break;
    }
    size_t avail = i2sMic.available();
    if (avail > 0) {
      size_t toRead = min(avail, (size_t)sizeof(buffer));
      size_t bytesRead = i2sMic.readBytes(buffer, toRead);
      // Apply digital gain with saturation to prevent clipping
      int16_t* samples = reinterpret_cast<int16_t*>(buffer);
      int sampleCount = bytesRead / sizeof(int16_t);
      for (int i = 0; i < sampleCount; i++) {
        int32_t amplified = (int32_t)samples[i] * AUDIO_GAIN;
        if (amplified > 32767) amplified = 32767;
        else if (amplified < -32768) amplified = -32768;
        samples[i] = (int16_t)amplified;
      }
      audioclient.write(reinterpret_cast<uint8_t*>(buffer), bytesRead);
    }
    delay(1);
  }
}

inline void startAudioServer() {
  AudioServer.on("/audio", HTTP_GET, handleAudioStream);
  AudioServer.begin();
}

#endif // HAS_MICROPHONE
