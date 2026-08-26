/*******************************************************************
    Displays Album Art on a 320 x 240 ESP32.

    Parts:
    ESP32 With Built in 320x240 LCD with Touch Screen (ESP32-2432S028R)
    https://github.com/witnessmenow/Spotify-Diy-Thing#hardware-required

 *******************************************************************/

// ----------------------------
// Display type
// ---------------------------

// This project currently supports the following displays
// (Uncomment the required #define)

// 1. Cheap yellow display (Using TFT-eSPI library)
// #define YELLOW_DISPLAY

// 2. Matrix Displays (Like the ESP32 Trinity)
// #define MATRIX_DISPLAY

// If no defines are set, it will default to CYD
#if !defined(YELLOW_DISPLAY) && !defined(MATRIX_DISPLAY)
#define YELLOW_DISPLAY // Default to Yellow Display for display type
#endif

// NFC disabled for the CYD car display build. Re-enable only when a PN532 is connected.
// #define NFC_ENABLED 1

// This causes issues in certain circumstances e.g. Play an album and let it auto play to related songs
bool writeContextToNfc = true;
unsigned long nextWiFiReconnectTime = 0;

// ----------------------------
// Library Defines - Need to be defined before library import
// ----------------------------

// ----------------------------
// Standard Libraries
// ----------------------------
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <FS.h>
#include "SPIFFS.h"

// ----------------------------
// Additional Libraries - each one of these will need to be installed.
// ----------------------------

#include <WiFiManager.h>
// Captive portal for configuring the WiFi

// If installing from the library manager (Search for "WifiManager")
// https://github.com/tzapu/WiFiManager

// ArduinoJson is included first so the private/public shim below only affects
// SpotifyArduino itself. The shim exposes the library's generic response parser
// helpers for the optional queue-prefetch request.
#include <ArduinoJson.h>
#define private public
#include <SpotifyArduino.h>
#undef private

// including a "spotify_server_cert" variable
// header is included as part of the SpotifyArduino libary
#include <SpotifyArduinoCert.h>

WiFiClientSecure client;
WiFiClientSecure controlClient;
WiFiClientSecure queueClient;

//------- Replace the following! ------

// Country code, including this is advisable
#define SPOTIFY_MARKET "IE"
//------- ---------------------- ------

// ----------------------------
// Internal includes
// ----------------------------
#include "refreshToken.h"

#include "spotifyDisplay.h"

#include "spotifyLogic.h"

#include "configFile.h"

#include "serialPrint.h"

#include "powerCycleDetector.h"

#include "WifiManagerHandler.h"

// ----------------------------
// Display Handling Code
// ----------------------------

#if defined YELLOW_DISPLAY

#include "cheapYellowLCD.h"
CheapYellowDisplay cyd;
SpotifyDisplay *spotifyDisplay = &cyd;

#elif defined MATRIX_DISPLAY
#include "matrixDisplay.h"
MatrixDisplay matrixDisplay;
SpotifyDisplay *spotifyDisplay = &matrixDisplay;

#endif
// ----------------------------

#ifdef HONDATHING_FONT_INSTALLER
#include "fontInstaller.h"
#endif

#ifdef NFC_ENABLED
#include "nfc.h"
#endif

void drawWifiManagerMessage(WiFiManager *myWiFiManager)
{
  // Stop the saved-network animation before WiFiManager switches to its AP portal.
  spotifyDisplay->stopWiFiConnectingAnimation();
  spotifyDisplay->drawWifiManagerMessage(myWiFiManager);
}

void setup()
{
  Serial.begin(115200);

#ifdef HONDATHING_FONT_INSTALLER
  runHondaJapaneseFontInstaller();
  return;
#endif

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  bool spiffsInitSuccess = SPIFFS.begin(false) || SPIFFS.begin(true);
  if (!spiffsInitSuccess)
  {
    Serial.println("SPIFFS initialisation failed!");
    while (1)
      yield(); // Stay here twiddling thumbs waiting
  }
  Serial.println("\r\nInitialisation done.");

  const bool quickPowerCycle = detectQuickPowerCycle();
  if (quickPowerCycle)
  {
    Serial.println(F("Forcing config mode after a quick double power cycle"));
  }

  refreshToken[0] = '\0';
  const bool spotifyConfigAvailable = fetchConfigFile(refreshToken, clientId, clientSecret);
  const bool forceConfig = quickPowerCycle || !spotifyConfigAvailable;

  spotifyDisplay->displaySetup(&spotify);
  spotifyDisplay->startWiFiConnectingAnimation();

  bool wifiConnected = setupWiFiManager(forceConfig, refreshToken,
                                        &saveConfigFile, &drawWifiManagerMessage,
                                        spotifyDisplay);

  // A normal boot never opens the setup portal just because the phone hotspot
  // is late. Keep the UI alive and retry the saved network without rebooting.
  while (!wifiConnected)
  {
    servicePowerCycleDetector();
    if (forceConfig)
    {
      Serial.println("Wi-Fi setup timed out; reopening setup portal");
      wifiConnected = setupWiFiManager(true, refreshToken,
                                      &saveConfigFile, &drawWifiManagerMessage,
                                      spotifyDisplay);
    }
    else
    {
      Serial.println("Waiting for saved Wi-Fi...");
      WiFi.reconnect();
      const unsigned long retryDeadline = millis() + 5000UL;
      while (WiFi.status() != WL_CONNECTED && millis() < retryDeadline)
      {
        servicePowerCycleDetector();
        delay(20);
      }
      wifiConnected = WiFi.status() == WL_CONNECTED;
    }
  }

  spotifyDisplay->stopWiFiConnectingAnimation();
  clearPowerCycleMarker();
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

#ifdef NFC_ENABLED
  if (nfcSetup(&spotify, spotifyDisplay))
    Serial.println("NFC Good");
  else
    Serial.println("NFC Bad");
#endif

  spotifySetup(spotifyDisplay, clientId, clientSecret);

#if defined YELLOW_DISPLAY

  pinMode(0, INPUT); // has an internal pullup
  bool forceRefreshToken = digitalRead(0) == LOW;
  if (forceRefreshToken)
  {
    Serial.println("GPIO 0 is low, forcing refreshToken");
  }

#else
  bool forceRefreshToken = false;

#endif

  // Check if we have a refresh Token
  if (forceRefreshToken || refreshToken[0] == '\0')
  {

    spotifyDisplay->drawRefreshTokenMessage();
    Serial.println("Launching refresh token flow");
    if (launchRefreshTokenFlow(&spotify, clientId))
    {
      Serial.println("Spotify authorization completed");
      saveConfigFile(refreshToken, clientId, clientSecret);
    }
  }

  spotifyRefreshToken(refreshToken);
  spotifyControlSetup(clientId, clientSecret, refreshToken);
  spotifyQueueSetup(clientId, clientSecret, refreshToken);

  spotifyDisplay->showDefaultScreen();
}

void loop()
{
  servicePowerCycleDetector();

  // A dropped phone hotspot must not send the unit into setup or a reset loop.
  // Retry quietly in the background and keep the last screen responsive.
  if (WiFi.status() != WL_CONNECTED && millis() >= nextWiFiReconnectTime)
  {
    nextWiFiReconnectTime = millis() + 5000UL;
    WiFi.reconnect();
  }

  spotifyDisplay->checkForInput();

  bool forceUpdate = false;

#ifdef NFC_ENABLED
  if (writeContextToNfc)
  {
    forceUpdate = nfcLoop(lastTrackUri, lastTrackContextUri);
  }
  else
  {
    forceUpdate = nfcLoop(lastTrackUri);
  }

#endif

  // Keep the visualizer render loop isolated from synchronous Spotify HTTPS
  // polling. Those requests can block for seconds and were the real cause of the
  // random animation freezes. Polling and progress resume on exit.
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

  // Give Wi-Fi and the display worker tasks time without slowing the UI.
  delay(1);
}
