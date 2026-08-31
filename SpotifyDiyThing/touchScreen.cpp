#include "touchScreen.h"

#include <SPI.h>

#include "CYD28_TouchscreenR.h"
#include "cydTheme.h"
#include "timing.h"

namespace
{
constexpr int16_t DISPLAY_HOR_RES_MAX = 320;
constexpr int16_t DISPLAY_VER_RES_MAX = 240;

// ESP32-2432S028 2-USB touch tuning for the bottom media controls.
constexpr int PRESSURE_THRESHOLD = 120;
constexpr unsigned long SAMPLE_MS = 8;
constexpr unsigned long COMMAND_LOCK_MS = 180;

// Number of consecutive "not touched" samples before a new press may latch.
constexpr uint8_t RELEASE_SAMPLES = 3;

struct Hotspot
{
  int16_t minX, maxX, minY, maxY;

  bool contains(int16_t x, int16_t y) const
  {
    return x >= minX && x <= maxX && y >= minY && y <= maxY;
  }
};

// Player screen hotspots.
constexpr Hotspot PREVIOUS_ZONE = {0, 124, 190, 239};
constexpr Hotspot PLAY_ZONE = {125, 195, 190, 239};
constexpr Hotspot NEXT_ZONE = {196, 319, 190, 239};

// Header: Spotify badge cycles brightness, centre clock opens clock mode, the
// Wi-Fi bars open the microphone visualizer.
constexpr Hotspot BRIGHTNESS_ZONE = {0, 95, 0, 44};
constexpr Hotspot CLOCK_ZONE = {100, 230, 0, 44};
constexpr Hotspot VISUALIZER_ZONE = {270, 319, 0, 44};

// Full-screen overlay close button (top-right).
constexpr Hotspot OVERLAY_CLOSE_ZONE = {276, 319, 0, 46};

// Large button shown only on the Wi-Fi setup screen.
constexpr Hotspot OFFLINE_VISUALIZER_ZONE = {32, 288, 176, 232};

CYD28_TouchR ts(DISPLAY_HOR_RES_MAX, DISPLAY_VER_RES_MAX);

volatile TouchAction pendingAction = TouchAction::None;
volatile bool blocked = false;
volatile unsigned long blockedUntil = 0;
volatile bool clockModeActive = false;
volatile bool visualizerModeActive = false;
volatile bool setupPortalModeActive = false;

TaskHandle_t touchReaderTaskHandle = nullptr;

TouchAction classify(int16_t x, int16_t y)
{
  if (setupPortalModeActive)
  {
    return OFFLINE_VISUALIZER_ZONE.contains(x, y) ? TouchAction::OpenOfflineVisualizer
                                                  : TouchAction::None;
  }

  if (clockModeActive)
  {
    // Clock only closes from the top-right X; taps elsewhere do nothing.
    return OVERLAY_CLOSE_ZONE.contains(x, y) ? TouchAction::ToggleClock
                                             : TouchAction::None;
  }

  if (visualizerModeActive)
  {
    // Top-right X closes. Otherwise the overlay is split down the middle:
    // right half advances, left half goes back.
    //
    // v0.3 cycled one way from any tap, which was fine for twenty modes and is
    // not for forty - overshooting the one you wanted meant thirty-nine more
    // taps to come back to it.
    if (OVERLAY_CLOSE_ZONE.contains(x, y))
      return TouchAction::ToggleVisualizer;
    return x >= layout::CENTRE_X ? TouchAction::NextVisualizerStyle
                                 : TouchAction::PreviousVisualizerStyle;
  }

  if (VISUALIZER_ZONE.contains(x, y))
    return TouchAction::ToggleVisualizer;
  if (BRIGHTNESS_ZONE.contains(x, y))
    return TouchAction::CycleBrightness;
  if (CLOCK_ZONE.contains(x, y))
    return TouchAction::ToggleClock;
  if (PREVIOUS_ZONE.contains(x, y))
    return TouchAction::Previous;
  if (PLAY_ZONE.contains(x, y))
    return TouchAction::PlayPause;
  if (NEXT_ZONE.contains(x, y))
    return TouchAction::Next;

  return TouchAction::None;
}

void touchReaderTask(void *)
{
  bool pressLatched = false;
  uint8_t releasedSamples = RELEASE_SAMPLES;

  while (true)
  {
    const bool isDown = ts.touched();

    // Ignore the rest of the same physical press. The short guard filters
    // contact bounce without making intentional taps feel sluggish.
    if (blocked)
    {
      if (isDown)
        releasedSamples = 0;
      else if (releasedSamples < RELEASE_SAMPLES)
        releasedSamples++;

      if (!isDown && releasedSamples >= RELEASE_SAMPLES && timeReached(blockedUntil))
      {
        pressLatched = false;
        blocked = false;
      }

      vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
      continue;
    }

    if (isDown)
    {
      releasedSamples = 0;

      if (!pressLatched && pendingAction == TouchAction::None)
      {
        const CYD28_TS_Point p = ts.getPointScaled();
        if (p.z >= PRESSURE_THRESHOLD)
        {
          const TouchAction action = classify(p.x, p.y);
          // Latch even on a dead zone, so one press cannot produce two actions
          // by sliding from an inert area onto a live one.
          if (setupPortalModeActive || clockModeActive || visualizerModeActive ||
              action != TouchAction::None)
          {
            pendingAction = action;
            pressLatched = true;
          }
        }
      }
    }
    else
    {
      if (releasedSamples < RELEASE_SAMPLES)
        releasedSamples++;
      if (releasedSamples >= RELEASE_SAMPLES)
        pressLatched = false;
    }

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
  }
}
} // namespace

void touchSetup()
{
  ts.begin();
  ts.setRotation(1);
  ts.setThreshold(PRESSURE_THRESHOLD);

  xTaskCreatePinnedToCore(touchReaderTask, "cydTouch", 2048, nullptr, 2,
                          &touchReaderTaskHandle, 1);
}

TouchAction takeTouchAction()
{
  const TouchAction action = pendingAction;
  if (action == TouchAction::None)
    return TouchAction::None;

  pendingAction = TouchAction::None;
  blocked = true;
  blockedUntil = deadlineIn(COMMAND_LOCK_MS);
  return action;
}

void setTouchClockMode(bool active) { clockModeActive = active; }
void setTouchVisualizerMode(bool active) { visualizerModeActive = active; }
void setTouchSetupPortalMode(bool active) { setupPortalModeActive = active; }
