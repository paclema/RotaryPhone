#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

// MAX4466 configuration
// Use GPIO35 (ADC1_CH7). It's input-only and safe with WiFi (ADC1).
#define MIC_PIN 35
#define SAMPLE_RATE 8000 
#define SAMPLE_BITS 16
#define BUFFER_SIZE 2048


// Audio buffer
int16_t audioBuffer[BUFFER_SIZE];
volatile bool bufferReady = false;
volatile bool recording = false;
bool fileReady = false;
File recordFile;
size_t bytesWritten = 0; // audio data bytes written

// Sampling state
volatile int bufferIndex = 0;

// Simple task for sampling at SAMPLE_RATE
TaskHandle_t samplerTaskHandle = nullptr;
volatile bool samplingActive = false;

void setupADC() {
  // Configure ADC
  analogReadResolution(12); // 12-bit resolution (0-4095)
  analogSetAttenuation(ADC_11db); // Full range 0-3.3V
  // Optional: set per-pin attenuation
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
}

void samplerTask(void* pv) {
  const uint32_t usPerSample = 1000000UL / SAMPLE_RATE;
  uint32_t nextTick = micros();
  uint32_t samplesSinceYield = 0;
  samplingActive = true;
  for (;;) {
    if (!samplingActive) {
      break; // exit task
    }
    // Read analog value from MAX4466
    int rawValue = analogRead(MIC_PIN); // 0..4095 (12-bit)

    // Convert to signed 16-bit audio sample centered around 0
    int16_t sample = (int16_t)((rawValue - 2048) << 4); // scale to ~16-bit

    audioBuffer[bufferIndex++] = sample;
    if (bufferIndex >= BUFFER_SIZE) {
      bufferIndex = 0;
      bufferReady = true;
      // Pause sampling until buffer is consumed when recording
      while (recording && bufferReady && samplingActive) {
        vTaskDelay(1);
      }
    }

    // Wait until next sample time (simple busy-wait for timing accuracy)
    nextTick += usPerSample;
    while ((int32_t)(micros() - nextTick) < 0) {
      // tight loop; yields are avoided to maintain timing
    }

    // If drift occurred (e.g., long ISR), resync
    if ((int32_t)(micros() - nextTick) > (int32_t)(usPerSample * 4)) {
      nextTick = micros();
    }

    // Occasionally yield to allow other tasks and idle to run
    if (++samplesSinceYield >= 2048) { // ~128ms at 16kHz
      samplesSinceYield = 0;
      vTaskDelay(1); // let idle/task wdt run (~1ms)
    }
  }
  vTaskDelete(nullptr);
}

void startSampling() {
  if (samplerTaskHandle == nullptr) {
    bufferIndex = 0;
    bufferReady = false;
    xTaskCreatePinnedToCore(
        samplerTask,
        "sampler",
        4096,
        nullptr,
        2, // modest priority
        &samplerTaskHandle,
        0 // pin to core 0; webserver/loop on core 1
    );
  }
}

void stopSampling() {
  if (samplerTaskHandle != nullptr) {
    samplingActive = false;
    // wait a moment for task to exit
    vTaskDelay(2);
    samplerTaskHandle = nullptr;
  }
}

// Recording control and file handlers
bool beginRecording() {
  if (SPIFFS.exists("/recorded.wav")) SPIFFS.remove("/recorded.wav");
  recordFile = SPIFFS.open("/recorded.wav", FILE_WRITE);
  if (!recordFile) { Serial.println("Failed to open file for writing"); return false; }
  // Write WAV header placeholder
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t bitsPerSample = SAMPLE_BITS;
  uint16_t channels = 1;
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  uint8_t wavHeader[44] = {
    'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0,
    1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0
  };
  memcpy(&wavHeader[24], &sampleRate, 4);
  memcpy(&wavHeader[28], &byteRate, 4);
  recordFile.write(wavHeader, 44);
  recordFile.flush();
  bytesWritten = 0;
  fileReady = false;
  recording = true;
  startSampling();
  return true;
}


bool endRecording() {
  recording = false;
  delay(50);
  stopSampling();
  if (!recordFile) return false;
  uint32_t subchunk2Size = bytesWritten;
  uint32_t chunkSize = 36 + subchunk2Size;
  recordFile.seek(4, SeekSet); recordFile.write((uint8_t*)&chunkSize, 4);
  recordFile.seek(40, SeekSet); recordFile.write((uint8_t*)&subchunk2Size, 4);
  recordFile.flush();
  recordFile.close();
  fileReady = true;
  return true;
}
