#include "audioSpectrum.h"

#include <math.h>
#include <string.h>

namespace
{
constexpr float TWO_PI_F = 6.28318530717958647692f;

// Full-scale value of a signed 24-bit sample.
constexpr float FULL_SCALE_24 = 8388608.0f;

float clampf(float value, float low, float high)
{
  return value < low ? low : (value > high ? high : value);
}

// The oscilloscope uses a fixed, deliberately conservative gain. A previous
// peak-normalising version expanded tiny room noise to full-screen movement.
constexpr float WAVE_RMS_GATE = 0.00090f;
constexpr float WAVE_PEAK_GATE = 0.00180f;
constexpr float WAVE_SAMPLE_GATE = 0.00065f;
constexpr float WAVE_GAIN = 1050.0f;

// Below this overall RMS the room is treated as silent.
constexpr float SILENCE_RMS = 0.00035f;
constexpr float BASS_SILENCE_RMS = 0.00075f;

// 16 approximately logarithmic groups across 62.5 Hz - 8 kHz at 16 kHz sampling.
constexpr uint8_t BAND_EDGES[AudioSpectrum::BAR_COUNT + 1] = {
    1, 2, 3, 4, 5, 7, 9, 12, 16, 21, 27, 35, 45, 58, 74, 96, 128};

// The INMP441 on CYD wiring leaves low-frequency hum even in a quiet room, and
// a car cabin adds engine and electrical rumble. The first five bands are gated
// and progressively attenuated: real kick transients still cross the thresholds
// while idle rumble stays near zero.
constexpr float BASS_GATE[5] = {28.0f, 22.0f, 17.0f, 12.0f, 8.0f};
constexpr float BASS_GAIN[5] = {0.12f, 0.20f, 0.30f, 0.42f, 0.60f};

// Modest lift for the upper bands so hats and vocals read clearly without
// making the whole display nervous.
constexpr size_t UPPER_BAND_START = 11;
constexpr size_t MID_BAND_START = 8;

// Fast rise, slower fall: responsive bars without flicker.
constexpr float ATTACK = 0.75f;
constexpr float RELEASE = 0.18f;
} // namespace

AudioSpectrum::AudioSpectrum()
{
  reset();
}

void AudioSpectrum::begin()
{
  buildTables();
  reset();
  tablesReady = true;
}

void AudioSpectrum::buildTables()
{
  for (size_t i = 0; i < FFT_SIZE; ++i)
    window[i] = 0.5f - 0.5f * cosf((TWO_PI_F * i) / (FFT_SIZE - 1));

  for (size_t k = 0; k < FFT_SIZE / 2; ++k)
  {
    const float angle = -TWO_PI_F * static_cast<float>(k) / static_cast<float>(FFT_SIZE);
    twiddleReal[k] = cosf(angle);
    twiddleImag[k] = sinf(angle);
  }
}

void AudioSpectrum::reset()
{
  memset(smoothed, 0, sizeof(smoothed));
  for (size_t i = 0; i < WAVEFORM_COUNT; ++i)
    waveformSmoothed[i] = static_cast<float>(WAVE_CENTRE);
}

void AudioSpectrum::forwardTransform()
{
  ensureTables();

  // In-place bit reversal.
  for (size_t i = 1, j = 0; i < FFT_SIZE; ++i)
  {
    size_t bit = FFT_SIZE >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;

    if (i < j)
    {
      const float tempReal = real[i];
      real[i] = real[j];
      real[j] = tempReal;

      const float tempImag = imag[i];
      imag[i] = imag[j];
      imag[j] = tempImag;
    }
  }

  // Iterative radix-2 Cooley-Tukey, twiddles read from the precomputed table.
  for (size_t length = 2; length <= FFT_SIZE; length <<= 1)
  {
    const size_t half = length >> 1;
    const size_t stride = FFT_SIZE / length;

    for (size_t start = 0; start < FFT_SIZE; start += length)
    {
      for (size_t offset = 0; offset < half; ++offset)
      {
        const size_t twiddle = offset * stride;
        const float wReal = twiddleReal[twiddle];
        const float wImag = twiddleImag[twiddle];

        const size_t evenIndex = start + offset;
        const size_t oddIndex = evenIndex + half;

        const float oddReal = real[oddIndex] * wReal - imag[oddIndex] * wImag;
        const float oddImag = real[oddIndex] * wImag + imag[oddIndex] * wReal;
        const float evenReal = real[evenIndex];
        const float evenImag = imag[evenIndex];

        real[evenIndex] = evenReal + oddReal;
        imag[evenIndex] = evenImag + oddImag;
        real[oddIndex] = evenReal - oddReal;
        imag[oddIndex] = evenImag - oddImag;
      }
    }
  }
}

void AudioSpectrum::process(const int32_t *rawSamples, Output &out)
{
  ensureTables();

  // INMP441 provides signed 24-bit audio in the upper bits of each 32-bit word.
  float mean = 0.0f;
  for (size_t i = 0; i < FFT_SIZE; ++i)
  {
    real[i] = static_cast<float>(rawSamples[i] >> 8) / FULL_SCALE_24;
    mean += real[i];
  }
  mean /= FFT_SIZE;

  float rms = 0.0f;
  float peakAmplitude = 0.0f;
  for (size_t i = 0; i < FFT_SIZE; ++i)
  {
    const float sample = real[i] - mean;
    rms += sample * sample;
    const float magnitude = fabsf(sample);
    if (magnitude > peakAmplitude)
      peakAmplitude = magnitude;
    real[i] = sample * window[i];
    imag[i] = 0.0f;
  }
  rms = sqrtf(rms / FFT_SIZE);

  // ---- Oscilloscope trace -------------------------------------------------
  const bool waveformSignalPresent =
      rms >= WAVE_RMS_GATE && peakAmplitude >= WAVE_PEAK_GATE;

  for (size_t i = 0; i < WAVEFORM_COUNT; ++i)
  {
    float target = static_cast<float>(WAVE_CENTRE);
    if (waveformSignalPresent)
    {
      const size_t sampleIndex = (i * (FFT_SIZE - 1)) / (WAVEFORM_COUNT - 1);
      float sample = (static_cast<float>(rawSamples[sampleIndex] >> 8) / FULL_SCALE_24) - mean;
      const float amplitude = fabsf(sample);
      if (amplitude > WAVE_SAMPLE_GATE)
      {
        sample = (sample < 0.0f ? -1.0f : 1.0f) * (amplitude - WAVE_SAMPLE_GATE);
        target = WAVE_CENTRE + sample * WAVE_GAIN;
      }
    }

    target = clampf(target, 6.0f, 94.0f);
    const float follow = waveformSignalPresent ? 0.42f : 0.22f;
    waveformSmoothed[i] += (target - waveformSmoothed[i]) * follow;
    if (!waveformSignalPresent && fabsf(waveformSmoothed[i] - WAVE_CENTRE) < 0.35f)
      waveformSmoothed[i] = static_cast<float>(WAVE_CENTRE);

    out.waveform[i] = static_cast<uint8_t>(clampf(waveformSmoothed[i] + 0.5f, 6.0f, 94.0f));
  }

  // ---- Spectrum -----------------------------------------------------------
  forwardTransform();

  uint16_t levelSum = 0;
  uint8_t strongestLevel = 0;

  for (size_t bar = 0; bar < BAR_COUNT; ++bar)
  {
    float magnitudeSum = 0.0f;
    size_t bins = 0;
    for (uint8_t bin = BAND_EDGES[bar]; bin < BAND_EDGES[bar + 1]; ++bin)
    {
      const float re = real[bin];
      const float im = imag[bin];
      magnitudeSum += sqrtf(re * re + im * im);
      bins++;
    }

    const float magnitude = bins > 0 ? magnitudeSum / (bins * (FFT_SIZE * 0.5f)) : 0.0f;
    const float db = 20.0f * log10f(magnitude + 1.0e-9f);

    const bool silent = rms < SILENCE_RMS;
    float target = silent ? 0.0f : (db + 72.0f) * (100.0f / 52.0f);

    if (bar < 5)
    {
      if (target <= BASS_GATE[bar] || rms < BASS_SILENCE_RMS)
        target = 0.0f;
      else
        target = (target - BASS_GATE[bar]) * BASS_GAIN[bar];
    }
    else if (bar >= UPPER_BAND_START)
    {
      target = target * 1.16f + 3.0f;
    }
    else if (bar >= MID_BAND_START)
    {
      target = target * 1.08f + 1.5f;
    }

    // The treble and mid lifts add a constant offset, so applying the silence
    // gate before them left bands 8-15 sitting at a permanent floor of 1-2 in a
    // dead-quiet cabin: the visualizer never went fully dark. Re-assert the gate
    // after the lifts. (Bands 0-4 were already safe; their gate runs last.)
    if (silent)
      target = 0.0f;

    target = clampf(target, 0.0f, 100.0f);

    if (target > smoothed[bar])
      smoothed[bar] = smoothed[bar] * (1.0f - ATTACK) + target * ATTACK;
    else
      smoothed[bar] = smoothed[bar] * (1.0f - RELEASE) + target * RELEASE;

    out.levels[bar] = static_cast<uint8_t>(clampf(smoothed[bar], 0.0f, 100.0f));
    levelSum += out.levels[bar];
    if (out.levels[bar] > strongestLevel)
      strongestLevel = out.levels[bar];
  }

  // Weight the average more heavily than the peak so the VU needle tracks
  // loudness rather than one spiking band.
  const uint16_t averageLevel = levelSum / BAR_COUNT;
  out.overall = static_cast<uint8_t>(
      clampf(static_cast<float>(averageLevel * 2 + strongestLevel) / 3.0f, 0.0f, 100.0f));
}
