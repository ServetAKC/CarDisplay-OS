#include "spotifyLogic.h"

#include <ArduinoJson.h>
#include <SpotifyArduinoCert.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "timing.h"

namespace
{
WiFiClientSecure pollingClient;
WiFiClientSecure controlClient;
WiFiClientSecure queueClient;

SpotifyDisplay *display = nullptr;

// Copied out of DeviceConfig at setup time; SpotifyArduino keeps the pointers it
// is handed, so these must outlive every request.
char marketCode[sizeof(DeviceConfig::market)] = CARDISPLAY_DEFAULT_MARKET;

bool albumArtChanged = false;
bool spotifyQueueReady = false;

unsigned long songStartMillis = 0;
long songDuration = 0;
bool songProgressValid = false;

constexpr unsigned long DELAY_BETWEEN_REQUESTS_MS = 5000;
constexpr unsigned long DELAY_BETWEEN_PROGRESS_UPDATES_MS = 500;
constexpr unsigned long RAPID_REFRESH_INTERVAL_MS = 350;

unsigned long requestDueTime = 0;
unsigned long progressDueTime = 0;
volatile uint8_t rapidRefreshesRemaining = 0;

bool isSameTrack(const char *trackUri)
{
  return strcmp(lastTrackUri, trackUri) == 0;
}
} // namespace

SpotifyArduino spotify(pollingClient, NULL, NULL);
SpotifyArduino spotifyControl(controlClient, NULL, NULL);
SpotifyArduino spotifyQueue(queueClient, NULL, NULL);

char lastTrackUri[200];
char lastTrackContextUri[200];

volatile bool spotifyControlCommandPending = false;
bool spotifyControlReady = false;

void scheduleNextPoll(unsigned long delayMs)
{
  requestDueTime = deadlineIn(delayMs);
}

void requestRapidRefresh(uint8_t extraPolls, unsigned long firstDelayMs)
{
  rapidRefreshesRemaining = extraPolls;
  scheduleNextPoll(firstDelayMs);
}

void spotifySetup(SpotifyDisplay *theDisplay, const DeviceConfig &config)
{
  display = theDisplay;
  copyField(marketCode, config.market);

  pollingClient.setCACert(spotify_server_cert);
  pollingClient.setTimeout(2000);
  spotify.lateInit(config.clientId, config.clientSecret);

  lastTrackUri[0] = '\0';
  lastTrackContextUri[0] = '\0';
}

void spotifyControlSetup(const DeviceConfig &config)
{
  controlClient.setCACert(spotify_server_cert);
  controlClient.setTimeout(1500);
  spotifyControl.lateInit(config.clientId, config.clientSecret);
  spotifyControl.setRefreshToken(config.refreshToken);

  Serial.println(F("Refreshing fast-control access token"));
  spotifyControlReady = spotifyControl.refreshAccessToken();
  if (!spotifyControlReady)
    Serial.println(F("Fast-control token refresh failed; controls will retry automatically"));
}

void spotifyQueueSetup(const DeviceConfig &config)
{
  queueClient.setCACert(spotify_server_cert);
  queueClient.setTimeout(2500);
  spotifyQueue.lateInit(config.clientId, config.clientSecret);
  spotifyQueue.setRefreshToken(config.refreshToken);

  Serial.println(F("Refreshing queue-prefetch access token"));
  spotifyQueueReady = spotifyQueue.refreshAccessToken();
  if (!spotifyQueueReady)
    Serial.println(F("Queue-prefetch token refresh failed; it will retry later"));
}

void spotifyReleaseQueueConnection()
{
  queueClient.stop();
}

void spotifyRefreshToken(const char *refreshToken)
{
  spotify.setRefreshToken(refreshToken);

  Serial.println(F("Refreshing access tokens"));
  if (!spotify.refreshAccessToken())
    Serial.println(F("Failed to get access tokens"));
}

size_t spotifyGetQueuedAlbumArtUrls(char out[][SPOTIFY_QUEUE_URL_SIZE], size_t maxUrls)
{
  if (out == nullptr || maxUrls == 0 || WiFi.status() != WL_CONNECTED)
    return 0;

  for (size_t i = 0; i < maxUrls; i++)
    out[i][0] = '\0';

  if (!spotifyQueueReady)
    spotifyQueueReady = spotifyQueue.refreshAccessToken();

  if (!spotifyQueueReady || !spotifyQueue.checkAndRefreshAccessToken())
    return 0;

  static const char *QUEUE_ENDPOINT = "/v1/me/player/queue";
  const int statusCode = spotifyQueue.makeGetRequest(QUEUE_ENDPOINT, spotifyQueue.bearerToken());

  if (statusCode > 0)
    spotifyQueue.consumeResponseHeaders();

  size_t count = 0;
  if (statusCode == 200)
  {
    StaticJsonDocument<256> filter;
    // Array item zero acts as the filter template for all queue entries.
    filter["queue"][0]["album"]["images"][0]["url"] = true;
    filter["queue"][0]["images"][0]["url"] = true;

    // Temporary heap allocation; released as soon as the queue request ends.
    DynamicJsonDocument doc(12288);
    const DeserializationError error = deserializeJson(
        doc, *spotifyQueue.client, DeserializationOption::Filter(filter));

    if (!error)
    {
      JsonArray queue = doc["queue"].as<JsonArray>();
      for (JsonVariant item : queue)
      {
        const char *url = item["album"]["images"][0]["url"];
        if (url == nullptr || url[0] == '\0')
          url = item["images"][0]["url"]; // podcast episode
        if (url == nullptr || url[0] == '\0')
          continue;

        bool duplicate = false;
        for (size_t existing = 0; existing < count; existing++)
        {
          if (strcmp(out[existing], url) == 0)
          {
            duplicate = true;
            break;
          }
        }
        if (duplicate)
          continue;

        copyField(out[count], url);
        count++;
        if (count >= maxUrls)
          break;
      }
    }
    else
    {
      Serial.print(F("Queue JSON parse failed: "));
      Serial.println(error.c_str());
    }
  }
  else
  {
    Serial.print(F("Queue prefetch HTTP status: "));
    Serial.println(statusCode);
  }

  spotifyQueue.finishRequest();
  return count;
}

static void handleCurrentlyPlaying(CurrentlyPlaying currentlyPlaying)
{
  if (currentlyPlaying.trackUri == NULL)
    return;

  display->setPlaybackState(currentlyPlaying.isPlaying);
  if (!isSameTrack(currentlyPlaying.trackUri))
  {
    // Bounded copies: both URIs come straight off the network.
    copyField(lastTrackUri, currentlyPlaying.trackUri);
    copyField(lastTrackContextUri, currentlyPlaying.contextUri);
    display->printCurrentlyPlayingToScreen(currentlyPlaying);
  }

  albumArtChanged = display->processImageInfo(currentlyPlaying);
  display->displayTrackProgress(currentlyPlaying.progressMs, currentlyPlaying.durationMs);

  if (currentlyPlaying.isPlaying)
  {
    songStartMillis = millis() - currentlyPlaying.progressMs;
    songDuration = currentlyPlaying.durationMs;
    songProgressValid = true;
  }
  else
  {
    songProgressValid = false;
  }
}

void updateProgressBar()
{
  if (!songProgressValid || !timeReached(progressDueTime))
    return;

  long songProgress = static_cast<long>(millis() - songStartMillis);
  if (songProgress > songDuration)
    songProgress = songDuration;

  display->displayTrackProgress(songProgress, songDuration);
  progressDueTime = deadlineIn(DELAY_BETWEEN_PROGRESS_UPDATES_MS);
}

void updateCurrentlyPlaying(bool forceUpdate)
{
  // Never begin a comparatively heavy now-playing request while a touch command
  // is queued or being sent by the dedicated control task.
  if (spotifyControlCommandPending)
    return;

  if (!forceUpdate && !timeReached(requestDueTime))
    return;

  if (forceUpdate)
    Serial.println(F("Forcing an update"));

  const int status = spotify.getCurrentlyPlaying(handleCurrentlyPlaying, marketCode);
  if (status == 200)
  {
    if (albumArtChanged)
    {
      display->clearImage();
      if (display->displayImage())
        albumArtChanged = false;
      else
        Serial.println(F("Failed to display image"));
    }
  }
  else if (status == 204)
  {
    songProgressValid = false;
    Serial.println(F("Nothing appears to be playing"));
  }
  else
  {
    Serial.print(F("Now-playing error: "));
    Serial.println(status);
  }

  if (rapidRefreshesRemaining > 0)
  {
    rapidRefreshesRemaining--;
    scheduleNextPoll(RAPID_REFRESH_INTERVAL_MS);
  }
  else
  {
    scheduleNextPoll(DELAY_BETWEEN_REQUESTS_MS);
  }
}
