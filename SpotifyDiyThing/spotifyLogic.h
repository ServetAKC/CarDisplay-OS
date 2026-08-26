SpotifyDisplay *sp_Display;

// The normal client handles now-playing polling. A second client is dedicated to
// playback commands so a slow GET/currently-playing request cannot hold up a tap.
SpotifyArduino spotify(client, NULL, NULL);
SpotifyArduino spotifyControl(controlClient, NULL, NULL);
// Dedicated client for GET /me/player/queue. Keeping it separate means next-cover
// prefetch can never block the normal now-playing or playback-control clients.
SpotifyArduino spotifyQueue(queueClient, NULL, NULL);

bool albumArtChanged = false;

long songStartMillis;
long songDuration;

char lastTrackUri[200];
char lastTrackContextUri[200];

// Shared with the CYD display task. While true, normal polling pauses so playback
// commands get network priority.
volatile bool spotifyControlCommandPending = false;
volatile uint8_t rapidRefreshesRemaining = 0;
bool spotifyControlReady = false;
bool spotifyQueueReady = false;

unsigned long delayBetweenRequests = 5000; // Normal polling interval.
unsigned long requestDueTime;

unsigned long delayBetweenProgressUpdates = 500;
unsigned long progressDueTime;

void spotifySetup(SpotifyDisplay *theDisplay, const char *clientId, const char *clientSecret)
{
  sp_Display = theDisplay;
  client.setCACert(spotify_server_cert);
  client.setTimeout(2000);
  spotify.lateInit(clientId, clientSecret);

  lastTrackUri[0] = '\0';
  lastTrackContextUri[0] = '\0';
}

void spotifyControlSetup(const char *clientId, const char *clientSecret, const char *refreshToken)
{
  controlClient.setCACert(spotify_server_cert);
  controlClient.setTimeout(1500);
  spotifyControl.lateInit(clientId, clientSecret);
  spotifyControl.setRefreshToken(refreshToken);

  Serial.println("Refreshing fast-control access token");
  spotifyControlReady = spotifyControl.refreshAccessToken();
  if (!spotifyControlReady)
  {
    Serial.println("Fast-control token refresh failed; controls will retry automatically");
  }
}

bool isSameTrack(const char *trackUri)
{
  return strcmp(lastTrackUri, trackUri) == 0;
}

void setTrackUri(const char *trackUri)
{
  strcpy(lastTrackUri, trackUri);
}

void setTrackContextUri(const char *trackContext)
{
  if (trackContext == NULL)
  {
    lastTrackContextUri[0] = '\0';
  }
  else
  {
    strcpy(lastTrackContextUri, trackContext);
  }
}


void spotifyQueueSetup(const char *clientId, const char *clientSecret, const char *refreshToken)
{
  queueClient.setCACert(spotify_server_cert);
  queueClient.setTimeout(2500);
  spotifyQueue.lateInit(clientId, clientSecret);
  spotifyQueue.setRefreshToken(refreshToken);

  Serial.println("Refreshing queue-prefetch access token");
  spotifyQueueReady = spotifyQueue.refreshAccessToken();
  if (!spotifyQueueReady)
  {
    Serial.println("Queue-prefetch token refresh failed; it will retry later");
  }
}

// Read several queued cover URLs in one API request. The player keeps a rolling
// cache window rather than downloading the whole queue forever: after every
// track change, cached entries are skipped and the window is filled further.
static constexpr size_t SPOTIFY_QUEUE_URL_SIZE = 256;

size_t spotifyGetQueuedAlbumArtUrls(
    char out[][SPOTIFY_QUEUE_URL_SIZE], size_t maxUrls)
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
  int statusCode = spotifyQueue.makeGetRequest(QUEUE_ENDPOINT, spotifyQueue._bearerToken);

  if (statusCode > 0)
    spotifyQueue.skipHeaders();

  size_t count = 0;
  if (statusCode == 200)
  {
    StaticJsonDocument<256> filter;
    // Array item zero acts as the filter template for all queue entries.
    filter["queue"][0]["album"]["images"][0]["url"] = true;
    filter["queue"][0]["images"][0]["url"] = true;

    // Temporary heap allocation; released as soon as the queue request ends.
    DynamicJsonDocument doc(12288);
    DeserializationError error = deserializeJson(
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

        strncpy(out[count], url, SPOTIFY_QUEUE_URL_SIZE - 1);
        out[count][SPOTIFY_QUEUE_URL_SIZE - 1] = '\0';
        count++;
        if (count >= maxUrls)
          break;
      }
    }
    else
    {
      Serial.print("Queue JSON parse failed: ");
      Serial.println(error.c_str());
    }
  }
  else
  {
    Serial.print("Queue prefetch HTTP status: ");
    Serial.println(statusCode);
  }

  spotifyQueue.closeClient();
  return count;
}

void spotifyRefreshToken(const char *refreshToken)
{
  spotify.setRefreshToken(refreshToken);

  Serial.println("Refreshing Access Tokens");
  if (!spotify.refreshAccessToken())
  {
    Serial.println("Failed to get access tokens");
  }
}

void handleCurrentlyPlaying(CurrentlyPlaying currentlyPlaying)
{
  if (currentlyPlaying.trackUri != NULL)
  {
    sp_Display->setPlaybackState(currentlyPlaying.isPlaying);
    if (!isSameTrack(currentlyPlaying.trackUri))
    {
      setTrackUri(currentlyPlaying.trackUri);
      setTrackContextUri(currentlyPlaying.contextUri);
      sp_Display->printCurrentlyPlayingToScreen(currentlyPlaying);
    }

    albumArtChanged = sp_Display->processImageInfo(currentlyPlaying);
    sp_Display->displayTrackProgress(currentlyPlaying.progressMs, currentlyPlaying.durationMs);

    if (currentlyPlaying.isPlaying)
    {
      songStartMillis = millis() - currentlyPlaying.progressMs;
      songDuration = currentlyPlaying.durationMs;
    }
    else
    {
      songStartMillis = 0;
    }
  }
}

void updateProgressBar()
{
  if (songStartMillis != 0 && millis() > progressDueTime)
  {
    long songProgress = millis() - songStartMillis;
    if (songProgress > songDuration)
      songProgress = songDuration;

    sp_Display->displayTrackProgress(songProgress, songDuration);
    progressDueTime = millis() + delayBetweenProgressUpdates;
  }
}

void updateCurrentlyPlaying(boolean forceUpdate)
{
  // Never begin a comparatively heavy now-playing request while a touch command
  // is queued or being sent by the dedicated control task.
  if (spotifyControlCommandPending)
    return;

  if (forceUpdate || millis() > requestDueTime)
  {
    if (forceUpdate)
      Serial.println("forcing an update");

    Serial.println("getting currently playing song:");
    int status = spotify.getCurrentlyPlaying(handleCurrentlyPlaying, SPOTIFY_MARKET);
    if (status == 200)
    {
      Serial.println("Successfully got currently playing");
      if (albumArtChanged)
      {
        sp_Display->clearImage();
        int displayImageResult = sp_Display->displayImage();

        if (displayImageResult)
          albumArtChanged = false;
        else
        {
          Serial.print("failed to display image: ");
          Serial.println(displayImageResult);
        }
      }
    }
    else if (status == 204)
    {
      songStartMillis = 0;
      Serial.println("Doesn't seem to be anything playing");
    }
    else
    {
      Serial.print("Error: ");
      Serial.println(status);
    }

    // After a next/previous/play command, poll several times quickly. Spotify can
    // briefly return the old track immediately after accepting a command.
    if (rapidRefreshesRemaining > 0)
    {
      rapidRefreshesRemaining--;
      requestDueTime = millis() + 350;
    }
    else
    {
      requestDueTime = millis() + delayBetweenRequests;
    }
  }
}
