#include "RingBell.h"

void RingBell::begin() {
  begin(Config{});
}

void RingBell::begin(const Config &cfg) {
  _cfg = cfg;
  pinMode(_cfg.pinStep, OUTPUT);
  pinMode(_cfg.pinDir, OUTPUT);
  digitalWrite(_cfg.pinStep, LOW);
  digitalWrite(_cfg.pinDir, LOW);

  // Create the worker task on default core
  xTaskCreatePinnedToCore(
    RingBell::taskEntry,
    "RingBell",
    2048,               // stack size
    this,               // parameter
    1,                  // low priority
    &_task,             // handle
    tskNO_AFFINITY      // no core affinity
  );
}

void RingBell::setMode(Mode m) {
  _mode = m;
}

RingBell::Mode RingBell::getMode() const {
  return _mode;
}

void RingBell::updateConfig(const Config &cfg) {
  _cfg = cfg;
}

void RingBell::taskEntry(void *arg) {
  static_cast<RingBell*>(arg)->taskLoop();
}

inline void RingBell::stepOnce() {
  // A4988 advances on STEP rising edge; provide a short high pulse
  digitalWrite(_cfg.pinStep, HIGH);
  delayMicroseconds(_cfg.stepPulseUs);
  digitalWrite(_cfg.pinStep, LOW);
}

void RingBell::taskLoop() {
  uint32_t lastDirToggleMs = 0;
  bool dirState = false;

  for (;;) {
    Mode mode = _mode; // read volatile snapshot

    if (mode == Mode::Mute) {
      // Idle: give CPU time
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (mode == Mode::RingOnce) {
      // One burst then go back to Mute; keep current DIR
      uint32_t start = millis();
      while ((millis() - start) < _cfg.ringOnMs && _mode == Mode::RingOnce) {
        stepOnce();
        vTaskDelay(pdMS_TO_TICKS(_cfg.stepIntervalMs));
      }
      // Only revert to Mute if user didn't change mode during burst
      if (_mode == Mode::RingOnce) {
        _mode = Mode::Mute;
      }
      continue;
    }

    if (mode == Mode::Ring) {
      // Toggle DIR per burst if enabled
      if (_cfg.invertDirEachBurst) {
        dirState = !dirState;
        digitalWrite(_cfg.pinDir, dirState ? HIGH : LOW);
      }

      // Burst ON
      uint32_t start = millis();
      while ((millis() - start) < _cfg.ringOnMs && _mode == Mode::Ring) {
        stepOnce();
        vTaskDelay(pdMS_TO_TICKS(_cfg.stepIntervalMs));
      }

      // Burst OFF (exit early if mode changed)
      uint32_t offStart = millis();
      while ((millis() - offStart) < _cfg.ringOffMs && _mode == Mode::Ring) {
        vTaskDelay(pdMS_TO_TICKS(25));
      }

      continue;
    }

    // Safety for future modes
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
