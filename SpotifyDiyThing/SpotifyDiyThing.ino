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
        visualizerRenderer.*  the forty microphone modes
      visualizerModes.*     the twenty added in v0.4

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
#include "touchScreen.h"
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

// How long a normal boot keeps trying the saved network before it gives up and
// opens the setup portal on its own. Measured from the moment waitForWiFi()
// starts, so this is the real power-on-to-setup-screen time: three 5 s attempts
// and then the portal, which also carries the OFFLINE VISUALIZER button. A
// touch anywhere on the connecting screen cuts it short.
constexpr unsigned long OFFLINE_FALLBACK_MS = 15000;

unsigned long nextWiFiReconnectTime = 0;

void drawWifiManagerMessage(WiFiManager *myWiFiManager)
{
  // Stop the saved-network animation before WiFiManager switches to its portal.
  spotifyDisplay->stopWiFiConnectingAnimation();
  spotifyDisplay->drawWifiManagerMessage(myWiFiManager);
}

// Keeps the UI alive and retries the saved network without rebooting.
//
// v0.3.17 looped here forever when no saved network was reachable: the screen
// stayed on the connecting animation, touch was never sampled, and the only way
// to reach either the setup page or the offline visualizer was to power cycle
// the unit twice. That is the single worst thing this firmware did in a car.
//
// v0.4 gives the saved network a bounded window and then opens the setup portal
// by itself. The portal screen is also the screen that offers OFFLINE
// VISUALIZER, and setupWiFiManager() keeps re-trying the saved network
// underneath it, so a hotspot that appears late is still picked up with no
// reboot and no second power cycle.
void waitForWiFi(bool forceConfig)
{
  // The deadline is taken before the first attempt, not after it, so
  // OFFLINE_FALLBACK_MS is the real time from power-on to the setup screen. The
  // obvious version - start counting after the initial 5 s attempt and check
  // the deadline at the top of each 5 s round - overshoots by a whole round and
  // takes about twenty seconds. In a car that is the difference between waiting
  // and deciding the thing is broken.
  const unsigned long portalDeadline = deadlineIn(OFFLINE_FALLBACK_MS);

  // Any touch during the wait means stop waiting. The touch task is already
  // running - displaySetup() starts it before this is ever called - so the tap
  // costs nothing to support and removes the whole wait for anyone who already
  // knows the hotspot is off.
  setTouchConnectingMode(true);
  bool skipRequested = false;

  bool connected = setupWiFiManager(forceConfig, deviceConfig,
                                    &drawWifiManagerMessage, spotifyDisplay);
  if (connected)
  {
    setTouchConnectingMode(false);
    return;
  }

  // Only a normal boot gets the quiet retry window; a forced config request
  // means the user asked for the portal and should not have to wait for it.
  if (!forceConfig)
  {
    while (!skipRequested && !timeReached(portalDeadline))
    {
      servicePowerCycleDetector();

      Serial.println(F("Waiting for saved Wi-Fi..."));
      WiFi.reconnect();
      const unsigned long retryDeadline = deadlineIn(WIFI_RETRY_WINDOW_MS);
      while (WiFi.status() != WL_CONNECTED && !timeReached(retryDeadline))
      {
        servicePowerCycleDetector();
        if (takeTouchAction() != TouchAction::None)
        {
          skipRequested = true;
          break;
        }
        delay(20);
      }

      if (WiFi.status() == WL_CONNECTED)
      {
        setTouchConnectingMode(false);
        return;
      }
    }
    Serial.println(skipRequested
                       ? F("Wait skipped by touch; opening setup")
                       : F("No saved network after the retry window; opening setup"));
  }

  // From here the portal is the destination, and it owns the touch map.
  setTouchConnectingMode(false);

  // setupWiFiManager() only returns once something connected, but guard the
  // loop anyway so a portal that is dismissed without credentials reopens
  // instead of falling through to Spotify setup with no network.
  while (!connected)
  {
    connected = setupWiFiManager(true, deviceConfig,
                                 &drawWifiManagerMessage, spotifyDisplay);
    if (!connected)
      Serial.println(F("Wi-Fi setup ended without a connection; reopening"));
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
  // A forced config request opens the portal immediately and has no countdown.
  spotifyDisplay->startWiFiConnectingAnimation(forceConfig ? 0UL : OFFLINE_FALLBACK_MS);

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
