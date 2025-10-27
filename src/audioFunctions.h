#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

#define USE_I2S_ADC true

// Microphone configuration
// Use GPIO35 (ADC1_CH7). It's input-only and safe with WiFi (ADC1).
// #define MIC_PIN 35 // MAX4466
// #define MIC_PIN 36 // MAX9814
#define MIC_PIN 33 // Carbon microphone

#if USE_I2S_ADC
  #define SAMPLE_RATE 40000 // 40kHz sample rate
  #include "driver/i2s.h"
#else
  // #define SAMPLE_RATE 16000 // 16kHz sample rate. Maximum rate for analogRead-based sampling
  #define SAMPLE_RATE 8000 // 8kHz sample rate
#endif

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


// High-Speed ADC Sampling Using I2S and DMA
#if USE_I2S_ADC
// Configure I2S for ADC sampling
static i2s_config_t i2s_config = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_I2S_LSB,
  .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
  .dma_buf_count = 2,
  .dma_buf_len = 1024,
  .use_apll = false,
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};
#endif




void setupADC() {
#if USE_I2S_ADC
  // IMPORTANT: Using I2S ADC (legacy ADC driver). Do NOT use analogRead/oneshot anywhere to avoid driver conflict.
  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, nullptr);
  // ADC1_CHANNEL_0 GPIO36  ADC1_CHANNEL_4 GPIO32
  // ADC1_CHANNEL_1 GPIO37  ADC1_CHANNEL_5 GPIO33
  // ADC1_CHANNEL_2 GPIO38  ADC1_CHANNEL_6 GPIO34
  // ADC1_CHANNEL_3 GPIO39  ADC1_CHANNEL_7 GPIO35
  i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_5); // GPIO33
  i2s_adc_enable(I2S_NUM_0);
#else
  // Configure ADC (new oneshot driver via Arduino analogRead)
  analogReadResolution(12); // 12-bit resolution (0-4095)
  // analogSetAttenuation(ADC_11db); // Full range 0-3.3V
  // Optional: set per-pin attenuation
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
#endif
}

#if !USE_I2S_ADC
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
#endif

#if USE_I2S_ADC
void samplerTaskDMA(void* pv){
  // DMA-based sampler: reads from I2S ADC and fills audioBuffer
  samplingActive = true;
  // Read in modest chunks to keep latency low while maintaining throughput
  constexpr int CHUNK_SAMPLES = 256; // samples per i2s_read call (16-bit each)
  uint16_t raw[CHUNK_SAMPLES];
  for(;;){
    if (!samplingActive) {
      break;
    }

    size_t bytes_read = 0;
    // Blocking read from I2S DMA; returns multiples of 2 bytes (16-bit samples)
    esp_err_t err = i2s_read(I2S_NUM_0, (void*)raw, sizeof(raw), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK || bytes_read == 0) {
      // Yield briefly on error to avoid tight loop
      vTaskDelay(1);
      continue;
    }
    
    int samples_read = bytes_read / sizeof(uint16_t);
    for (int i = 0; i < samples_read; ++i) {
      // Built-in ADC via I2S returns 12-bit data in lower bits of 16-bit word (implementation-dependent).
      // Normalize to signed 16-bit centered at 0 for WAV PCM 16.
      // Extract 12-bit value: 0..4095, then center to +/- and scale to 16-bit.
      uint16_t adc12 = raw[i] & 0x0FFF; // keep lower 12 bits
      int16_t sample = (int16_t)(((int32_t)adc12 - 2048) << 4);
      
      audioBuffer[bufferIndex++] = sample;
      if (bufferIndex >= BUFFER_SIZE) {
        bufferIndex = 0;
        bufferReady = true;
        // When recording, pause producer until consumer writes buffer to file
        while (recording && bufferReady && samplingActive) {
          vTaskDelay(1);
        }
      }
    }
  }
  vTaskDelete(nullptr);
}
#endif


void startSampling() {
  #if USE_I2S_ADC
  // I2S DMA sampling task
    if (samplerTaskHandle == nullptr) {
      bufferIndex = 0;
      bufferReady = false;
      xTaskCreatePinnedToCore(
          samplerTaskDMA,
          "samplerDMA",
          4096,
          nullptr,
          2, // modest priority
          &samplerTaskHandle,
          0 // pin to core 0; webserver/loop on core 1
      );
    }
    return;
  #else
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
  #endif
}

void stopSampling() {
  if (samplerTaskHandle != nullptr) {
    samplingActive = false;
    // wait a moment for task to exit
    vTaskDelay(2);
    samplerTaskHandle = nullptr;
  #if USE_I2S_ADC
    // Ensure I2S ADC is disabled when stopping
    i2s_adc_disable(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
  #endif
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
