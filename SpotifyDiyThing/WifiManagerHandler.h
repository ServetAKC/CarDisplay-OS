#pragma once

#include <WiFiManager.h>

#include "deviceConfig.h"
#include "spotifyDisplay.h"

// Connects to the saved network, or opens the captive setup portal when
// `forceConfig` is set. The portal exposes the Spotify credentials plus the
// regional settings (market, timezone, coordinates), all of which are written
// straight into `config` and persisted before the device restarts.
//
// Returns true once Wi-Fi is connected.
bool setupWiFiManager(bool forceConfig, DeviceConfig &config,
                      void (*configModeCallback)(WiFiManager *myWiFiManager),
                      SpotifyDisplay *display);
