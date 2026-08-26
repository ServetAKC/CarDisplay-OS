/*******************************************************************
    Car Display OS - Spotify album art, clock and microphone
    visualizers on a 320 x 240 ESP32 "Cheap Yellow Display"
    (ESP32-2432S028R).

    This file is deliberately thin: it wires the modules together and
    runs the loop. The work lives in

      deviceConfig.*        persisted settings (credentials + region)
      WifiManagerHandler.*  connect / captive setup portal
      refreshToken.*        one-page Spotify authorisation helper
      spotifyLogic.*        polling, playback commands, queue prefetch
      cheapYellowLCD.*      the CYD screen, which in turn owns
        albumArtCache.*       cover download and caching
        backlightController.* brightness and sunset dimming
        visualizerRenderer.*  the sixteen microphone modes

    Hardware: https://github.com/witnessmenow/Spotify-Diy-Thing#hardware-required
 *******************************************************************/

// ----------------------------
// Display type
// ----------------------------
// 1. Cheap yellow display (TFT-eSPI)   -> YELLOW_DISPLAY
// 2. Matrix displays (ESP32 Trinity)   -> MATRIX_DISPLAY
#if !defined(YELLOW_DISPLAY) && !defined(MATRIX_DISPLAY)
#define YELLOW_DISPLAY
#endif

// NFC is disabled for the CYD car build. Re-enable only with a PN532 attached.
// #define NFC_ENABLED 1

#include <FS.h>
#include <SPIFFS.h>
#include <SpotifyArduino.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "deviceConfig.h"
#include "powerCycleDetector.h"
#include "refreshToken.h"
#include "spotifyDisplay.h"
#include "spotifyLogic.h"
#include "timing.h"
#include "version.h"
#include "WifiManagerHandler.h"

#if defined YELLOW_DISPLAY
#include "cheapYellowLCD.h"
CheapYellowDisplay cyd;
SpotifyDisplay *spotifyDisplay = &cyd;
#elif defined MATRIX_DISPLAY
#include "matrixDisplay.h"
MatrixDisplay matrixDisplay;
SpotifyDisplay *spotifyDisplay = &matrixDisplay;
#endif

#ifdef HONDATHING_FONT_INSTALLER
#include "fontInstaller.h"
#endif

#ifdef NFC_ENABLED
#include "nfc.h"
// Writing the context URI can misbehave when an album auto-plays into related
// songs, so it stays opt-in.
bool writeContextToNfc = true;
#endif

namespace
{
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
constexpr unsigned long WIFI_RETRY_WINDOW_MS = 5000;

unsigned long nextWiFiReconnectTime = 0;

void drawWifiManagerMessage(WiFiManager *myWiFiManager)
{
  // Stop the saved-network animation before WiFiManager switches to its portal.
  spotifyDisplay->stopWiFiConnectingAnimation();
  spotifyDisplay->drawWifiManagerMessage(myWiFiManager);
}

// Keeps the UI alive and retries the saved network without rebooting. A dropped
// phone hotspot must never send the unit into the setup portal or a reset loop.
void waitForWiFi(bool forceConfig)
{
  bool connected = setupWiFiManager(forceConfig, deviceConfig,
                                    &drawWifiManagerMessage, spotifyDisplay);

  while (!connected)
  {
    servicePowerCycleDetector();

    if (forceConfig)
    {
      Serial.println(F("Wi-Fi setup timed out; reopening setup portal"));
      connected = setupWiFiManager(true, deviceConfig,
                                   &drawWifiManagerMessage, spotifyDisplay);
      continue;
    }

    Serial.println(F("Waiting for saved Wi-Fi..."));
    WiFi.reconnect();
    const unsigned long retryDeadline = deadlineIn(WIFI_RETRY_WINDOW_MS);
    while (WiFi.status() != WL_CONNECTED && !timeReached(retryDeadline))
    {
      servicePowerCycleDetector();
      delay(20);
    }
    connected = WiFi.status() == WL_CONNECTED;
  }
}

// GPIO 0 (the CYD boot button) forces a fresh Spotify authorisation.
bool refreshTokenForced()
{
#if defined YELLOW_DISPLAY
  pinMode(0, INPUT); // has an internal pullup
  if (digitalRead(0) == LOW)
  {
    Serial.println(F("GPIO 0 is low, forcing a new refresh token"));
    return true;
  }
#endif
  return false;
}
} // namespace

void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println(F(CARDISPLAY_BUILD_TAG));

#ifdef HONDATHING_FONT_INSTALLER
  runHondaJapaneseFontInstaller();
  return;
#endif

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  if (!(SPIFFS.begin(false) || SPIFFS.begin(true)))
  {
    Serial.println(F("SPIFFS initialisation failed!"));
    while (true)
      delay(1000);
  }

  const bool quickPowerCycle = detectQuickPowerCycle();
  if (quickPowerCycle)
    Serial.println(F("Forcing config mode after a quick double power cycle"));

  const bool configAvailable = loadDeviceConfig(deviceConfig);
  const bool forceConfig = quickPowerCycle || !configAvailable;

  spotifyDisplay->displaySetup(&spotify);
  spotifyDisplay->startWiFiConnectingAnimation();

  waitForWiFi(forceConfig);

  spotifyDisplay->stopWiFiConnectingAnimation();
  clearPowerCycleMarker();
  Serial.print(F("IP address: "));
  Serial.println(WiFi.localIP());

#ifdef NFC_ENABLED
  Serial.println(nfcSetup(&spotify, spotifyDisplay) ? F("NFC good") : F("NFC bad"));
#endif

  spotifySetup(spotifyDisplay, deviceConfig);

  if (refreshTokenForced() || deviceConfig.refreshToken[0] == '\0')
  {
    spotifyDisplay->drawRefreshTokenMessage();
    Serial.println(F("Launching refresh token flow"));
    if (launchRefreshTokenFlow(&spotify, deviceConfig))
    {
      Serial.println(F("Spotify authorization completed"));
      saveDeviceConfig(deviceConfig);
    }
  }

  spotifyRefreshToken(deviceConfig.refreshToken);
  spotifyControlSetup(deviceConfig);
  spotifyQueueSetup(deviceConfig);

  spotifyDisplay->showDefaultScreen();
}

void loop()
{
  servicePowerCycleDetector();

  // Retry a dropped hotspot quietly in the background; the last screen stays
  // responsive throughout.
  if (WiFi.status() != WL_CONNECTED && timeReached(nextWiFiReconnectTime))
  {
    nextWiFiReconnectTime = deadlineIn(WIFI_RECONNECT_INTERVAL_MS);
    WiFi.reconnect();
  }

  spotifyDisplay->checkForInput();

  bool forceUpdate = false;

#ifdef NFC_ENABLED
  forceUpdate = writeContextToNfc ? nfcLoop(lastTrackUri, lastTrackContextUri)
                                  : nfcLoop(lastTrackUri);
#endif

  // Keep the visualizer render loop isolated from synchronous Spotify HTTPS
  // polling: those requests can block for seconds and were the real cause of
  // the random animation freezes. Polling resumes as soon as the overlay closes.
#if defined YELLOW_DISPLAY
  const bool visualizerActive = cyd.isVisualizerActive();
#else
  const bool visualizerActive = false;
#endif

  spotifyDisplay->service();

  if (!visualizerActive && WiFi.status() == WL_CONNECTED)
  {
    updateCurrentlyPlaying(forceUpdate);
    updateProgressBar();
  }

  // Give Wi-Fi and the worker tasks time without slowing the UI.
  delay(1);
}
