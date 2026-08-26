#include "backlightController.h"

#include "solarTime.h"
#include <time.h>

#include "timing.h"

namespace
{
// Manual brightness ladder, in percent.
constexpr uint8_t LEVELS[BacklightController::LEVEL_COUNT] = {100, 75, 50, 25, 10, 5};

// Cap applied by the automatic sunset dim.
constexpr uint8_t NIGHT_CAP_PERCENT = 25;

// time(nullptr) returns a small number until NTP has synced; anything below
// this is treated as "clock not ready".
constexpr time_t CLOCK_VALID_EPOCH = 1700000000;

} // namespace

void BacklightController::begin(uint8_t backlightPin, float lat, float lon)
{
  latitude = lat;
  longitude = lon;

  // Restore the last manually selected level before enabling the backlight, so
  // a dark cabin never gets a full-brightness flash at boot.
  preferencesReady = preferences.begin("hondathing", false);
  if (preferencesReady)
  {
    const uint8_t saved = preferences.getUChar("brightness", 0);
    manualIndex = saved < LEVEL_COUNT ? saved : 0;
  }

  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_BITS);
  ledcAttachPin(backlightPin, PWM_CHANNEL);
  apply(true);
}

void BacklightController::setLocation(float lat, float lon)
{
  latitude = lat;
  longitude = lon;
}

uint8_t BacklightController::effectivePercent() const
{
  uint8_t percent = LEVELS[manualIndex % LEVEL_COUNT];
  if (nightDimOverrideActive && percent > NIGHT_CAP_PERCENT)
    percent = NIGHT_CAP_PERCENT;
  return percent;
}

void BacklightController::apply(bool force)
{
  const uint8_t percent = effectivePercent();
  if (!force && percent == appliedPercent)
    return;

  appliedPercent = percent;
  const uint32_t maxDuty = (1U << PWM_BITS) - 1U;
  ledcWrite(PWM_CHANNEL, (maxDuty * percent) / 100U);

  Serial.print(F("Display brightness: "));
  Serial.print(percent);
  Serial.print('%');
  Serial.println(nightDimOverrideActive ? F(" (one-shot night dim active)") : F(""));
}

void BacklightController::saveManualLevel()
{
  if (preferencesReady)
    preferences.putUChar("brightness", manualIndex);
}

void BacklightController::cycleManual()
{
  // A manual press is an explicit override for the rest of this night. The
  // saved manual level keeps cycling normally and is never overwritten by the
  // automatic sunset dim.
  nightDimOverrideActive = false;
  manualIndex = (manualIndex + 1) % LEVEL_COUNT;
  saveManualLevel();
  apply(true);
}

bool BacklightController::service()
{
  if (!elapsed(lastCheckTime, CHECK_INTERVAL_MS))
    return false;

  lastCheckTime = millis();

  const bool shouldBeNight = isAfterSunset();
  if (shouldBeNight == automaticNightMode)
    return false;

  automaticNightMode = shouldBeNight;

  // Apply the sunset dim only once. cycleManual() releases the override, and
  // this loop will not re-apply it until the next day/night transition.
  nightDimOverrideActive = shouldBeNight;
  const uint8_t before = appliedPercent;
  apply(true);
  return appliedPercent != before;
}

bool BacklightController::isAfterSunset() const
{
  const time_t now = time(nullptr);
  if (now < CLOCK_VALID_EPOCH)
    return false; // NTP has not synced yet

  struct tm utcNow;
  gmtime_r(&now, &utcNow);
  return isNight(utcNow, latitude, longitude);
}
