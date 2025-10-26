#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <SPIFFS.h>

// WiFi credentials
const char* ssid = "wifissid";
const char* password = "wifipassword";

// Web server on port 80
WebServer server(80);

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

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(10);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.print("WiFi connected! IP address: ");
  Serial.println(WiFi.localIP());
}

void setupADC() {
  // Configure ADC
  analogReadResolution(12); // 12-bit resolution (0-4095)
  analogSetAttenuation(ADC_11db); // Full range 0-3.3V
  // Optional: set per-pin attenuation
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
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

// HTML page with recording UI
const char* htmlPage = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Audio Recorder</title>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: sans-serif; padding: 1rem; }
      button { font-size: 1rem; padding: .6rem 1rem; }
      #status { margin-left: .5rem; font-weight: bold; }
      #player { display: none; margin-top: 1rem; width: 100%; }
    </style>
</head>
<body>
    <h1>ESP32 MAX4466 Recorder</h1>
    <p>
      <button id="recBtn">● Start Recording</button>
      <span id="status">Idle</span>
    </p>
    <audio id="player" controls></audio>
    <script>
      const recBtn = document.getElementById('recBtn');
      const statusEl = document.getElementById('status');
      const player = document.getElementById('player');
      let recording = false;
      async function startRec(){
        statusEl.textContent = 'Starting…';
        const r = await fetch('/start');
        if (!r.ok) { statusEl.textContent = 'Error'; return; }
        recording = true;
        recBtn.textContent = '■ Stop Recording';
        statusEl.textContent = 'Recording…';
        player.style.display = 'none';
      }
      async function stopRec(){
        statusEl.textContent = 'Stopping…';
        const r = await fetch('/stop');
        if (!r.ok) { statusEl.textContent = 'Error'; return; }
        const j = await r.json().catch(()=>({}));
        recording = false;
        recBtn.textContent = '● Start Recording';
        statusEl.textContent = 'Saved';
        player.src = '/file';
        player.style.display = 'block';
        await player.load();
      }
      recBtn.addEventListener('click', ()=>{
        if (!recording) startRec(); else stopRec();
      });
    </script>
</body>
</html>
)";

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

// Recording control and file handlers
bool beginRecording();
bool endRecording();

void handleStart() {
  if (recording) { server.send(200, "application/json", "{\"status\":\"already-recording\"}"); return; }
  if (beginRecording()) server.send(200, "application/json", "{\"status\":\"recording\"}");
  else server.send(500, "application/json", "{\"status\":\"error\"}");
}

void handleStop() {
  if (!recording) { server.send(200, "application/json", "{\"status\":\"idle\"}"); return; }
  if (endRecording()) server.send(200, "application/json", "{\"status\":\"saved\",\"file\":\"/file\"}");
  else server.send(500, "application/json", "{\"status\":\"error\"}");
}

void handleFile() {
  if (!fileReady || !SPIFFS.exists("/recorded.wav")) { server.send(404, "text/plain", "No recording"); return; }
  File f = SPIFFS.open("/recorded.wav", FILE_READ);
  if (!f) { server.send(500, "text/plain", "Open failed"); return; }
  server.streamFile(f, "audio/wav");
  f.close();
}

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

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 MAX4466 Audio Streaming Server");
  
  // Setup components
  setupADC();
  setupWiFi();
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
  } else {
    Serial.println("SPIFFS mounted");
  }
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/start", handleStart);
  server.on("/stop", handleStop);
  server.on("/file", handleFile);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("HTTP server started");
  Serial.println("Open http://" + WiFi.localIP().toString() + " in your browser");
  // Note: recording starts from the web UI (/start)
}

void loop() {
  server.handleClient();
  
  // Optional: Print audio level for debugging
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    if (bufferReady) {
      // Calculate average amplitude
      long sum = 0;
      for (int i = 0; i < BUFFER_SIZE; i++) {
        sum += abs(audioBuffer[i]);
      }
      int avgLevel = sum / BUFFER_SIZE;
      Serial.println("Audio level: " + String(avgLevel));
    }
    lastPrint = millis();
  }
  
  // If recording, consume full buffers and append to file
  if (recording && bufferReady && recordFile) {
    // write the current buffer to file
    size_t n = recordFile.write((uint8_t*)audioBuffer, BUFFER_SIZE * sizeof(int16_t));
    bytesWritten += n;
    recordFile.flush();
    bufferReady = false; // allow sampler to continue
  }

  delay(1);
}
