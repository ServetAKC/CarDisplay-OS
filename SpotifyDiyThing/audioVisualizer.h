#pragma once

#include <Arduino.h>

#include "audioSpectrum.h"

// ---------------------------------------------------------------------------
// INMP441 microphone capture.
//
// Owns the I2S driver and a sampling task pinned to core 0. All signal
// processing lives in AudioSpectrum, which has no hardware dependencies and is
// covered by the native tests.
//
// Wiring for ESP32-2432S028R / 2-USB CYD:
//   INMP441 VDD -> CN1 3V3      SCK -> GPIO22      SD  -> GPIO35 (input-only)
//   INMP441 GND -> CN1/P3 GND   WS  -> GPIO27      L/R -> GND (left channel)
// GPIO21 is deliberately unused: it drives the TFT backlight.
// ---------------------------------------------------------------------------

#ifndef HONDATHING_MIC_BCLK_PIN
#define HONDATHING_MIC_BCLK_PIN 22
#endif

#ifndef HONDATHING_MIC_WS_PIN
#define HONDATHING_MIC_WS_PIN 27
#endif

#ifndef HONDATHING_MIC_DATA_PIN
#define HONDATHING_MIC_DATA_PIN 35
#endif

class AudioVisualizer
{
public:
  static constexpr size_t BAR_COUNT = AudioSpectrum::BAR_COUNT;
  static constexpr size_t WAVEFORM_COUNT = AudioSpectrum::WAVEFORM_COUNT;

  bool begin();
  bool isReady() const { return ready; }

  // Enabling or disabling always publishes a clean zero frame, so entering or
  // leaving the overlay can never show one stale spectrum.
  void setActive(bool enabled);
  bool isActive() const { return active; }

  // Copies levels, waveform and the VU value from the same microphone frame
  // under one short critical section, so the TFT never mixes two frames.
  // Returns the frame counter, which the renderer uses to skip redundant draws.
  uint32_t copyFrame(uint8_t *levelDestination, size_t levelCount,
                     uint8_t *waveDestination, size_t waveCount,
                     uint8_t *overallDestination);

private:
  static constexpr uint32_t SAMPLE_RATE = 16000;

  volatile bool started = false;
  volatile bool ready = false;
  volatile bool active = false;
  volatile bool resetRequested = false;

  AudioSpectrum spectrum;
  AudioSpectrum::Output published = {};
  uint32_t frameCounter = 0;
  portMUX_TYPE publishMux = portMUX_INITIALIZER_UNLOCKED;

  TaskHandle_t taskHandle = nullptr;
  int32_t rawSamples[AudioSpectrum::FFT_SIZE] = {};

  static void taskTrampoline(void *parameter);
  void taskLoop();
  void publishSilence();
};
