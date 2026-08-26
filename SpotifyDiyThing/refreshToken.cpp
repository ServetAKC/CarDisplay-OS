#include "refreshToken.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

namespace
{
SpotifyArduino *spotifyRefresh = nullptr;
DeviceConfig *targetConfig = nullptr;
bool haveRefreshToken = false;

char callbackURI[100];
constexpr const char *CALLBACK_PROTOCOL = "https%3A%2F%2F"; // "https://"
constexpr const char *CALLBACK_PATH = "%2Fcallback%2F";     // "/callback/"
constexpr const char *SCOPE = "user-read-playback-state%20user-modify-playback-state";

WebServer server(80);

const char *PAGE_TEMPLATE =
    R"(<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta http-equiv="X-UA-Compatible" content="IE=edge">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no" />
  </head>
  <body onload='decode()'>
    <div>
      Click here to auth the device: <a href="https://accounts.spotify.com/authorize?client_id=%s&response_type=code&redirect_uri=%s&scope=%s">spotify Auth</a>
    </div>
    </br>
    <div>
      Make sure to add <span style="font-weight:bold;" id="uri"> %s </span> to the "redirect URIs" in the <a href="https://developer.spotify.com/dashboard/applications">Spotify Developers Dashboard</a>
    </div>
  </body>
  <script>
    function decode() {
        document.getElementById('uri').innerText = decodeURIComponent(document.getElementById('uri').innerText);
    }
</script>
</html>
)";

void handleRoot()
{
  // snprintf, not sprintf: the client ID and callback URI are variable length
  // and a long pair used to be able to run past the end of this buffer.
  static char webpage[1400];
  snprintf(webpage, sizeof(webpage), PAGE_TEMPLATE,
           targetConfig != nullptr ? targetConfig->clientId : "",
           callbackURI, SCOPE, callbackURI);
  server.send(200, "text/html", webpage);
}

void handleCallback()
{
  const char *token = nullptr;
  for (uint8_t i = 0; i < server.args(); i++)
  {
    if (server.argName(i) == "code")
      token = spotifyRefresh->requestAccessTokens(server.arg(i).c_str(), callbackURI);
  }

  if (token != nullptr && token[0] != '\0' && targetConfig != nullptr)
  {
    copyField(targetConfig->refreshToken, token);
    haveRefreshToken = true;
    server.send(200, "text/plain", "Got Token, your device should be ready");
  }
  else
  {
    server.send(404, "text/plain", "Failed to load token, check serial monitor");
  }
}

void handleNotFound()
{
  String message = "File Not Found\n\nURI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++)
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";

  Serial.print(message);
  server.send(404, "text/plain", message);
}
} // namespace

bool launchRefreshTokenFlow(SpotifyArduino *spotifyObj, DeviceConfig &config)
{
  spotifyRefresh = spotifyObj;
  targetConfig = &config;
  haveRefreshToken = false;
  config.refreshToken[0] = '\0';

  snprintf(callbackURI, sizeof(callbackURI), "%s%s%s",
           CALLBACK_PROTOCOL, WiFi.localIP().toString().c_str(), CALLBACK_PATH);

  server.on("/", handleRoot);
  server.on("/callback/", handleCallback);
  server.onNotFound(handleNotFound);
  server.begin();

  delay(100);

  while (!haveRefreshToken)
  {
    server.handleClient();
    delay(1); // Feed the watchdog and let Wi-Fi run while we wait for the browser.
  }

  server.stop();
  return true;
}
