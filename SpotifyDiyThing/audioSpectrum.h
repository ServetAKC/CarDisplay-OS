#pragma once

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Microphone DSP: window, FFT, band mapping, gating and smoothing.
//
// Deliberately free of Arduino, FreeRTOS and I2S so it builds and runs on the
// host. `pio test -e native` exercises the FFT, the band edges and the noise
// gates directly - the parts most likely to break silently on hardware, where
// the only symptom would be "the bars look wrong".
//
// AudioVisualizer owns the I2S driver and the sampling task and calls process()
// once per complete window.
// ---------------------------------------------------------------------------

class AudioSpectrum
{
public:
  static constexpr size_t FFT_SIZE = 256;
  static constexpr size_t BAR_COUNT = 16;
  static constexpr size_t WAVEFORM_COUNT = 64;

  // Baseline of the oscilloscope trace, in the 0..100 output range.
  static constexpr uint8_t WAVE_CENTRE = 50;

  struct Output
  {
    uint8_t levels[BAR_COUNT];
    uint8_t waveform[WAVEFORM_COUNT];
    uint8_t overall;
  };

  AudioSpectrum();

  // Clears the attack/release history. Call when the visualizer is opened so a
  // stale frame from the last session cannot bleed into the first new one.
  void reset();

  // Consumes one full window of raw 32-bit I2S words (signed 24-bit audio in
  // the upper bits) and produces one display frame.
  void process(const int32_t *rawSamples, Output &out);

  // In-place radix-2 Cooley-Tukey FFT over the internal work buffers. Public so
  // the native tests can drive it with known signals.
  void forwardTransform();
  float *realPart() { return real; }
  float *imagPart() { return imag; }

private:
  float window[FFT_SIZE];

  // Precomputed twiddle factors, exp(-2*pi*i*k/N) for k < N/2.
  //
  // v0.3.15 generated these by repeated complex multiplication inside the
  // butterfly loop: two extra multiplies per butterfly, and rounding error that
  // accumulated across each stage. A 1 KB table removes both.
  float twiddleReal[FFT_SIZE / 2];
  float twiddleImag[FFT_SIZE / 2];

  float real[FFT_SIZE];
  float imag[FFT_SIZE];

  float smoothed[BAR_COUNT];
  float waveformSmoothed[WAVEFORM_COUNT];

  void buildTables();
};
