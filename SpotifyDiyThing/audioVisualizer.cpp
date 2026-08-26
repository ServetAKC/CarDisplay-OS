#include "audioVisualizer.h"

#include <string.h>

#include "driver/i2s.h"
#include "esp_idf_version.h"

namespace
{
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;

// How long to wait for one DMA block before treating the frame as incomplete.
constexpr TickType_t READ_TIMEOUT = pdMS_TO_TICKS(60);
constexpr TickType_t IDLE_DRAIN_TIMEOUT = pdMS_TO_TICKS(20);
} // namespace

bool AudioVisualizer::begin()
{
  if (started)
    return ready;

  started = true;

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT; // L/R tied to GND
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

  // Build the window and twiddle tables now rather than on the first frame.
  spectrum.begin();

  if (xTaskCreatePinnedToCore(taskTrampoline, "micVisualizer", 8192, this, 1,
                              &taskHandle, 0) != pdPASS)
  {
    Serial.println(F("INMP441: visualizer task could not start"));
    i2s_driver_uninstall(I2S_PORT);
    taskHandle = nullptr;
    return false;
  }

  ready = true;
  Serial.printf("INMP441 ready: BCLK=%d WS=%d SD=%d\n",
                HONDATHING_MIC_BCLK_PIN, HONDATHING_MIC_WS_PIN,
                HONDATHING_MIC_DATA_PIN);
  return true;
}

void AudioVisualizer::publishSilence()
{
  portENTER_CRITICAL(&publishMux);
  memset(published.levels, 0, sizeof(published.levels));
  memset(published.waveform, AudioSpectrum::WAVE_CENTRE, sizeof(published.waveform));
  published.overall = 0;
  frameCounter++;
  portEXIT_CRITICAL(&publishMux);
}

void AudioVisualizer::setActive(bool enabled)
{
  active = enabled;
  resetRequested = true;

  // Publish a clean zero frame on both enter and exit, so no stale
  // oscilloscope or spectrum frame survives the DMA reset.
  publishSilence();
}

uint32_t AudioVisualizer::copyFrame(uint8_t *levelDestination, size_t levelCount,
                                    uint8_t *waveDestination, size_t waveCount,
                                    uint8_t *overallDestination)
{
  const size_t levelCopyCount = levelCount < BAR_COUNT ? levelCount : BAR_COUNT;
  const size_t waveCopyCount = waveCount < WAVEFORM_COUNT ? waveCount : WAVEFORM_COUNT;

  portENTER_CRITICAL(&publishMux);
  memcpy(levelDestination, published.levels, levelCopyCount);
  memcpy(waveDestination, published.waveform, waveCopyCount);
  if (overallDestination != nullptr)
    *overallDestination = published.overall;
  const uint32_t frame = frameCounter;
  portEXIT_CRITICAL(&publishMux);

  return frame;
}

void AudioVisualizer::taskTrampoline(void *parameter)
{
  static_cast<AudioVisualizer *>(parameter)->taskLoop();
}

void AudioVisualizer::taskLoop()
{
  AudioSpectrum::Output frame = {};

  while (true)
  {
    if (resetRequested)
    {
      resetRequested = false;
      spectrum.reset();
      i2s_zero_dma_buffer(I2S_PORT);
    }

    if (!active)
    {
      // Keep the RX DMA drained while the player screen is open, so entering
      // the visualizer never starts with buffered audio from minutes ago.
      size_t drained = 0;
      i2s_read(I2S_PORT, rawSamples, 64 * sizeof(int32_t), &drained, IDLE_DRAIN_TIMEOUT);
      vTaskDelay(pdMS_TO_TICKS(15));
      continue;
    }

    // Collect a complete FFT window. i2s_read() may return a partial DMA block;
    // treating that as a failed frame caused intermittent stalls.
    size_t totalBytes = 0;
    uint8_t *writePtr = reinterpret_cast<uint8_t *>(rawSamples);
    while (active && totalBytes < sizeof(rawSamples))
    {
      size_t chunkBytes = 0;
      const esp_err_t result = i2s_read(I2S_PORT, writePtr + totalBytes,
                                        sizeof(rawSamples) - totalBytes,
                                        &chunkBytes, READ_TIMEOUT);
      if (result != ESP_OK || chunkBytes == 0)
        break;

      totalBytes += chunkBytes;
    }

    if (!active || totalBytes != sizeof(rawSamples))
    {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    spectrum.process(rawSamples, frame);

    portENTER_CRITICAL(&publishMux);
    published = frame;
    frameCounter++;
    portEXIT_CRITICAL(&publishMux);

    // Leave time for Wi-Fi and the Arduino loop, but keep the microphone DMA
    // serviced often enough that the animation never appears frozen.
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}
