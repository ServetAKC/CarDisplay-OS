#pragma once

#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Runtime device configuration.
//
// Market, timezone and the sunset-dimming coordinates used to be compile-time
// #defines pinned to Ireland/Sofia. They are user-facing settings: a wrong
// market makes Spotify report tracks as unavailable, and wrong coordinates
// make the automatic night dim fire at the wrong hour. All four are now
// editable from the Wi-Fi setup portal and persisted alongside the Spotify
// credentials.
//
// The CARDISPLAY_DEFAULT_* macros only supply the first-boot values; once a
// config file exists the stored values win.
// ---------------------------------------------------------------------------

#ifndef CARDISPLAY_DEFAULT_MARKET
#define CARDISPLAY_DEFAULT_MARKET "TR"
#endif

#ifndef CARDISPLAY_DEFAULT_TZ
// Turkey observes UTC+3 all year with no daylight saving.
#define CARDISPLAY_DEFAULT_TZ "<+03>-3"
#endif

#ifndef CARDISPLAY_DEFAULT_LATITUDE
#define CARDISPLAY_DEFAULT_LATITUDE 41.0082f // Istanbul
#endif

#ifndef CARDISPLAY_DEFAULT_LONGITUDE
#define CARDISPLAY_DEFAULT_LONGITUDE 28.9784f
#endif

struct DeviceConfig
{
  char refreshToken[400];
  char clientId[64];
  char clientSecret[64];
  char market[4];    // ISO 3166-1 alpha-2, e.g. "TR"
  char timezone[48]; // POSIX TZ string, e.g. "<+03>-3"
  float latitude;
  float longitude;

  void setDefaults();
  bool hasSpotifyCredentials() const;
};

extern DeviceConfig deviceConfig;

bool loadDeviceConfig(DeviceConfig &config);
bool saveDeviceConfig(const DeviceConfig &config);

// ---------------------------------------------------------------------------
// Bounded string copy.
//
// Replaces the strcpy()/strncpy(dst, src, <hard-coded length>) calls that were
// spread through the firmware. Taking the destination by array reference means
// the size can never drift away from the buffer it belongs to, and the result
// is always null-terminated (plain strncpy is not, when src fills the buffer).
// ---------------------------------------------------------------------------
template <size_t N>
inline void copyField(char (&destination)[N], const char *source)
{
  if (source == nullptr)
  {
    destination[0] = '\0';
    return;
  }
  strncpy(destination, source, N - 1);
  destination[N - 1] = '\0';
}
