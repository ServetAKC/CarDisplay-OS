
#define WM_CLIENT_ID_LABEL "clientID"
#define WM_CLIENT_SECRET_LABEL "clientSecret"
#define WM_REFRESH_TOKEN_LABEL "refreshToken"

char clientId[50];
char clientSecret[50];

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

bool setupWiFiManager(bool forceConfig, char *refreshToken, void (*saveConfig)(char *, char *, char *), void (*configModeCallback)(WiFiManager *myWiFiManager), SpotifyDisplay *display){
  WiFiManager wm;
  wm.setConnectTimeout(30);
  // The setup portal stays alive while the offline visualizer is in use.
  wm.setConfigPortalTimeout(0);
  //set config save notify callback
  wm.setSaveConfigCallback(saveConfigCallback);
  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wm.setAPCallback(configModeCallback);  

  WiFiManagerParameter clientIdParam(WM_CLIENT_ID_LABEL, "Client ID", clientId, 40);
  WiFiManagerParameter clientSecretParam(WM_CLIENT_SECRET_LABEL, "Client Secret", clientSecret, 40);
  WiFiManagerParameter clientRefreshToken(WM_REFRESH_TOKEN_LABEL, "Refresh Token (optional)", refreshToken, 399);

  wm.addParameter(&clientIdParam);
  wm.addParameter(&clientSecretParam);
  wm.addParameter(&clientRefreshToken);

  if (forceConfig) {
    // IF we forced config this time, lets stop the double reset so it doesn't get stuck in a loop
    clearPowerCycleMarker();
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("SpotifyDIY", "thing123");

    // Keep DNS/web setup, touch input and the microphone visualizer alive in
    // one cooperative loop. Saving valid Wi-Fi credentials exits this loop.
    while (WiFi.status() != WL_CONNECTED) {
      const bool connected = wm.process();
      if (display != nullptr) {
        display->serviceConfigPortal();
      }
      servicePowerCycleDetector();
      if (connected || WiFi.status() == WL_CONNECTED) {
        break;
      }
      delay(1);
    }
    wm.stopConfigPortal();
    if (display != nullptr) {
      display->finishConfigPortal();
    }
  } else {
    // Normal boot never opens a captive portal. It only tries the saved network;
    // the main loop keeps retrying if the phone hotspot is not ready yet.
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    const unsigned long connectDeadline = millis() + 5000UL;
    while (WiFi.status() != WL_CONNECTED && millis() < connectDeadline)
      delay(100);
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Saved Wi-Fi unavailable; will retry without restarting");
      return false;
    }
  }

  //save the custom parameters to FS
  if (shouldSaveConfig)
  {

    strncpy(clientId, clientIdParam.getValue(), 40);
    strncpy(clientSecret, clientSecretParam.getValue(), 40);
    strncpy(refreshToken, clientRefreshToken.getValue(), 399);

    saveConfig(refreshToken, clientId, clientSecret);
    clearPowerCycleMarker();
    ESP.restart();
    delay(5000);
  }
  return WiFi.status() == WL_CONNECTED;
}
