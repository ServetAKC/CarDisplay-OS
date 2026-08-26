#pragma once

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "driver/i2s.h"
#include "esp_idf_version.h"

// INMP441 wiring for ESP32-2432S028R / 2-USB CYD:
//   INMP441 VDD  -> CYD CN1 3V3
//   INMP441 GND  -> CYD CN1/P3 GND
//   INMP441 SCK  -> CYD GPIO22
//   INMP441 WS   -> CYD GPIO27
//   INMP441 SD   -> CYD GPIO35 (input-only, ideal for microphone data)
//   INMP441 L/R  -> GND (left channel)
// GPIO21 is deliberately not used because it controls the TFT backlight.
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
  static constexpr size_t BAR_COUNT = 16;
  static constexpr size_t WAVEFORM_COUNT = 64;

  bool begin()
  {
    if (started)
      return ready;

    started = true;
    buildWindow();

    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    config.sample_rate = SAMPLE_RATE;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT; // INMP441 L/R -> GND on this CYD wiring
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 6;
    config.dma_buf_len = 128;
    config.use_apll = false;
    config.tx_desc_auto_clear = false;
    config.fixed_mclk = 0;

    esp_err_t result = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
    if (result != ESP_OK)
    {
      Serial.printf("INMP441: i2s_driver_install failed: %d\n", result);
      return false;
    }

    i2s_pin_config_t pins = {};
#if ESP_IDF_VERSION_MAJOR >= 4
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
    pins.bck_io_num = HONDATHING_MIC_BCLK_PIN;
    pins.ws_io_num = HONDATHING_MIC_WS_PIN;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = HONDATHING_MIC_DATA_PIN;

    result = i2s_set_pin(I2S_PORT, &pins);
    if (result != ESP_OK)
    {
      Serial.printf("INMP441: i2s_set_pin failed: %d\n", result);
      i2s_driver_uninstall(I2S_PORT);
      return false;
    }

    i2s_zero_dma_buffer(I2S_PORT);

    BaseType_t taskResult = xTaskCreatePinnedToCore(
        taskTrampoline,
        "micVisualizer",
        8192,
        this,
        1,
        &taskHandle,
        0);

    if (taskResult != pdPASS)
    {
      Serial.println("INMP441: visualizer task could not start");
      i2s_driver_uninstall(I2S_PORT);
      taskHandle = nullptr;
      return false;
    }

    ready = true;
    Serial.printf("INMP441 ready: BCLK=%d WS=%d SD=%d\n",
                  HONDATHING_MIC_BCLK_PIN,
                  HONDATHING_MIC_WS_PIN,
                  HONDATHING_MIC_DATA_PIN);
    return true;
  }

  bool isReady() const
  {
    return ready;
  }

  void setActive(bool enabled)
  {
    active = enabled;
    resetRequested = true;

    // Publish a clean zero frame on both enter and exit. This avoids one stale
    // oscilloscope/spectrum frame while the DMA buffer is being reset.
    portENTER_CRITICAL(&levelsMux);
    for (size_t i = 0; i < BAR_COUNT; ++i)
      levels[i] = 0;
    for (size_t i = 0; i < WAVEFORM_COUNT; ++i)
      waveform[i] = 50;
    overallLevel = 0;
    frameCounter++;
    portEXIT_CRITICAL(&levelsMux);
  }

  bool isActive() const
  {
    return active;
  }

  uint32_t copyFrame(uint8_t *levelDestination, size_t levelCount,
                     uint8_t *waveDestination, size_t waveCount,
                     uint8_t *overallDestination)
  {
    const size_t levelCopyCount = min(levelCount, BAR_COUNT);
    const size_t waveCopyCount = min(waveCount, WAVEFORM_COUNT);

    // Levels, waveform and VU value belong to one microphone frame. Copy all of
    // them under the same short critical section so the TFT never mixes frames.
    portENTER_CRITICAL(&levelsMux);
    for (size_t i = 0; i < levelCopyCount; ++i)
      levelDestination[i] = levels[i];
    for (size_t i = 0; i < waveCopyCount; ++i)
      waveDestination[i] = waveform[i];
    if (overallDestination != nullptr)
      *overallDestination = overallLevel;
    const uint32_t frame = frameCounter;
    portEXIT_CRITICAL(&levelsMux);
    return frame;
  }

  uint32_t copyLevels(uint8_t *destination, size_t count)
  {
    uint8_t ignoredWave[1] = {50};
    return copyFrame(destination, count, ignoredWave, 0, nullptr);
  }

private:
  static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
  static constexpr uint32_t SAMPLE_RATE = 16000;
  static constexpr size_t FFT_SIZE = 256;

  volatile bool started = false;
  volatile bool ready = false;
  volatile bool active = false;
  volatile bool resetRequested = false;
  uint8_t levels[BAR_COUNT] = {};
  uint8_t waveform[WAVEFORM_COUNT] = {};
  uint8_t overallLevel = 0;
  uint32_t frameCounter = 0;
  portMUX_TYPE levelsMux = portMUX_INITIALIZER_UNLOCKED;
  TaskHandle_t taskHandle = nullptr;

  int32_t rawSamples[FFT_SIZE] = {};
  float realPart[FFT_SIZE] = {};
  float imagPart[FFT_SIZE] = {};
  float window[FFT_SIZE] = {};
  float smoothed[BAR_COUNT] = {};
  float waveformSmoothed[WAVEFORM_COUNT] = {};

  static void taskTrampoline(void *parameter)
  {
    static_cast<AudioVisualizer *>(parameter)->taskLoop();
  }

  void buildWindow()
  {
    for (size_t i = 0; i < FFT_SIZE; ++i)
    {
      window[i] = 0.5f - 0.5f * cosf((2.0f * PI * i) / (FFT_SIZE - 1));
    }
    for (size_t i = 0; i < WAVEFORM_COUNT; ++i)
      waveformSmoothed[i] = 50.0f;
  }

  void taskLoop()
  {
    while (true)
    {
      if (resetRequested)
      {
        resetRequested = false;
        memset(smoothed, 0, sizeof(smoothed));
        for (size_t i = 0; i < WAVEFORM_COUNT; ++i)
          waveformSmoothed[i] = 50.0f;
        i2s_zero_dma_buffer(I2S_PORT);
      }

      size_t bytesRead = 0;

      if (!active)
      {
        // Keep the RX DMA drained while the player screen is open so entering the
        // visualizer never begins with old buffered audio.
        i2s_read(I2S_PORT, rawSamples, 64 * sizeof(int32_t), &bytesRead,
                 pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(15));
        continue;
      }

      // Collect a complete FFT window. i2s_read() is allowed to return a partial
      // DMA block; treating that as a failed frame caused intermittent stalls.
      size_t totalBytes = 0;
      uint8_t *writePtr = reinterpret_cast<uint8_t *>(rawSamples);
      while (active && totalBytes < sizeof(rawSamples))
      {
        size_t chunkBytes = 0;
        const esp_err_t result = i2s_read(
            I2S_PORT, writePtr + totalBytes, sizeof(rawSamples) - totalBytes,
            &chunkBytes, pdMS_TO_TICKS(60));

        if (result != ESP_OK || chunkBytes == 0)
          break;

        totalBytes += chunkBytes;
      }

      if (!active || totalBytes != sizeof(rawSamples))
      {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }

      calculateSpectrum();
      // Leave time for Wi-Fi and the Arduino loop, but keep the microphone DMA
      // serviced frequently enough that the animation never appears frozen.
      vTaskDelay(pdMS_TO_TICKS(4));
    }
  }

  void calculateSpectrum()
  {
    uint8_t nextLevels[BAR_COUNT] = {};
    uint8_t nextWaveform[WAVEFORM_COUNT] = {};
    float mean = 0.0f;
    for (size_t i = 0; i < FFT_SIZE; ++i)
    {
      // INMP441 provides signed 24-bit audio in the upper bits of each 32-bit word.
      const int32_t sample24 = rawSamples[i] >> 8;
      realPart[i] = static_cast<float>(sample24) / 8388608.0f;
      mean += realPart[i];
    }
    mean /= FFT_SIZE;

    float rms = 0.0f;
    float peakAmplitude = 0.0f;
    for (size_t i = 0; i < FFT_SIZE; ++i)
    {
      const float sample = realPart[i] - mean;
      rms += sample * sample;
      peakAmplitude = max(peakAmplitude, fabsf(sample));
      realPart[i] = sample * window[i];
      imagPart[i] = 0.0f;
    }
    rms = sqrtf(rms / FFT_SIZE);

    // Oscilloscope uses a fixed, deliberately conservative gain. The previous
    // peak-normalized waveform expanded tiny room noise to full-screen movement.
    static constexpr float WAVE_RMS_GATE = 0.00090f;
    static constexpr float WAVE_PEAK_GATE = 0.00180f;
    static constexpr float WAVE_SAMPLE_GATE = 0.00065f;
    static constexpr float WAVE_GAIN = 1050.0f;
    const bool waveformSignalPresent =
        rms >= WAVE_RMS_GATE && peakAmplitude >= WAVE_PEAK_GATE;

    for (size_t i = 0; i < WAVEFORM_COUNT; ++i)
    {
      float target = 50.0f;
      if (waveformSignalPresent)
      {
        const size_t sampleIndex = (i * (FFT_SIZE - 1)) / (WAVEFORM_COUNT - 1);
        const int32_t sample24 = rawSamples[sampleIndex] >> 8;
        float sample = (static_cast<float>(sample24) / 8388608.0f) - mean;
        const float amplitude = fabsf(sample);
        if (amplitude > WAVE_SAMPLE_GATE)
        {
          sample = (sample < 0.0f ? -1.0f : 1.0f) *
                   (amplitude - WAVE_SAMPLE_GATE);
          target = 50.0f + sample * WAVE_GAIN;
        }
      }

      target = constrain(target, 6.0f, 94.0f);
      const float follow = waveformSignalPresent ? 0.42f : 0.22f;
      waveformSmoothed[i] += (target - waveformSmoothed[i]) * follow;
      if (!waveformSignalPresent && fabsf(waveformSmoothed[i] - 50.0f) < 0.35f)
        waveformSmoothed[i] = 50.0f;
      nextWaveform[i] = static_cast<uint8_t>(
          constrain(static_cast<int>(waveformSmoothed[i] + 0.5f), 6, 94));
    }

    runFft();

    // 16 approximately logarithmic groups across 62.5 Hz - 8 kHz.
    static constexpr uint8_t edges[BAR_COUNT + 1] = {
        1, 2, 3, 4, 5, 7, 9, 12, 16,
        21, 27, 35, 45, 58, 74, 96, 128};

    uint16_t levelSum = 0;
    uint8_t strongestLevel = 0;
    for (size_t bar = 0; bar < BAR_COUNT; ++bar)
    {
      float magnitudeSum = 0.0f;
      size_t bins = 0;
      for (uint8_t bin = edges[bar]; bin < edges[bar + 1]; ++bin)
      {
        const float re = realPart[bin];
        const float im = imagPart[bin];
        magnitudeSum += sqrtf(re * re + im * im);
        bins++;
      }

      const float magnitude = bins > 0
                                  ? magnitudeSum / (bins * (FFT_SIZE * 0.5f))
                                  : 0.0f;
      const float db = 20.0f * log10f(magnitude + 1.0e-9f);

      float target = (db + 72.0f) * (100.0f / 52.0f);
      if (rms < 0.00035f)
        target = 0.0f;

      // The INMP441/CYD wiring tends to leave a little low-frequency hum even
      // in a quiet room. Suppress that floor progressively in the first four
      // bands, while giving the upper bands a modest lift so hats, vocals and
      // other treble detail read more clearly without making the whole display
      // nervous. These adjustments feed every visualizer mode consistently.
      // Strong car-cabin/electrical rumble rejection. The first five bands are
      // gated and progressively attenuated; real kick transients still cross
      // the thresholds, while idle low-frequency noise stays near zero.
      static constexpr float bassGate[5] = {28.0f, 22.0f, 17.0f, 12.0f, 8.0f};
      static constexpr float bassGain[5] = {0.12f, 0.20f, 0.30f, 0.42f, 0.60f};
      if (bar < 5)
      {
        if (target <= bassGate[bar] || rms < 0.00075f)
          target = 0.0f;
        else
          target = (target - bassGate[bar]) * bassGain[bar];
      }
      else if (bar >= 11)
        target = target * 1.16f + 3.0f;
      else if (bar >= 8)
        target = target * 1.08f + 1.5f;

      target = constrain(target, 0.0f, 100.0f);

      // Fast rise, slower fall gives responsive bars without nervous flicker.
      if (target > smoothed[bar])
        smoothed[bar] = smoothed[bar] * 0.25f + target * 0.75f;
      else
        smoothed[bar] = smoothed[bar] * 0.82f + target * 0.18f;

      nextLevels[bar] = static_cast<uint8_t>(constrain(smoothed[bar], 0.0f, 100.0f));
      levelSum += nextLevels[bar];
      strongestLevel = max(strongestLevel, nextLevels[bar]);
    }

    const uint8_t averageLevel = static_cast<uint8_t>(levelSum / BAR_COUNT);
    const uint8_t nextOverall = static_cast<uint8_t>(
        constrain((averageLevel * 2 + strongestLevel) / 3, 0, 100));

    portENTER_CRITICAL(&levelsMux);
    memcpy(levels, nextLevels, sizeof(levels));
    memcpy(waveform, nextWaveform, sizeof(waveform));
    overallLevel = nextOverall;
    frameCounter++;
    portEXIT_CRITICAL(&levelsMux);
  }

  void runFft()
  {
    // In-place bit reversal.
    for (size_t i = 1, j = 0; i < FFT_SIZE; ++i)
    {
      size_t bit = FFT_SIZE >> 1;
      for (; j & bit; bit >>= 1)
        j ^= bit;
      j ^= bit;

      if (i < j)
      {
        const float tempReal = realPart[i];
        realPart[i] = realPart[j];
        realPart[j] = tempReal;

        const float tempImag = imagPart[i];
        imagPart[i] = imagPart[j];
        imagPart[j] = tempImag;
      }
    }

    // Iterative radix-2 Cooley-Tukey FFT. Only one sin/cos pair is calculated
    // per stage; the remaining twiddles are generated by multiplication.
    for (size_t length = 2; length <= FFT_SIZE; length <<= 1)
    {
      const float angle = -2.0f * PI / static_cast<float>(length);
      const float wLengthReal = cosf(angle);
      const float wLengthImag = sinf(angle);

      for (size_t start = 0; start < FFT_SIZE; start += length)
      {
        float wReal = 1.0f;
        float wImag = 0.0f;
        const size_t half = length >> 1;

        for (size_t offset = 0; offset < half; ++offset)
        {
          const size_t evenIndex = start + offset;
          const size_t oddIndex = evenIndex + half;

          const float oddReal = realPart[oddIndex] * wReal - imagPart[oddIndex] * wImag;
          const float oddImag = realPart[oddIndex] * wImag + imagPart[oddIndex] * wReal;
          const float evenReal = realPart[evenIndex];
          const float evenImag = imagPart[evenIndex];

          realPart[evenIndex] = evenReal + oddReal;
          imagPart[evenIndex] = evenImag + oddImag;
          realPart[oddIndex] = evenReal - oddReal;
          imagPart[oddIndex] = evenImag - oddImag;

          const float nextWReal = wReal * wLengthReal - wImag * wLengthImag;
          wImag = wReal * wLengthImag + wImag * wLengthReal;
          wReal = nextWReal;
        }
      }
    }
  }
};
