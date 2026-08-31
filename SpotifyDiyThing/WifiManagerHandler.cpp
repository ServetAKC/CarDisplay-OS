#include "WifiManagerHandler.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdlib.h>

#include "powerCycleDetector.h"
#include "timing.h"

namespace
{
constexpr const char *WM_AP_SSID = "SpotifyDIY";
constexpr const char *WM_AP_PASSWORD = "thing123";

constexpr unsigned long SAVED_NETWORK_TIMEOUT_MS = 5000UL;

// How often to re-try the saved network while the setup portal is open.
//
// v0.3.17 treated the portal as a dead end: once it was open the only way back
// to the player was to type credentials in or power cycle. In a car the phone
// hotspot usually appears a few seconds AFTER the unit powers up, so the portal
// has to keep watching for it. 20 s is long enough not to disturb the AP and
// short enough that the hand-off feels automatic.
constexpr unsigned long PORTAL_SAVED_RETRY_MS = 20000UL;

bool shouldSaveConfig = false;

void saveConfigCallback()
{
  Serial.println(F("Should save config"));
  shouldSaveConfig = true;
}

// WiFiManagerParameter hands back raw form input. Parse defensively: an empty or
// non-numeric field keeps the value that was already configured rather than
// silently snapping the coordinates to 0,0 (the Gulf of Guinea), which would put
// the automatic sunset dim hours out.
float parseCoordinate(const char *text, float fallback, float limit)
{
  if (text == nullptr || text[0] == '\0')
    return fallback;

  char *end = nullptr;
  const float value = strtof(text, &end);
  if (end == text || value < -limit || value > limit)
    return fallback;
  return value;
}
} // namespace

bool setupWiFiManager(bool forceConfig, DeviceConfig &config,
                      void (*configModeCallback)(WiFiManager *myWiFiManager),
                      SpotifyDisplay *display)
{
  shouldSaveConfig = false;

  WiFiManager wm;
  wm.setConnectTimeout(30);
  // The setup portal stays alive while the offline visualizer is in use.
  wm.setConfigPortalTimeout(0);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);

  char latitudeText[16];
  char longitudeText[16];
  snprintf(latitudeText, sizeof(latitudeText), "%.4f", config.latitude);
  snprintf(longitudeText, sizeof(longitudeText), "%.4f", config.longitude);

  WiFiManagerParameter clientIdParam("clientID", "Spotify Client ID",
                                     config.clientId, sizeof(config.clientId) - 1);
  WiFiManagerParameter clientSecretParam("clientSecret", "Spotify Client Secret",
                                         config.clientSecret, sizeof(config.clientSecret) - 1);
  WiFiManagerParameter refreshTokenParam("refreshToken", "Refresh Token (optional)",
                                         config.refreshToken, sizeof(config.refreshToken) - 1);
  WiFiManagerParameter marketParam("market", "Spotify market (e.g. TR)",
                                   config.market, sizeof(config.market) - 1);
  WiFiManagerParameter timezoneParam("timezone", "POSIX timezone (e.g. <+03>-3)",
                                     config.timezone, sizeof(config.timezone) - 1);
  WiFiManagerParameter latitudeParam("latitude", "Latitude (sunset dimming)",
                                     latitudeText, sizeof(latitudeText) - 1);
  WiFiManagerParameter longitudeParam("longitude", "Longitude (sunset dimming)",
                                      longitudeText, sizeof(longitudeText) - 1);

  wm.addParameter(&clientIdParam);
  wm.addParameter(&clientSecretParam);
  wm.addParameter(&refreshTokenParam);
  wm.addParameter(&marketParam);
  wm.addParameter(&timezoneParam);
  wm.addParameter(&latitudeParam);
  wm.addParameter(&longitudeParam);

  if (forceConfig)
  {
    // Stop the double-power-cycle marker so forcing config cannot loop.
    clearPowerCycleMarker();

    // Whether there is anything to fall back to. On a first-ever boot there is
    // no saved network, so the portal is the only destination and re-trying
    // would just churn the radio.
    const bool hasSavedNetwork = wm.getWiFiIsSaved();

    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal(WM_AP_SSID, WM_AP_PASSWORD);

    // Keep DNS/web setup, touch input and the microphone visualizer alive in
    // one cooperative loop. Saving valid Wi-Fi credentials exits this loop - and
    // so does the saved network turning up on its own.
    //
    // The portal is also torn down entirely while the offline visualizer is
    // open. "Offline" was never true before: the AP kept broadcasting, the DNS
    // responder kept answering and wm.process() kept running, all of it on core
    // 0 next to the microphone task. Honouring wantsRadioSilence() is what makes
    // the mode mean what its name says.
    bool portalRunning = true;
    unsigned long nextSavedRetry = deadlineIn(PORTAL_SAVED_RETRY_MS);
    while (WiFi.status() != WL_CONNECTED)
    {
      const bool wantsSilence = display != nullptr && display->wantsRadioSilence();

      if (wantsSilence && portalRunning)
      {
        Serial.println(F("Offline visualizer open; shutting the radio down"));
        wm.stopConfigPortal();
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, false); // keep the stored credentials
        WiFi.mode(WIFI_OFF);
        portalRunning = false;
      }
      else if (!wantsSilence && !portalRunning)
      {
        Serial.println(F("Offline visualizer closed; radio back on"));
        WiFi.mode(WIFI_AP_STA);
        wm.setConfigPortalBlocking(false);
        wm.startConfigPortal(WM_AP_SSID, WM_AP_PASSWORD);
        portalRunning = true;
        nextSavedRetry = deadlineIn(PORTAL_SAVED_RETRY_MS);
      }

      if (portalRunning)
      {
        const bool connected = wm.process();
        if (connected || WiFi.status() == WL_CONNECTED)
          break;

        if (hasSavedNetwork && timeReached(nextSavedRetry))
        {
          nextSavedRetry = deadlineIn(PORTAL_SAVED_RETRY_MS);
          Serial.println(F("Portal open; re-trying the saved network"));
          WiFi.begin();
        }
      }

      if (display != nullptr)
        display->serviceConfigPortal();
      servicePowerCycleDetector();
      delay(1);
    }

    // Bringing the radio back is the caller's problem only if it never came
    // back on its own; stopConfigPortal() is safe to call when nothing is
    // running.
    if (!portalRunning)
      WiFi.mode(WIFI_AP_STA);
    wm.stopConfigPortal();
    if (display != nullptr)
      display->finishConfigPortal();
  }
  else
  {
    // Normal boot never opens a captive portal on its own. It only tries the
    // saved network; the caller escalates to the portal if that keeps failing.
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    const unsigned long connectDeadline = deadlineIn(SAVED_NETWORK_TIMEOUT_MS);
    while (WiFi.status() != WL_CONNECTED && !timeReached(connectDeadline))
      delay(100);
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println(F("Saved Wi-Fi unavailable; will retry without restarting"));
      return false;
    }
  }

  if (shouldSaveConfig)
  {
    copyField(config.clientId, clientIdParam.getValue());
    copyField(config.clientSecret, clientSecretParam.getValue());
    copyField(config.refreshToken, refreshTokenParam.getValue());
    copyField(config.market, marketParam.getValue());
    copyField(config.timezone, timezoneParam.getValue());
    config.latitude = parseCoordinate(latitudeParam.getValue(), config.latitude, 90.0f);
    config.longitude = parseCoordinate(longitudeParam.getValue(), config.longitude, 180.0f);

    if (config.market[0] == '\0')
      copyField(config.market, CARDISPLAY_DEFAULT_MARKET);
    if (config.timezone[0] == '\0')
      copyField(config.timezone, CARDISPLAY_DEFAULT_TZ);

    saveDeviceConfig(config);
    clearPowerCycleMarker();
    ESP.restart();
    delay(5000);
  }

  return WiFi.status() == WL_CONNECTED;
}
