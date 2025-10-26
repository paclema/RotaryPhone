// RingBell - simple non-blocking ring bell driver using an A4988 (STEP/DIR)
#ifndef RINGBELL_H
#define RINGBELL_H
#pragma once
#include <Arduino.h>

class RingBell {
public:
  enum class Mode { Mute, Ring, RingOnce };

  struct Config {
    int pinStep = 19;
    int pinDir = 18;
    // timing: at 40 steps/s -> 25 ms per step => ~20 Hz polarity inversion in full-step
    uint16_t stepIntervalMs = 25;   // period between STEP rising edges
    uint8_t stepPulseUs = 3;        // STEP high pulse width (tWH >= ~1.9 us)

    // burst pattern (used when mode == Ring)
    uint32_t ringOnMs = 2000;       // burst duration in ms
    uint32_t ringOffMs = 4000;      // pause between bursts in ms

    // fine tuning
    bool invertDirEachBurst = true; // toggle DIR at each burst
  };

  RingBell() = default;

  // Must be called from setup() - creates the internal non-blocking task
  void begin();
  void begin(const Config &cfg);

  // Change mode at runtime
  void setMode(Mode m);
  Mode getMode() const;

  // Update configuration on-the-fly (optional)
  void updateConfig(const Config &cfg);

private:
  static void taskEntry(void *arg);
  void taskLoop();
  inline void stepOnce();

  Config _cfg;
  volatile Mode _mode { Mode::Mute };
  TaskHandle_t _task { nullptr };
};

#endif // RINGBELL_H
