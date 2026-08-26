#pragma once

#include <Arduino.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Backlight brightness: manual cycling plus a one-shot automatic sunset dim.
//
// Manual levels cycle 100/75/50/25/10/5 and persist in NVS. At sunset the
// controller applies a one-shot 25% cap; the first manual press releases that
// cap so the full range stays available for the rest of the night. Daylight
// re-arms it for the next sunset.
//
// The coordinates come from DeviceConfig, so the dim fires at the right hour
// wherever the car actually is.
// ---------------------------------------------------------------------------

class BacklightController
{
public:
  static constexpr uint8_t LEVEL_COUNT = 6;

  // `backlightPin` is the TFT backlight GPIO (21 on the CYD).
  void begin(uint8_t backlightPin, float latitude, float longitude);

  // Advance to the next manual level and release any active night dim.
  void cycleManual();

  // Re-evaluates the day/night state at most twice a minute. Returns true when
  // the effective level changed, so the caller can show a brightness toast.
  bool service();

  uint8_t effectivePercent() const;

  // Sunset dimming follows the device location; call after the config changes.
  void setLocation(float latitude, float longitude);

private:
  static constexpr uint8_t PWM_CHANNEL = 7;
  static constexpr uint16_t PWM_FREQUENCY = 5000;
  static constexpr uint8_t PWM_BITS = 8;
  static constexpr unsigned long CHECK_INTERVAL_MS = 30000;

  Preferences preferences;
  bool preferencesReady = false;
  uint8_t manualIndex = 0;
  uint8_t appliedPercent = 255; // deliberately invalid so the first apply runs
  bool automaticNightMode = false;
  bool nightDimOverrideActive = false;
  unsigned long lastCheckTime = 0;
  float latitude = 0.0f;
  float longitude = 0.0f;

  void apply(bool force);
  void saveManualLevel();
  bool isAfterSunset() const;
};
