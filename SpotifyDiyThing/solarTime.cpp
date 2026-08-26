#include "solarTime.h"

#include <math.h>

namespace
{
constexpr double ZENITH = 90.833; // standard refraction + solar radius

double normalizeDegrees(double value)
{
  value = fmod(value, 360.0);
  return value < 0.0 ? value + 360.0 : value;
}

double toRadians(double value) { return value * 0.017453292519943295; }
double toDegrees(double value) { return value * 57.29577951308232; }
} // namespace

double solarEventUtcHour(const struct tm &utcDate, double latitude,
                         double longitude, bool sunrise)
{
  const int dayOfYear = utcDate.tm_yday + 1;
  const double lngHour = longitude / 15.0;
  const double approximateTime = dayOfYear + (((sunrise ? 6.0 : 18.0) - lngHour) / 24.0);

  const double meanAnomaly = (0.9856 * approximateTime) - 3.289;
  double trueLongitude =
      meanAnomaly +
      (1.916 * sin(toRadians(meanAnomaly))) +
      (0.020 * sin(2.0 * toRadians(meanAnomaly))) +
      282.634;
  trueLongitude = normalizeDegrees(trueLongitude);

  double rightAscension = toDegrees(atan(0.91764 * tan(toRadians(trueLongitude))));
  rightAscension = normalizeDegrees(rightAscension);

  // Put the right ascension in the same quadrant as the true longitude.
  const double longitudeQuadrant = floor(trueLongitude / 90.0) * 90.0;
  const double rightAscensionQuadrant = floor(rightAscension / 90.0) * 90.0;
  rightAscension += longitudeQuadrant - rightAscensionQuadrant;
  rightAscension /= 15.0;

  const double sinDeclination = 0.39782 * sin(toRadians(trueLongitude));
  const double cosDeclination = cos(asin(sinDeclination));

  const double cosHourAngle =
      (cos(toRadians(ZENITH)) - (sinDeclination * sin(toRadians(latitude)))) /
      (cosDeclination * cos(toRadians(latitude)));

  if (cosHourAngle > 1.0 || cosHourAngle < -1.0)
    return sunrise ? 6.0 : 18.0; // polar day or night

  double localHourAngle = sunrise ? 360.0 - toDegrees(acos(cosHourAngle))
                                  : toDegrees(acos(cosHourAngle));
  localHourAngle /= 15.0;

  const double localMeanTime =
      localHourAngle + rightAscension - (0.06571 * approximateTime) - 6.622;

  double utcHour = fmod(localMeanTime - lngHour, 24.0);
  if (utcHour < 0.0)
    utcHour += 24.0;
  return utcHour;
}

bool isNight(const struct tm &utcNow, double latitude, double longitude)
{
  const double currentUtcHour =
      utcNow.tm_hour + (utcNow.tm_min / 60.0) + (utcNow.tm_sec / 3600.0);
  const double sunriseUtc = solarEventUtcHour(utcNow, latitude, longitude, true);
  const double sunsetUtc = solarEventUtcHour(utcNow, latitude, longitude, false);

  return currentUtcHour >= sunsetUtc || currentUtcHour < sunriseUtc;
}
