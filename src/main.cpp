#include <Arduino.h>

#include <audioFunctions.h>
#include <network.h>

// RingBell class
#include <RingBell.h>
RingBell ring;
RingBell::Config cfg;


void setup() {
  Serial.begin(115200);

  // Default config (GPIO19 STEP, GPIO18 DIR, 25ms per step, 2s on/4s off)
  cfg.pinStep = 19;
  cfg.pinDir = 18;
  cfg.stepIntervalMs = 25; // ~40 steps/s -> ~20 Hz polarity inversion in full-step
  cfg.stepPulseUs = 3;
  cfg.ringOnMs = 2000;
  cfg.ringOffMs = 4000;
  cfg.invertDirEachBurst = true;

  
  if (!SPIFFS.begin(true)) {
    Serial.println("Failed to mount SPIFFS");
  } else {
    Serial.println("SPIFFS mounted");
  }
  
  // Wait for Serial Monitor
  while (!Serial) {}

  // Setup components
  setupADC();
  setupWiFi();
  setupServer();

  ring.begin(cfg);
  ring.setMode(RingBell::Mode::Mute);

  Serial.println("RingBell ready. Serial commands: r=ring, m=mute, o=ring once, 1..5 quick tests");
}

void loop() {
  // Change mode via Serial without blocking
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'r': ring.setMode(RingBell::Mode::Ring); Serial.println("Mode: RING"); break;
      case 'm': ring.setMode(RingBell::Mode::Mute); Serial.println("Mode: MUTE"); break;
      case 'o': ring.setMode(RingBell::Mode::RingOnce); Serial.println("Mode: RING ONCE"); break;
      case '1': cfg.stepIntervalMs = 5;  ring.updateConfig(cfg); ring.setMode(RingBell::Mode::RingOnce); Serial.println("Mode: RING ONCE (5ms)"); break;
      case '2': cfg.stepIntervalMs = 10; ring.updateConfig(cfg); ring.setMode(RingBell::Mode::RingOnce); Serial.println("Mode: RING ONCE (10ms)"); break;
      case '3': cfg.stepIntervalMs = 15; ring.updateConfig(cfg); ring.setMode(RingBell::Mode::RingOnce); Serial.println("Mode: RING ONCE (15ms)"); break;
      case '4': cfg.stepIntervalMs = 20; ring.updateConfig(cfg); ring.setMode(RingBell::Mode::RingOnce); Serial.println("Mode: RING ONCE (20ms)"); break;
      case '5': cfg.stepIntervalMs = 25; ring.updateConfig(cfg); ring.setMode(RingBell::Mode::RingOnce); Serial.println("Mode: RING ONCE (25ms)"); break;
    }
  }

  loopServer();

  // Do other work; internal task handles ringing. Optional yield:
  // vTaskDelay(pdMS_TO_TICKS(1)); // or just yield();
}
