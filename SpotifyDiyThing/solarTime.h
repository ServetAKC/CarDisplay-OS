#pragma once

#include <time.h>

// ---------------------------------------------------------------------------
// Sunrise and sunset for the automatic night dim.
//
// NOAA-style approximation, accurate to a couple of minutes, which is far more
// than the backlight needs. Free of Arduino so the native tests can check it
// against published times for a known date and city - a wrong result here is
// otherwise invisible until the screen dims at the wrong hour.
// ---------------------------------------------------------------------------

// Returns the UTC decimal hour (0..24) of the event on the given UTC date.
// `sunrise` selects sunrise; false selects sunset.
//
// Inside the polar day/night bands the event does not occur; the function then
// returns 6.0 for sunrise and 18.0 for sunset so callers still get a usable,
// clearly-neutral answer.
double solarEventUtcHour(const struct tm &utcDate, double latitude,
                         double longitude, bool sunrise);

// True when `utcNow` falls between sunset and the following sunrise.
bool isNight(const struct tm &utcNow, double latitude, double longitude);
