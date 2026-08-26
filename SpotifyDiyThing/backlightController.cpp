#include "backlightController.h"

#include <math.h>
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

double normalizeDegrees(double value)
{
  value = fmod(value, 360.0);
  return value < 0.0 ? value + 360.0 : value;
}

double degreesToRadians(double value) { return value * 0.017453292519943295; }
double radiansToDegrees(double value) { return value * 57.29577951308232; }
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
    return false; // NTP not ready yet

  struct tm utcNow;
  gmtime_r(&now, &utcNow);

  const double currentUtcHour =
      utcNow.tm_hour + (utcNow.tm_min / 60.0) + (utcNow.tm_sec / 3600.0);
  const double sunriseUtc = solarEventUtcHour(utcNow, true);
  const double sunsetUtc = solarEventUtcHour(utcNow, false);

  return currentUtcHour >= sunsetUtc || currentUtcHour < sunriseUtc;
}

// NOAA-style sunrise/sunset approximation. Returns a UTC decimal hour.
double BacklightController::solarEventUtcHour(const struct tm &utcDate, bool sunrise) const
{
  constexpr double zenith = 90.833; // standard refraction + solar radius
  const int dayOfYear = utcDate.tm_yday + 1;
  const double lngHour = longitude / 15.0;
  const double approximateTime =
      dayOfYear + (((sunrise ? 6.0 : 18.0) - lngHour) / 24.0);

  const double meanAnomaly = (0.9856 * approximateTime) - 3.289;
  double trueLongitude =
      meanAnomaly +
      (1.916 * sin(degreesToRadians(meanAnomaly))) +
      (0.020 * sin(2.0 * degreesToRadians(meanAnomaly))) +
      282.634;
  trueLongitude = normalizeDegrees(trueLongitude);

  double rightAscension = radiansToDegrees(
      atan(0.91764 * tan(degreesToRadians(trueLongitude))));
  rightAscension = normalizeDegrees(rightAscension);

  const double longitudeQuadrant = floor(trueLongitude / 90.0) * 90.0;
  const double rightAscensionQuadrant = floor(rightAscension / 90.0) * 90.0;
  rightAscension += longitudeQuadrant - rightAscensionQuadrant;
  rightAscension /= 15.0;

  const double sinDeclination = 0.39782 * sin(degreesToRadians(trueLongitude));
  const double cosDeclination = cos(asin(sinDeclination));

  const double cosHourAngle =
      (cos(degreesToRadians(zenith)) -
       (sinDeclination * sin(degreesToRadians(latitude)))) /
      (cosDeclination * cos(degreesToRadians(latitude)));

  if (cosHourAngle > 1.0 || cosHourAngle < -1.0)
    return sunrise ? 6.0 : 18.0; // polar day/night fallback

  double localHourAngle = sunrise
                              ? 360.0 - radiansToDegrees(acos(cosHourAngle))
                              : radiansToDegrees(acos(cosHourAngle));
  localHourAngle /= 15.0;

  const double localMeanTime =
      localHourAngle + rightAscension - (0.06571 * approximateTime) - 6.622;

  double utcHour = fmod(localMeanTime - lngHour, 24.0);
  if (utcHour < 0.0)
    utcHour += 24.0;
  return utcHour;
}
