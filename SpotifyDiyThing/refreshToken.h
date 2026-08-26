#pragma once

#include <SpotifyArduino.h>

#include "deviceConfig.h"

// Serves the one-page Spotify authorisation helper on port 80 and blocks until
// the browser redirect delivers a refresh token, which is written straight into
// `config.refreshToken`. Returns true when a token was captured.
bool launchRefreshTokenFlow(SpotifyArduino *spotifyObj, DeviceConfig &config);
