//#include <XPT2046_Touchscreen.h>
#include "CYD28_TouchscreenR.h"
#include <SPI.h>

#define CYD28_DISPLAY_HOR_RES_MAX 320
#define CYD28_DISPLAY_VER_RES_MAX 240

// ESP32-2432S028 2-USB touch tuning for the bottom media controls.
#define TOUCH_PRESSURE_THRESHOLD 120
#define TOUCH_PREVIOUS_MAX_X 124
#define TOUCH_PLAY_MIN_X 125
#define TOUCH_PLAY_MAX_X 195
#define TOUCH_NEXT_MIN_X 196
#define TOUCH_MIN_Y 190
#define TOUCH_MAX_Y 239
#define TOUCH_SAMPLE_MS 8
#define TOUCH_COMMAND_LOCK_MS 180

// Header hotspots. The center clock opens clock mode; the Wi-Fi bars open the
// microphone visualizer. Overlay X buttons close; visualizer body taps cycle modes.
#define TOUCH_CLOCK_MIN_X 100
#define TOUCH_CLOCK_MAX_X 230
#define TOUCH_CLOCK_MIN_Y 0
#define TOUCH_CLOCK_MAX_Y 44
#define TOUCH_VISUALIZER_MIN_X 270
#define TOUCH_VISUALIZER_MAX_X 319
#define TOUCH_VISUALIZER_MIN_Y 0
#define TOUCH_VISUALIZER_MAX_Y 44

// Full-screen clock/visualizer close button (top-right).
#define TOUCH_OVERLAY_CLOSE_MIN_X 276
#define TOUCH_OVERLAY_CLOSE_MAX_X 319
#define TOUCH_OVERLAY_CLOSE_MIN_Y 0
#define TOUCH_OVERLAY_CLOSE_MAX_Y 46

// Large button shown only on the Wi-Fi setup screen.
#define TOUCH_OFFLINE_VISUALIZER_MIN_X 32
#define TOUCH_OFFLINE_VISUALIZER_MAX_X 288
#define TOUCH_OFFLINE_VISUALIZER_MIN_Y 176
#define TOUCH_OFFLINE_VISUALIZER_MAX_Y 232

// Spotify badge hotspot: cycles display brightness 100 -> 75 -> 50 -> 25 -> 10 -> 5.
#define TOUCH_BRIGHTNESS_MIN_X 0
#define TOUCH_BRIGHTNESS_MAX_X 95
#define TOUCH_BRIGHTNESS_MIN_Y 0
#define TOUCH_BRIGHTNESS_MAX_Y 44

bool previousTrackStatus = false;
bool playPauseStatus = false;
bool nextTrackStatus = false;
bool clockToggleStatus = false;
bool visualizerToggleStatus = false;
bool visualizerNextModeStatus = false;
bool brightnessCycleStatus = false;
bool offlineVisualizerStatus = false;

CYD28_TouchR ts(CYD28_DISPLAY_HOR_RES_MAX, CYD28_DISPLAY_VER_RES_MAX);
SpotifyArduino *spotify_touch;

// -1: previous, 0: none, 1: next, 2: play/pause, 3: clock,
// 4: visualizer open/close, 5: brightness, 6: next visualizer mode
volatile int8_t pendingTouchAction = 0;
volatile bool touchBlocked = false;
volatile unsigned long touchBlockedUntil = 0;
volatile bool touchClockModeActive = false;
volatile bool touchVisualizerModeActive = false;
volatile bool touchSetupPortalModeActive = false;
TaskHandle_t touchReaderTaskHandle = nullptr;

void touchReaderTask(void *parameter)
{
  bool pressLatched = false;
  uint8_t releasedSamples = 3;

  while (true)
  {
    const bool isDown = ts.touched();

    // Ignore the rest of the same physical press. The short 180 ms guard filters
    // contact bounce without making intentional taps feel sluggish.
    if (touchBlocked)
    {
      if (isDown)
      {
        releasedSamples = 0;
      }
      else if (releasedSamples < 3)
      {
        releasedSamples++;
      }

      if (!isDown && releasedSamples >= 3 && millis() >= touchBlockedUntil)
      {
        pressLatched = false;
        touchBlocked = false;
      }

      vTaskDelay(pdMS_TO_TICKS(TOUCH_SAMPLE_MS));
      continue;
    }

    if (isDown)
    {
      releasedSamples = 0;

      if (!pressLatched && pendingTouchAction == 0)
      {
        CYD28_TS_Point p = ts.getPointScaled();

        if (p.z >= TOUCH_PRESSURE_THRESHOLD)
        {
          if (touchSetupPortalModeActive)
          {
            if (p.x >= TOUCH_OFFLINE_VISUALIZER_MIN_X &&
                p.x <= TOUCH_OFFLINE_VISUALIZER_MAX_X &&
                p.y >= TOUCH_OFFLINE_VISUALIZER_MIN_Y &&
                p.y <= TOUCH_OFFLINE_VISUALIZER_MAX_Y)
              pendingTouchAction = 7;
            pressLatched = true;
          }
          else if (touchClockModeActive)
          {
            // Clock only closes from the top-right X; taps elsewhere do nothing.
            if (p.x >= TOUCH_OVERLAY_CLOSE_MIN_X && p.x <= TOUCH_OVERLAY_CLOSE_MAX_X &&
                p.y >= TOUCH_OVERLAY_CLOSE_MIN_Y && p.y <= TOUCH_OVERLAY_CLOSE_MAX_Y)
              pendingTouchAction = 3;
            pressLatched = true;
          }
          else if (touchVisualizerModeActive)
          {
            // Top-right X closes. Any other tap advances to the next visualizer.
            if (p.x >= TOUCH_OVERLAY_CLOSE_MIN_X && p.x <= TOUCH_OVERLAY_CLOSE_MAX_X &&
                p.y >= TOUCH_OVERLAY_CLOSE_MIN_Y && p.y <= TOUCH_OVERLAY_CLOSE_MAX_Y)
              pendingTouchAction = 4;
            else
              pendingTouchAction = 6;
            pressLatched = true;
          }
          else if (p.x >= TOUCH_VISUALIZER_MIN_X && p.x <= TOUCH_VISUALIZER_MAX_X &&
                   p.y >= TOUCH_VISUALIZER_MIN_Y && p.y <= TOUCH_VISUALIZER_MAX_Y)
          {
            // Header Wi-Fi indicator area.
            pendingTouchAction = 4;
            pressLatched = true;
          }
          else if (p.x >= TOUCH_BRIGHTNESS_MIN_X && p.x <= TOUCH_BRIGHTNESS_MAX_X &&
                   p.y >= TOUCH_BRIGHTNESS_MIN_Y && p.y <= TOUCH_BRIGHTNESS_MAX_Y)
          {
            // Header Spotify badge area.
            pendingTouchAction = 5;
            pressLatched = true;
          }
          else if (p.x >= TOUCH_CLOCK_MIN_X && p.x <= TOUCH_CLOCK_MAX_X &&
                   p.y >= TOUCH_CLOCK_MIN_Y && p.y <= TOUCH_CLOCK_MAX_Y)
          {
            // Header clock area.
            pendingTouchAction = 3;
            pressLatched = true;
          }
          else if (p.y >= TOUCH_MIN_Y && p.y <= TOUCH_MAX_Y)
          {
            if (p.x <= TOUCH_PREVIOUS_MAX_X)
            {
              pendingTouchAction = -1;
              pressLatched = true;
            }
            else if (p.x >= TOUCH_PLAY_MIN_X && p.x <= TOUCH_PLAY_MAX_X)
            {
              pendingTouchAction = 2;
              pressLatched = true;
            }
            else if (p.x >= TOUCH_NEXT_MIN_X)
            {
              pendingTouchAction = 1;
              pressLatched = true;
            }
          }
        }
      }
    }
    else
    {
      if (releasedSamples < 3)
      {
        releasedSamples++;
      }

      if (releasedSamples >= 3)
      {
        pressLatched = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(TOUCH_SAMPLE_MS));
  }
}

void touchSetup(SpotifyArduino *spotifyObj)
{
  ts.begin();
  ts.setRotation(1);
  ts.setThreshold(TOUCH_PRESSURE_THRESHOLD);
  spotify_touch = spotifyObj;

  xTaskCreatePinnedToCore(
      touchReaderTask,
      "cydTouch",
      2048,
      nullptr,
      2,
      &touchReaderTaskHandle,
      1);
}

void setTouchClockMode(bool active)
{
  touchClockModeActive = active;
}

void setTouchVisualizerMode(bool active)
{
  touchVisualizerModeActive = active;
}

void setTouchSetupPortalMode(bool active)
{
  touchSetupPortalModeActive = active;
}

bool handleTouched()
{
  previousTrackStatus = false;
  playPauseStatus = false;
  nextTrackStatus = false;
  clockToggleStatus = false;
  visualizerToggleStatus = false;
  visualizerNextModeStatus = false;
  brightnessCycleStatus = false;
  offlineVisualizerStatus = false;

  const int8_t action = pendingTouchAction;
  if (action == 0)
  {
    return false;
  }

  pendingTouchAction = 0;
  touchBlocked = true;
  touchBlockedUntil = millis() + TOUCH_COMMAND_LOCK_MS;

  if (action < 0)
  {
    previousTrackStatus = true;
  }
  else if (action == 2)
  {
    playPauseStatus = true;
  }
  else if (action == 3)
  {
    clockToggleStatus = true;
  }
  else if (action == 4)
  {
    visualizerToggleStatus = true;
  }
  else if (action == 5)
  {
    brightnessCycleStatus = true;
  }
  else if (action == 6)
  {
    visualizerNextModeStatus = true;
  }
  else if (action == 7)
  {
    offlineVisualizerStatus = true;
  }
  else
  {
    nextTrackStatus = true;
  }

  return true;
}
