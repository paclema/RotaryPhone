#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>

// WiFi credentials
const char* ssid = "wifissid";
const char* password = "wifipassword";

// Web server on port 80
WebServer server(80);

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

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}


void setupServer() {
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

void loopServer() {
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

}
