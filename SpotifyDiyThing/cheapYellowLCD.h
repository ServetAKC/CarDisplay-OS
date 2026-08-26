#include "spotifyDisplay.h"
#include "touchScreen.h"
#include "audioVisualizer.h"

#include <TFT_eSPI.h>
#include <JPEGDEC.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <math.h>
#include <Preferences.h>
#include "sdJapaneseFont.h"

#ifndef HONDATHING_TZ
// Sofia / Bulgaria, including daylight-saving time.
#define HONDATHING_TZ "EET-2EEST,M3.5.0/3,M10.5.0/4"
#endif

// Automatic sunset dimming location. Defaults to Sofia, Bulgaria.
// Change these two values if the device is used in another city.
#ifndef HONDATHING_LATITUDE
#define HONDATHING_LATITUDE 42.6977
#endif
#ifndef HONDATHING_LONGITUDE
#define HONDATHING_LONGITUDE 23.3219
#endif

// -------------------------------
// Display/JPEG globals
// -------------------------------

TFT_eSPI tft = TFT_eSPI();
JPEGDEC jpeg;
JPEGDEC cacheJpeg;

static constexpr int CACHED_ART_SIZE = 160;
static constexpr uint32_t RAW_CACHE_MAGIC = 0x48544331UL; // "HTC1"

struct RawCacheHeader
{
  uint32_t magic;
  uint16_t width;
  uint16_t height;
};

const char *ALBUM_ART = "/album.jpg";
const char *ALBUM_ART_TEMP = "/album.tmp";

// CYD microSD slot (ESP32-2432S028 / 2 USB)
static constexpr int SD_CS_PIN = 5;
const char *ALBUM_CACHE_DIR = "/hondathing_cache";

int JPEGDraw(JPEGDRAW *pDraw)
{
  if (pDraw->y >= tft.height())
    return 0;

  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

// JPEGDEC uses C-style callbacks. Keep separate input handles for the display
// decoder and the background raw-cache decoder so they can never corrupt each
// other's file position.
fs::File displayJpegFile;
fs::FS *displayJpegFs = &SPIFFS;

void *myOpen(const char *filename, int32_t *size)
{
  displayJpegFile = displayJpegFs->open(filename, FILE_READ);
  *size = displayJpegFile ? displayJpegFile.size() : 0;
  return &displayJpegFile;
}

void myClose(void *handle)
{
  if (displayJpegFile)
    displayJpegFile.close();
}

int32_t myRead(JPEGFILE *handle, uint8_t *buffer, int32_t length)
{
  if (!displayJpegFile)
    return 0;
  return displayJpegFile.read(buffer, length);
}

int32_t mySeek(JPEGFILE *handle, int32_t position)
{
  if (!displayJpegFile)
    return 0;
  return displayJpegFile.seek(position);
}

fs::File cacheJpegInputFile;
fs::File cacheRawOutputFile;
fs::FS *cacheJpegInputFs = &SD;

void *cacheJpegOpen(const char *filename, int32_t *size)
{
  cacheJpegInputFile = cacheJpegInputFs->open(filename, FILE_READ);
  *size = cacheJpegInputFile ? cacheJpegInputFile.size() : 0;
  return &cacheJpegInputFile;
}

void cacheJpegClose(void *handle)
{
  if (cacheJpegInputFile)
    cacheJpegInputFile.close();
}

int32_t cacheJpegRead(JPEGFILE *handle, uint8_t *buffer, int32_t length)
{
  if (!cacheJpegInputFile)
    return 0;
  return cacheJpegInputFile.read(buffer, length);
}

int32_t cacheJpegSeek(JPEGFILE *handle, int32_t position)
{
  if (!cacheJpegInputFile)
    return 0;
  return cacheJpegInputFile.seek(position);
}

int cacheJpegDraw(JPEGDRAW *pDraw)
{
  if (!cacheRawOutputFile || pDraw->x >= CACHED_ART_SIZE || pDraw->y >= CACHED_ART_SIZE)
    return 0;

  const int clippedWidth = min(pDraw->iWidth, CACHED_ART_SIZE - pDraw->x);
  const int clippedHeight = min(pDraw->iHeight, CACHED_ART_SIZE - pDraw->y);
  const size_t headerSize = sizeof(RawCacheHeader);

  for (int row = 0; row < clippedHeight; ++row)
  {
    const size_t pixelOffset =
        ((size_t)(pDraw->y + row) * CACHED_ART_SIZE + pDraw->x) * sizeof(uint16_t);
    if (!cacheRawOutputFile.seek(headerSize + pixelOffset))
      return 0;

    const uint8_t *rowBytes = reinterpret_cast<const uint8_t *>(
        pDraw->pPixels + (row * pDraw->iWidth));
    if (cacheRawOutputFile.write(rowBytes, clippedWidth * sizeof(uint16_t)) !=
        clippedWidth * sizeof(uint16_t))
      return 0;
  }

  return 1;
}

class CheapYellowDisplay : public SpotifyDisplay
{
public:
  bool isVisualizerActive() const
  {
    return visualizerMode;
  }

  void displaySetup(SpotifyArduino *spotifyObj) override
  {
    spotify_display = spotifyObj;
    _albumArtUrl[0] = '\0';

    touchSetup(spotifyObj);

    Serial.println("cyd display setup - Car Display OS v0.3.15 offline menu + buffered visualizers");
    setWidth(320);
    setHeight(240);
    setImageHeight(IMAGE_SIZE);
    setImageWidth(IMAGE_SIZE);

    tft.init();
    tft.setRotation(1);
    tft.setTextWrap(false);

    // Restore the last manually selected level before enabling the backlight.
    preferencesReady = preferences.begin("hondathing", false);
    if (preferencesReady)
    {
      const uint8_t savedBrightness = preferences.getUChar("brightness", 0);
      manualBrightnessIndex = savedBrightness < 6 ? savedBrightness : 0;
    }

    // CYD backlight is GPIO21. PWM allows smooth, reliable brightness levels.
    ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQUENCY, BACKLIGHT_PWM_BITS);
    ledcAttachPin(TFT_BL, BACKLIGHT_PWM_CHANNEL);
    applyBrightness(true);

    tft.fillScreen(TFT_BLACK);

    micReady = audioVisualizer.begin();
    if (!micReady)
      Serial.println("Visualizer will show MIC ERROR until I2S setup is fixed");

    // The CYD microSD slot uses the ESP32 VSPI pins (SCK 18, MISO 19,
    // MOSI 23, CS 5). Cache is optional: the player still works if no card
    // is inserted or mounting fails. Format the card as FAT32.
    sdCacheReady = SD.begin(SD_CS_PIN);
    if (sdCacheReady)
    {
      if (!SD.exists(ALBUM_CACHE_DIR))
        SD.mkdir(ALBUM_CACHE_DIR);
      japaneseFontReady = japaneseFont.begin(SD);
      Serial.print("SD album cache ready. Card MB: ");
      Serial.println((uint32_t)(SD.cardSize() / (1024ULL * 1024ULL)));
      Serial.println(japaneseFontReady
                         ? "Installed SD Japanese font ready"
                         : "Japanese font not installed yet");
    }
    else
    {
      Serial.println("SD album cache unavailable; using network only");
    }

    albumRequestQueue = xQueueCreate(1, sizeof(AlbumRequest));
    if (albumRequestQueue == nullptr)
    {
      Serial.println("Failed to create album-art queue");
      return;
    }

    BaseType_t taskResult = xTaskCreatePinnedToCore(
        albumDownloadTaskTrampoline,
        "albumDownload",
        8192,
        this,
        1,
        &albumDownloadTaskHandle,
        0);

    if (taskResult != pdPASS)
    {
      Serial.println("Failed to start album-art task");
      albumDownloadTaskHandle = nullptr;
    }

    playbackCommandQueue = xQueueCreate(4, sizeof(int8_t));
    if (playbackCommandQueue == nullptr)
    {
      Serial.println("Failed to create playback-command queue");
    }
    else
    {
      BaseType_t commandTaskResult = xTaskCreatePinnedToCore(
          playbackCommandTaskTrampoline,
          "spotifyControls",
          6144,
          this,
          3,
          &playbackCommandTaskHandle,
          0);

      if (commandTaskResult != pdPASS)
      {
        Serial.println("Failed to start playback-command task");
        playbackCommandTaskHandle = nullptr;
      }
    }
  }

  void startWiFiConnectingAnimation() override
  {
    stopWiFiConnectingAnimation();

    wifiAnimationActive = true;
    wifiAnimationFrame = 0;
    drawWiFiConnectingBase();

    BaseType_t result = xTaskCreatePinnedToCore(
        wifiAnimationTaskTrampoline,
        "wifiAnim",
        2048,
        this,
        1,
        &wifiAnimationTaskHandle,
        0);

    if (result != pdPASS)
    {
      wifiAnimationTaskHandle = nullptr;
      wifiAnimationActive = false;
      Serial.println("Failed to start Wi-Fi animation task");
    }
  }

  void stopWiFiConnectingAnimation() override
  {
    wifiAnimationActive = false;

    // Let the animation task leave cleanly so it cannot draw over the next screen.
    for (int i = 0; i < 25 && wifiAnimationTaskHandle != nullptr; ++i)
      delay(10);

    if (wifiAnimationTaskHandle != nullptr)
    {
      vTaskDelete(wifiAnimationTaskHandle);
      wifiAnimationTaskHandle = nullptr;
    }
  }

  void serviceConfigPortal() override
  {
    checkForInput();
    service();
  }

  void finishConfigPortal() override
  {
    wifiSetupMode = false;
    setTouchSetupPortalMode(false);
    setTouchVisualizerMode(false);
    visualizerMode = false;
    audioVisualizer.setActive(false);
    releaseVisualizerSprite();
    playerShellDrawn = false;
  }

  void showDefaultScreen() override
  {
    drawPlayerShell();

    if (!drawCurrentAlbumArt())
      drawAlbumPlaceholder();

    drawTrackPlaceholder();
    displayTrackProgress(0, 1);
    drawPlaybackControls(ACTION_NONE);
  }

  void displayTrackProgress(long progress, long duration) override
  {
    // Keep the latest state even while the full-screen clock is covering the UI.
    cachedProgress = progress;
    cachedDuration = duration > 0 ? duration : 1;

    if (overlayModeActive())
      return;

    ensurePlayerShell();

    if (duration <= 0)
      duration = 1;

    progress = constrain(progress, 0L, duration);
    const int lineWidth = map(progress, 0, duration, 0, PROGRESS_WIDTH);

    // Thin Car-Thing style progress line: a clearly visible dark track with a
    // bright played section. Only changed pixels are repainted to avoid flicker.
    if (lastProgressBarWidth < 0 || duration != lastProgressDuration)
    {
      tft.fillRect(PROGRESS_X, PROGRESS_Y,
                   PROGRESS_WIDTH, PROGRESS_HEIGHT, PROGRESS_TRACK);
      lastProgressBarWidth = 0;
      lastProgressDuration = duration;
    }

    if (lineWidth > lastProgressBarWidth)
    {
      tft.fillRect(PROGRESS_X + lastProgressBarWidth, PROGRESS_Y,
                   lineWidth - lastProgressBarWidth, PROGRESS_HEIGHT, HONDA_GREEN);
    }
    else if (lineWidth < lastProgressBarWidth)
    {
      tft.fillRect(PROGRESS_X + lineWidth, PROGRESS_Y,
                   lastProgressBarWidth - lineWidth, PROGRESS_HEIGHT, PROGRESS_TRACK);
    }

    // Elapsed and total time live just above the right end of the progress line.
    // Redraw this small area only when the shown second changes.
    char elapsedText[8];
    char durationText[8];
    formatDuration(progress, elapsedText, sizeof(elapsedText));
    formatDuration(duration, durationText, sizeof(durationText));

    if (strcmp(elapsedText, lastElapsedText) != 0 ||
        strcmp(durationText, lastDurationText) != 0)
    {
      strncpy(lastElapsedText, elapsedText, sizeof(lastElapsedText) - 1);
      lastElapsedText[sizeof(lastElapsedText) - 1] = '\0';
      strncpy(lastDurationText, durationText, sizeof(lastDurationText) - 1);
      lastDurationText[sizeof(lastDurationText) - 1] = '\0';

      char progressTimeText[20];
      snprintf(progressTimeText, sizeof(progressTimeText), "%s / %s",
               elapsedText, durationText);

      tft.fillRect(PROGRESS_TIME_X, PROGRESS_TIME_Y,
                   PROGRESS_TIME_WIDTH, PROGRESS_TIME_HEIGHT, TFT_BLACK);
      tft.setTextColor(HONDA_DIM, TFT_BLACK);
      tft.drawRightString(progressTimeText,
                          PROGRESS_X + PROGRESS_WIDTH,
                          PROGRESS_TIME_Y, 2);
    }

    lastProgressBarWidth = lineWidth;
  }

  void printCurrentlyPlayingToScreen(CurrentlyPlaying currentlyPlaying) override
  {
    playbackIsPlaying = currentlyPlaying.isPlaying;

    const char *trackName = safeText(currentlyPlaying.trackName);
    const char *artistName = currentlyPlaying.numArtists > 0
                                 ? safeText(currentlyPlaying.artists[0].artistName)
                                 : "Unknown Artist";
    const char *albumName = safeText(currentlyPlaying.albumName);

    copyCachedText(cachedTrackName, sizeof(cachedTrackName), trackName);
    copyCachedText(cachedArtistName, sizeof(cachedArtistName), artistName);
    copyCachedText(cachedAlbumName, sizeof(cachedAlbumName), albumName);
    cachedTrackValid = true;

    if (overlayModeActive())
      return;

    ensurePlayerShell();
    drawCachedTrackInfo();
    drawPlaybackControls(ACTION_NONE);
  }

  void setPlaybackState(bool isPlaying) override
  {
    if (playbackIsPlaying == isPlaying)
      return;

    playbackIsPlaying = isPlaying;
    if (playerShellDrawn && !overlayModeActive())
      drawPlaybackControls(ACTION_NONE);
  }

  void checkForInput() override
  {
    if (millis() <= touchScreenCoolDownTime || !handleTouched())
      return;

    if (brightnessCycleStatus)
    {
      cycleManualBrightness();
      touchScreenCoolDownTime = millis() + touchScreenCoolDownInterval;
      return;
    }

    if (visualizerNextModeStatus)
    {
      cycleVisualizerStyle();
      touchScreenCoolDownTime = millis() + touchScreenCoolDownInterval;
      return;
    }

    if (visualizerToggleStatus)
    {
      toggleVisualizerMode();
      touchScreenCoolDownTime = millis() + touchScreenCoolDownInterval;
      return;
    }

    if (offlineVisualizerStatus)
    {
      if (wifiSetupMode && !visualizerMode)
      {
        setTouchSetupPortalMode(false);
        toggleVisualizerMode();
      }
      touchScreenCoolDownTime = millis() + touchScreenCoolDownInterval;
      return;
    }

    if (clockToggleStatus)
    {
      toggleClockMode();
      touchScreenCoolDownTime = millis() + touchScreenCoolDownInterval;
      return;
    }

    // Playback controls are hidden and disabled under either full-screen overlay.
    if (overlayModeActive())
      return;

    int8_t activeAction = ACTION_NONE;
    if (previousTrackStatus)
      activeAction = ACTION_PREVIOUS;
    else if (playPauseStatus)
      activeAction = ACTION_PLAY_PAUSE;
    else if (nextTrackStatus)
      activeAction = ACTION_NEXT;

    if (activeAction == ACTION_NONE)
      return;

    // Optimistic feedback: the icon changes immediately and the HTTPS request is
    // sent by a dedicated high-priority task. The screen loop never waits for it.
    bool toggledPlayState = false;
    int8_t commandAction = activeAction;
    if (activeAction == ACTION_PLAY_PAUSE)
    {
      playbackIsPlaying = !playbackIsPlaying;
      toggledPlayState = true;
      commandAction = playbackIsPlaying ? ACTION_PLAY : ACTION_PAUSE;
    }

    // Restore any previously highlighted icon before showing the new tap.
    if (controlFeedbackAction != ACTION_NONE && controlFeedbackAction != activeAction)
      drawSinglePlaybackControl(controlFeedbackAction, HONDA_GREEN);

    drawSinglePlaybackControl(activeAction,
                              (activeAction == ACTION_PREVIOUS || activeAction == ACTION_NEXT)
                                  ? HONDA_PRESS
                                  : HONDA_BRIGHT);
    controlFeedbackAction = activeAction;
    controlFeedbackUntil = millis() + CONTROL_FEEDBACK_MS;

    bool queued = false;
    if (playbackCommandQueue != nullptr && playbackCommandTaskHandle != nullptr)
    {
      spotifyControlCommandPending = true;
      queued = xQueueSend(playbackCommandQueue, &commandAction, 0) == pdTRUE;
      if (!queued)
        spotifyControlCommandPending = uxQueueMessagesWaiting(playbackCommandQueue) > 0;
    }

    if (!queued)
    {
      // Very unlikely fallback if the task could not be created.
      bool commandOk = false;
      if (commandAction == ACTION_PREVIOUS)
        commandOk = spotify_display->previousTrack();
      else if (commandAction == ACTION_NEXT)
        commandOk = spotify_display->nextTrack();
      else if (commandAction == ACTION_PLAY)
        commandOk = spotify_display->play();
      else if (commandAction == ACTION_PAUSE)
        commandOk = spotify_display->pause();

      if (!commandOk && toggledPlayState)
        playbackIsPlaying = !playbackIsPlaying;

      rapidRefreshesRemaining = commandOk ? 5 : 0;
      requestDueTime = millis() + (commandOk ? 250 : 1000);
    }

    touchScreenCoolDownTime = millis() + touchScreenCoolDownInterval;
  }

  // Draw completed album art on the main Arduino loop. TFT/JPEG operations stay
  // out of the network task, so the display driver is only touched from one task.
  void service() override
  {
    serviceClock();
    serviceBrightness();

    if (clockMode)
      serviceFullscreenClock();
    else if (visualizerMode)
      serviceVisualizer();
    else
      serviceControlFeedback();

    if (playPauseRollbackRequested)
    {
      playPauseRollbackRequested = false;
      playbackIsPlaying = !playbackIsPlaying;
      if (!overlayModeActive())
        drawSinglePlaybackControl(ACTION_PLAY_PAUSE, HONDA_GREEN);
    }

    // The visualizer owns the render loop while it is open. Deferring album file
    // hand-off and cache conversion prevents SD/network work from interrupting
    // animation frames. Everything resumes immediately after leaving the overlay.
    if (visualizerMode)
      return;

    // processImageInfo() runs inside Spotify's response callback. Let that HTTPS
    // request finish before the second TLS connection starts.
    if (!albumReady)
    {
      if (albumDownloadInFlight)
        albumStartAllowed = true;
      return;
    }

    const uint32_t readyGeneration = albumReadyGeneration;
    const uint32_t currentGeneration = albumRequestGeneration;
    const uint8_t readySource = albumReadySource;
    char readyPath[sizeof(albumReadyPath)] = {};
    strncpy(readyPath, albumReadyPath, sizeof(readyPath) - 1);

    if (readyGeneration != currentGeneration)
    {
      if (readySource == ALBUM_SOURCE_SPIFFS_TEMP && SPIFFS.exists(ALBUM_ART_TEMP))
        SPIFFS.remove(ALBUM_ART_TEMP);

      albumReady = false;
      albumDownloadInFlight = false;
      return;
    }

    bool fileReady = false;
    if (readySource == ALBUM_SOURCE_SPIFFS_TEMP)
    {
      if (SPIFFS.exists(ALBUM_ART))
        SPIFFS.remove(ALBUM_ART);
      fileReady = SPIFFS.exists(ALBUM_ART_TEMP) &&
                  SPIFFS.rename(ALBUM_ART_TEMP, ALBUM_ART);
      if (fileReady)
      {
        currentAlbumSource = ALBUM_SOURCE_SPIFFS_CURRENT;
        strncpy(currentAlbumPath, ALBUM_ART, sizeof(currentAlbumPath) - 1);
      }
    }
    else if (readySource == ALBUM_SOURCE_SD_RAW)
    {
      fileReady = sdCacheReady && SD.exists(readyPath);
      if (fileReady)
      {
        currentAlbumSource = ALBUM_SOURCE_SD_RAW;
        strncpy(currentAlbumPath, readyPath, sizeof(currentAlbumPath) - 1);
      }
    }
    else if (readySource == ALBUM_SOURCE_SD_JPEG)
    {
      fileReady = sdCacheReady && SD.exists(readyPath);
      if (fileReady)
      {
        currentAlbumSource = ALBUM_SOURCE_SD_JPEG;
        strncpy(currentAlbumPath, readyPath, sizeof(currentAlbumPath) - 1);
      }
    }

    currentAlbumPath[sizeof(currentAlbumPath) - 1] = '\0';

    if (fileReady)
    {
      if (overlayModeActive())
      {
        // Keep the newest cover ready, but do not draw over the full-screen overlay.
        albumDisplayed = false;
      }
      else
      {
        albumDisplayed = drawCurrentAlbumArt();
        Serial.print("async imageStatus: ");
        Serial.println(albumDisplayed ? 1 : 0);
      }
    }
    else
    {
      albumDisplayed = false;
      nextAlbumRetryTime = millis() + albumRetryInterval;
      Serial.println("Album-art cache/temp file was not ready");
    }

    albumReadySource = ALBUM_SOURCE_NONE;
    albumReadyPath[0] = '\0';
    albumReady = false;
    albumDownloadInFlight = false;
  }

  void clearImage() override
  {
    if (overlayModeActive())
      return;

    tft.fillRect(IMAGE_X, IMAGE_Y, IMAGE_SIZE, IMAGE_SIZE, TFT_BLACK);
    tft.drawRect(IMAGE_X - 1, IMAGE_Y - 1, IMAGE_SIZE + 2, IMAGE_SIZE + 2, HONDA_GREEN);
  }

  boolean processImageInfo(CurrentlyPlaying currentlyPlaying) override
  {
    if (currentlyPlaying.numImages <= 0)
      return false;

    // Spotify returns images largest-first. Use the 640 px cover so
    // JPEG_SCALE_QUARTER produces a crisp 160 x 160 image.
    int imageIndex = 0;
    SpotifyImage image = currentlyPlaying.albumImages[imageIndex];

    if (image.url == nullptr || image.url[0] == '\0')
      return false;

    const bool differentAlbum = !isSameAlbum(image.url);
    if (differentAlbum)
    {
      setImageHeight(IMAGE_SIZE);
      setImageWidth(IMAGE_SIZE);
      setAlbumArtUrl(image.url);
      albumDisplayed = false;
      queueAlbumDownload(image.url);
    }
    else if (!albumDisplayed && !albumDownloadInFlight && !albumReady && millis() >= nextAlbumRetryTime)
    {
      queueAlbumDownload(image.url);
    }

    // Returning false prevents spotifyLogic.h from using its old blocking path.
    return false;
  }

  int displayImage() override
  {
    if (overlayModeActive())
      return 0;
    albumDisplayed = drawCurrentAlbumArt();
    return albumDisplayed ? 1 : 0;
  }

  void markDisplayAsTagRead() override
  {
    if (overlayModeActive())
      return;
    tft.drawRect(IMAGE_X - 2, IMAGE_Y - 2, IMAGE_SIZE + 4, IMAGE_SIZE + 4, TFT_BLUE);
  }

  void markDisplayAsTagWritten() override
  {
    if (overlayModeActive())
      return;
    tft.drawRect(IMAGE_X - 2, IMAGE_Y - 2, IMAGE_SIZE + 4, IMAGE_SIZE + 4, HONDA_GREEN);
  }

  void drawWifiManagerMessage(WiFiManager *myWiFiManager) override
  {
    wifiSetupMode = true;
    strncpy(wifiSetupSsid, myWiFiManager->getConfigPortalSSID().c_str(),
            sizeof(wifiSetupSsid) - 1);
    wifiSetupSsid[sizeof(wifiSetupSsid) - 1] = '\0';
    strncpy(wifiSetupIp, WiFi.softAPIP().toString().c_str(),
            sizeof(wifiSetupIp) - 1);
    wifiSetupIp[sizeof(wifiSetupIp) - 1] = '\0';
    clockMode = false;
    visualizerMode = false;
    audioVisualizer.setActive(false);
    setTouchClockMode(false);
    setTouchVisualizerMode(false);
    setTouchSetupPortalMode(true);
    Serial.println("Entered Conf Mode");
    drawWifiSetupScreen();
  }

  void drawWifiSetupScreen()
  {
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(3, 3, 314, 234, HONDA_GREEN);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawCentreString("WI-FI SETUP", screenCenterX, 8, 4);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawCentreString("Connect phone or use offline mode", screenCenterX, 39, 2);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawString("Network:", 18, 68, 2);
    tft.drawString(wifiSetupSsid, 104, 68, 2);
    tft.drawString("Password:", 18, 91, 2);
    tft.drawString("thing123", 104, 91, 2);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawString("Setup address:", 18, 120, 2);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawCentreString(wifiSetupIp, screenCenterX, 139, 4);
    tft.drawRect(32, 176, 256, 56, Y2K_GLOW);
    tft.drawRect(34, 178, 252, 52, Y2K_NEON);
    tft.setTextColor(Y2K_MINT, TFT_BLACK);
    tft.drawCentreString("OFFLINE VISUALIZER", screenCenterX, 186, 2);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawCentreString("MIC MODE - NO INTERNET", screenCenterX, 207, 1);
  }

  void drawRefreshTokenMessage() override
  {
    wifiSetupMode = false;
    setTouchSetupPortalMode(false);
    clockMode = false;
    visualizerMode = false;
    audioVisualizer.setActive(false);
    setTouchClockMode(false);
    setTouchVisualizerMode(false);
    Serial.println("Refresh Token Mode");
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(3, 3, 314, 234, HONDA_GREEN);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawCentreString("SPOTIFY AUTH", screenCenterX, 16, 4);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawCentreString("Open the address below", screenCenterX, 66, 2);
    tft.drawCentreString("and authorize this device", screenCenterX, 86, 2);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawCentreString(WiFi.localIP().toString(), screenCenterX, 128, 4);
  }

private:
  static constexpr uint16_t HONDA_GREEN = 0x4EE6;
  static constexpr uint16_t HONDA_BRIGHT = 0x9FF0;
  static constexpr uint16_t HONDA_PRESS = 0xDFFB; // pale mint highlight for previous/next taps
  static constexpr uint16_t HONDA_DIM = 0x2363;
  static constexpr uint16_t HONDA_DARK = 0x1A22;
  static constexpr uint16_t PROGRESS_TRACK = 0x2A65; // dark muted green for the unplayed line
  static constexpr uint16_t Y2K_NEON = 0x2FEC;
  static constexpr uint16_t Y2K_LIME = 0xAFE7;
  static constexpr uint16_t Y2K_MINT = 0x67F6;
  static constexpr uint16_t Y2K_GLOW = 0x03E7;
  static constexpr uint16_t Y2K_DEEP = 0x0183;

  static constexpr int HEADER_HEIGHT = 30;
  static constexpr int CONTENT_Y = 34;
  static constexpr int CONTENT_HEIGHT = 152;
  static constexpr int IMAGE_X = 8; // aligned with PROGRESS_X
  static constexpr int IMAGE_Y = 34;
  static constexpr int IMAGE_SIZE = 160;
  static constexpr int TEXT_X = 174;
  static constexpr int TEXT_WIDTH = 138;

  static constexpr int PROGRESS_AREA_Y = 190;
  static constexpr int PROGRESS_AREA_HEIGHT = 16;
  static constexpr int PROGRESS_X = 8;
  static constexpr int PROGRESS_Y = 197;
  static constexpr int PROGRESS_WIDTH = 304;
  static constexpr int PROGRESS_HEIGHT = 2;
  static constexpr int PROGRESS_TIME_X = 202;
  static constexpr int PROGRESS_TIME_Y = 178;
  static constexpr int PROGRESS_TIME_WIDTH = 110;
  static constexpr int PROGRESS_TIME_HEIGHT = 17;

  // Keep eight unique covers ahead. On every track change the cache window
  // rolls forward, skips existing files and downloads the next missing ones.
  static constexpr size_t QUEUE_PREFETCH_LIMIT = 8;
  char queuedAlbumUrls[QUEUE_PREFETCH_LIMIT][SPOTIFY_QUEUE_URL_SIZE] = {};

  static constexpr int CONTROLS_Y = 218;
  static constexpr int CONTROLS_CLEAR_Y = 205;
  static constexpr int CONTROLS_CLEAR_HEIGHT = 28;

  static constexpr int8_t ACTION_PREVIOUS = -1;
  static constexpr int8_t ACTION_NONE = -2;
  static constexpr int8_t ACTION_NEXT = 1;
  static constexpr int8_t ACTION_PLAY_PAUSE = 2;
  static constexpr int8_t ACTION_PLAY = 3;
  static constexpr int8_t ACTION_PAUSE = 4;
  static constexpr int8_t ACTION_CLOCK = 5;
  static constexpr int8_t ACTION_VISUALIZER = 6;

  enum VisualizerStyle : uint8_t
  {
    VISUALIZER_SPECTRUM = 0,
    VISUALIZER_MIRRORED,
    VISUALIZER_OSCILLOSCOPE,
    VISUALIZER_BLOCK_EQ,
    VISUALIZER_VU_METER,
    VISUALIZER_RADIAL,
    VISUALIZER_PULSE_RING,
    VISUALIZER_WATERFALL,
    VISUALIZER_CYBER_GRID,
    VISUALIZER_LASER_TUNNEL,
    VISUALIZER_DATA_RAIN,
    VISUALIZER_NEON_WAVE,
    VISUALIZER_ORBIT_LINK,
    VISUALIZER_PIXEL_CITY,
    VISUALIZER_RADAR_2000,
    VISUALIZER_DUAL_DISC,
    VISUALIZER_STYLE_COUNT
  };

  enum AlbumSource : uint8_t
  {
    ALBUM_SOURCE_NONE = 0,
    ALBUM_SOURCE_SPIFFS_TEMP,
    ALBUM_SOURCE_SPIFFS_CURRENT,
    ALBUM_SOURCE_SD_JPEG,
    ALBUM_SOURCE_SD_RAW
  };

  struct AlbumRequest
  {
    char url[256];
    uint32_t generation;
  };

  QueueHandle_t albumRequestQueue = nullptr;
  TaskHandle_t albumDownloadTaskHandle = nullptr;

  QueueHandle_t playbackCommandQueue = nullptr;
  TaskHandle_t playbackCommandTaskHandle = nullptr;
  volatile bool playPauseRollbackRequested = false;
  int8_t controlFeedbackAction = ACTION_NONE;
  unsigned long controlFeedbackUntil = 0;
  static constexpr unsigned long CONTROL_FEEDBACK_MS = 220;

  volatile bool albumDownloadInFlight = false;
  volatile bool albumReady = false;
  volatile bool albumStartAllowed = false;
  volatile uint32_t albumRequestGeneration = 0;
  volatile uint32_t albumReadyGeneration = 0;
  volatile uint8_t albumReadySource = ALBUM_SOURCE_NONE;
  char albumReadyPath[64] = "";

  uint8_t currentAlbumSource = ALBUM_SOURCE_NONE;
  char currentAlbumPath[64] = "";

  unsigned long nextAlbumRetryTime = 0;
  const unsigned long albumRetryInterval = 15000;

  unsigned long touchScreenCoolDownInterval = 150;
  unsigned long touchScreenCoolDownTime = 0;

  // Manual levels cycle 100/75/50/25/10/5 and are stored in ESP32 NVS.
  // At sunset, night mode applies a one-shot 25% cap. The first manual
  // brightness press releases that cap, so 50/75/100 remain available all night.
  // Daylight clears the one-shot state and the next sunset can dim once again.
  static constexpr uint8_t BACKLIGHT_PWM_CHANNEL = 7;
  static constexpr uint16_t BACKLIGHT_PWM_FREQUENCY = 5000;
  static constexpr uint8_t BACKLIGHT_PWM_BITS = 8;
  Preferences preferences;
  bool preferencesReady = false;
  uint8_t manualBrightnessIndex = 0;
  uint8_t appliedBrightnessPercent = 255;
  bool automaticNightMode = false;
  bool nightDimOverrideActive = false;
  unsigned long lastBrightnessCheckTime = 0;
  unsigned long brightnessToastUntil = 0;
  bool brightnessToastVisible = false;

  bool playerShellDrawn = false;
  bool playbackIsPlaying = true;
  int lastProgressBarWidth = -1;
  long lastProgressDuration = -1;
  char lastElapsedText[8] = "";
  char lastDurationText[8] = "";
  bool clockConfigured = false;
  unsigned long lastClockDrawTime = 0;
  char lastClockText[6] = "";
  bool clockMode = false;
  unsigned long lastFullscreenClockDrawTime = 0;
  char lastFullscreenClockText[6] = "";

  AudioVisualizer audioVisualizer;
  bool micReady = false;
  volatile bool visualizerMode = false;
  bool wifiSetupMode = false;
  char wifiSetupSsid[34] = "SpotifyDIY";
  char wifiSetupIp[20] = "192.168.4.1";
  TFT_eSprite visualizerSprite = TFT_eSprite(&tft);
  bool visualizerSpriteReady = false;
  VisualizerStyle visualizerStyle = VISUALIZER_PULSE_RING; // intentionally survives close/reopen
  unsigned long lastVisualizerDrawTime = 0;
  uint32_t lastVisualizerFrame = 0;
  uint16_t lastVisualizerHeight[AudioVisualizer::BAR_COUNT] = {};
  uint16_t visualizerPeakHeight[AudioVisualizer::BAR_COUNT] = {};
  unsigned long visualizerPeakHoldUntil[AudioVisualizer::BAR_COUNT] = {};
  uint8_t lastBlockCount[AudioVisualizer::BAR_COUNT] = {};
  int16_t lastOscilloscopeY[AudioVisualizer::WAVEFORM_COUNT] = {};
  bool oscilloscopeFrameValid = false;
  uint16_t lastRadialLength[AudioVisualizer::BAR_COUNT] = {};
  static constexpr size_t PULSE_RING_BAR_COUNT = AudioVisualizer::BAR_COUNT * 2;
  uint8_t lastPulseRingLength[PULSE_RING_BAR_COUNT] = {};
  uint16_t retroVisualizerFrame = 0;
  int lastVuWidth = 0;
  int vuPeakWidth = 0;
  unsigned long vuPeakHoldUntil = 0;
  int waterfallWriteY = 64;
  unsigned long lastWaterfallAdvanceTime = 0;

  bool sdCacheReady = false;
  SdJapaneseFont japaneseFont;
  bool japaneseFontReady = false;

  // Cached player state used for an instant return from full-screen clock mode.
  char cachedTrackName[101] = "";
  char cachedArtistName[101] = "";
  char cachedAlbumName[101] = "";
  bool cachedTrackValid = false;
  long cachedProgress = 0;
  long cachedDuration = 1;

  // Startup Wi-Fi animation state.
  TaskHandle_t wifiAnimationTaskHandle = nullptr;
  volatile bool wifiAnimationActive = false;
  volatile uint8_t wifiAnimationFrame = 0;

  static void wifiAnimationTaskTrampoline(void *parameter)
  {
    CheapYellowDisplay *display = static_cast<CheapYellowDisplay *>(parameter);
    display->wifiAnimationTask();
  }

  static void albumDownloadTaskTrampoline(void *parameter)
  {
    CheapYellowDisplay *display = static_cast<CheapYellowDisplay *>(parameter);
    display->albumDownloadTask();
  }

  static void playbackCommandTaskTrampoline(void *parameter)
  {
    CheapYellowDisplay *display = static_cast<CheapYellowDisplay *>(parameter);
    display->playbackCommandTask();
  }

  static double normalizeDegrees(double value)
  {
    while (value < 0.0)
      value += 360.0;
    while (value >= 360.0)
      value -= 360.0;
    return value;
  }

  static double degreesToRadians(double value)
  {
    return value * 0.017453292519943295;
  }

  static double radiansToDegrees(double value)
  {
    return value * 57.29577951308232;
  }

  int dayOfYear(const struct tm &timeInfo) const
  {
    return timeInfo.tm_yday + 1;
  }

  // NOAA-style sunrise/sunset approximation. Returned value is UTC decimal hour.
  // sunrise=true calculates sunrise; false calculates sunset.
  double solarEventUtcHour(const struct tm &utcDate, bool sunrise) const
  {
    const double latitude = HONDATHING_LATITUDE;
    const double longitude = HONDATHING_LONGITUDE;
    const double zenith = 90.833; // standard refraction + solar radius
    const int n = dayOfYear(utcDate);
    const double lngHour = longitude / 15.0;
    const double approximateTime =
        n + (((sunrise ? 6.0 : 18.0) - lngHour) / 24.0);

    const double meanAnomaly = (0.9856 * approximateTime) - 3.289;
    double trueLongitude =
        meanAnomaly +
        (1.916 * sin(degreesToRadians(meanAnomaly))) +
        (0.020 * sin(2.0 * degreesToRadians(meanAnomaly))) +
        282.634;
    trueLongitude = normalizeDegrees(trueLongitude);

    double rightAscension = radiansToDegrees(
        atan(0.91764 * tan(degreesToRadians(trueLongitude))));
    rightAscension = normalizeDegrees(rightAscension);

    const double longitudeQuadrant = floor(trueLongitude / 90.0) * 90.0;
    const double rightAscensionQuadrant = floor(rightAscension / 90.0) * 90.0;
    rightAscension += longitudeQuadrant - rightAscensionQuadrant;
    rightAscension /= 15.0;

    const double sinDeclination =
        0.39782 * sin(degreesToRadians(trueLongitude));
    const double cosDeclination =
        cos(asin(sinDeclination));

    const double cosHourAngle =
        (cos(degreesToRadians(zenith)) -
         (sinDeclination * sin(degreesToRadians(latitude)))) /
        (cosDeclination * cos(degreesToRadians(latitude)));

    if (cosHourAngle > 1.0 || cosHourAngle < -1.0)
      return sunrise ? 6.0 : 18.0; // polar fallback; irrelevant for Sofia

    double localHourAngle = sunrise
                                ? 360.0 - radiansToDegrees(acos(cosHourAngle))
                                : radiansToDegrees(acos(cosHourAngle));
    localHourAngle /= 15.0;

    const double localMeanTime =
        localHourAngle + rightAscension -
        (0.06571 * approximateTime) - 6.622;

    double utcHour = localMeanTime - lngHour;
    while (utcHour < 0.0)
      utcHour += 24.0;
    while (utcHour >= 24.0)
      utcHour -= 24.0;
    return utcHour;
  }

  bool isAfterSunset() const
  {
    const time_t now = time(nullptr);
    if (now < 1700000000)
      return false; // NTP not ready yet

    struct tm utcNow;
    gmtime_r(&now, &utcNow);

    const double currentUtcHour =
        utcNow.tm_hour + (utcNow.tm_min / 60.0) + (utcNow.tm_sec / 3600.0);
    const double sunriseUtc = solarEventUtcHour(utcNow, true);
    const double sunsetUtc = solarEventUtcHour(utcNow, false);

    return currentUtcHour >= sunsetUtc || currentUtcHour < sunriseUtc;
  }

  static uint8_t manualBrightnessPercent(uint8_t index)
  {
    // Switch form avoids the C++11 static-array linker issue seen on ESP32.
    switch (index % 6)
    {
    case 0:
      return 100;
    case 1:
      return 75;
    case 2:
      return 50;
    case 3:
      return 25;
    case 4:
      return 10;
    default:
      return 5;
    }
  }

  uint8_t effectiveBrightnessPercent() const
  {
    uint8_t percent = manualBrightnessPercent(manualBrightnessIndex);
    if (nightDimOverrideActive && percent > 25)
      percent = 25;
    return percent;
  }

  void applyBrightness(bool force = false)
  {
    const uint8_t percent = effectiveBrightnessPercent();
    if (!force && percent == appliedBrightnessPercent)
      return;

    appliedBrightnessPercent = percent;
    const uint32_t maxDuty = (1U << BACKLIGHT_PWM_BITS) - 1U;
    const uint32_t duty = (maxDuty * percent) / 100U;
    ledcWrite(BACKLIGHT_PWM_CHANNEL, duty);

    Serial.print("Display brightness: ");
    Serial.print(percent);
    Serial.print("%");
    Serial.println(nightDimOverrideActive ? " (one-shot night dim active)" : "");
  }

  void saveManualBrightness()
  {
    if (preferencesReady)
      preferences.putUChar("brightness", manualBrightnessIndex);
  }

  void drawBrightnessToast()
  {
    if (overlayModeActive() || !playerShellDrawn)
      return;

    char value[8];
    snprintf(value, sizeof(value), "%u%%", effectiveBrightnessPercent());
    tft.fillRect(5, 5, 92, HEADER_HEIGHT - 6, TFT_BLACK);
    tft.setTextColor(HONDA_BRIGHT, TFT_BLACK);
    tft.drawCentreString(value, 49, 8, 2);
    brightnessToastVisible = true;
    brightnessToastUntil = millis() + 900;
  }

  void cycleManualBrightness()
  {
    // A manual press is an explicit override for the rest of this night.
    // The saved manual level continues cycling normally and is never
    // overwritten by the automatic 25% sunset dim.
    nightDimOverrideActive = false;
    manualBrightnessIndex = (manualBrightnessIndex + 1) % 6;
    saveManualBrightness();
    applyBrightness(true);
    drawBrightnessToast();
  }

  void serviceBrightness()
  {
    if (millis() - lastBrightnessCheckTime >= 30000)
    {
      lastBrightnessCheckTime = millis();
      const bool shouldBeNight = isAfterSunset();
      if (shouldBeNight != automaticNightMode)
      {
        automaticNightMode = shouldBeNight;

        // Apply the sunset dim only once. Once the user touches the brightness
        // control, cycleManualBrightness() releases this override and the
        // 30-second service loop will not re-apply it until the next day/night
        // transition.
        nightDimOverrideActive = shouldBeNight;
        applyBrightness(true);
        if (!overlayModeActive())
          drawBrightnessToast();
      }
    }

    if (brightnessToastVisible && millis() >= brightnessToastUntil)
    {
      brightnessToastVisible = false;
      if (!overlayModeActive() && playerShellDrawn)
        drawHeader(true);
    }
  }

  bool overlayModeActive() const
  {
    return clockMode || visualizerMode;
  }

  const char *safeText(const char *text)
  {
    return (text == nullptr || text[0] == '\0') ? "-" : text;
  }

  void ensurePlayerShell()
  {
    if (!playerShellDrawn)
      drawPlayerShell();
  }

  void drawPlayerShell()
  {
    wifiSetupMode = false;
    setTouchSetupPortalMode(false);
    clockMode = false;
    visualizerMode = false;
    audioVisualizer.setActive(false);
    setTouchClockMode(false);
    setTouchVisualizerMode(false);
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(3, 3, 314, 234, HONDA_DARK);
    tft.drawFastHLine(8, HEADER_HEIGHT, 304, HONDA_DARK);
    tft.drawRect(IMAGE_X - 1, IMAGE_Y - 1, IMAGE_SIZE + 2, IMAGE_SIZE + 2, HONDA_GREEN);
    drawHeader(true);
    drawPlaybackControls(ACTION_NONE);
    lastProgressBarWidth = -1;
    lastProgressDuration = -1;
    lastElapsedText[0] = '\0';
    lastDurationText[0] = '\0';
    playerShellDrawn = true;
  }

  void drawHeader(bool force = false)
  {
    char clockText[6] = "--:--";
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 5))
      strftime(clockText, sizeof(clockText), "%H:%M", &timeInfo);

    if (!force && strcmp(clockText, lastClockText) == 0)
      return;

    strncpy(lastClockText, clockText, sizeof(lastClockText) - 1);
    lastClockText[sizeof(lastClockText) - 1] = '\0';

    tft.fillRect(5, 5, 310, HEADER_HEIGHT - 6, TFT_BLACK);
    drawSpotifyIcon(17, 16, 9, HONDA_GREEN);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawString("Spotify", 31, 8, 2);

    const int clockWidth = tft.textWidth(clockText, 2);
    tft.drawString(clockText, screenCenterX - (clockWidth / 2), 8, 2);

    drawWifiIndicator();
    tft.drawFastHLine(8, HEADER_HEIGHT, 304, HONDA_DARK);
  }

  void serviceClock()
  {
    if (!clockConfigured && WiFi.status() == WL_CONNECTED)
    {
      configTzTime(HONDATHING_TZ, "pool.ntp.org", "time.nist.gov");
      clockConfigured = true;
      lastClockDrawTime = 0;
    }

    if (overlayModeActive() || !playerShellDrawn || millis() - lastClockDrawTime < 1000)
      return;

    lastClockDrawTime = millis();
    drawHeader();
  }

  void restorePlayerScreen()
  {
    playerShellDrawn = false;
    drawPlayerShell();

    if (!drawCurrentAlbumArt())
      drawAlbumPlaceholder();

    if (cachedTrackValid)
      drawCachedTrackInfo();
    else
      drawTrackPlaceholder();

    displayTrackProgress(cachedProgress, cachedDuration);
    drawPlaybackControls(ACTION_NONE);
  }

  void toggleClockMode()
  {
    clockMode = !clockMode;
    visualizerMode = false;
    audioVisualizer.setActive(false);
    setTouchVisualizerMode(false);
    setTouchClockMode(clockMode);
    controlFeedbackAction = ACTION_NONE;

    if (clockMode)
    {
      // Spotify polling and album downloads continue in the background. Only the
      // TFT contents are replaced by the clock overlay.
      playerShellDrawn = false;
      lastFullscreenClockText[0] = '\0';
      lastFullscreenClockDrawTime = 0;
      tft.fillScreen(TFT_BLACK);
      drawFullscreenClock(true);
      return;
    }

    restorePlayerScreen();
  }

  void drawOverlayCloseButton()
  {
    static constexpr int X = 286;
    static constexpr int Y = 6;
    static constexpr int SIZE = 26;
    tft.fillRect(X, Y, SIZE, SIZE, TFT_BLACK);
    tft.drawRect(X, Y, SIZE, SIZE, HONDA_DIM);
    tft.drawLine(X + 7, Y + 7, X + SIZE - 8, Y + SIZE - 8, HONDA_GREEN);
    tft.drawLine(X + SIZE - 8, Y + 7, X + 7, Y + SIZE - 8, HONDA_GREEN);
  }

  const char *visualizerStyleName() const
  {
    switch (visualizerStyle)
    {
    case VISUALIZER_SPECTRUM:
      return "SPECTRUM";
    case VISUALIZER_MIRRORED:
      return "MIRRORED";
    case VISUALIZER_OSCILLOSCOPE:
      return "OSCILLOSCOPE";
    case VISUALIZER_BLOCK_EQ:
      return "BLOCK EQ";
    case VISUALIZER_VU_METER:
      return "VU METER";
    case VISUALIZER_RADIAL:
      return "RADIAL";
    case VISUALIZER_PULSE_RING:
      return "PULSE RING";
    case VISUALIZER_WATERFALL:
      return "WATERFALL";
    case VISUALIZER_CYBER_GRID:
      return "CYBER GRID";
    case VISUALIZER_LASER_TUNNEL:
      return "LASER TUNNEL";
    case VISUALIZER_DATA_RAIN:
      return "DATA RAIN";
    case VISUALIZER_NEON_WAVE:
      return "NEON WAVE";
    case VISUALIZER_ORBIT_LINK:
      return "ORBIT LINK";
    case VISUALIZER_PIXEL_CITY:
      return "PIXEL CITY";
    case VISUALIZER_RADAR_2000:
      return "RADAR 2000";
    case VISUALIZER_DUAL_DISC:
      return "DUAL DISC";
    default:
      return "VISUALIZER";
    }
  }

  void drawVisualizerChrome()
  {
    char label[28];
    snprintf(label, sizeof(label), "%u/%u  %s",
             static_cast<unsigned>(visualizerStyle) + 1U,
             static_cast<unsigned>(VISUALIZER_STYLE_COUNT),
             visualizerStyleName());
    tft.fillRect(0, 0, 278, 40, TFT_BLACK);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawString(label, 8, 12, 2);
    drawOverlayCloseButton();
  }

  void resetVisualizerDrawingState()
  {
    lastVisualizerFrame = 0;
    memset(lastVisualizerHeight, 0, sizeof(lastVisualizerHeight));
    memset(visualizerPeakHeight, 0, sizeof(visualizerPeakHeight));
    memset(visualizerPeakHoldUntil, 0, sizeof(visualizerPeakHoldUntil));
    memset(lastBlockCount, 0, sizeof(lastBlockCount));
    memset(lastOscilloscopeY, 0, sizeof(lastOscilloscopeY));
    oscilloscopeFrameValid = false;
    memset(lastRadialLength, 0, sizeof(lastRadialLength));
    memset(lastPulseRingLength, 0, sizeof(lastPulseRingLength));
    retroVisualizerFrame = 0;
    lastVuWidth = 0;
    vuPeakWidth = 0;
    vuPeakHoldUntil = 0;
    waterfallWriteY = 64;
    lastWaterfallAdvanceTime = 0;
  }

  void cycleVisualizerStyle()
  {
    if (!visualizerMode)
      return;

    visualizerStyle = static_cast<VisualizerStyle>(
        (static_cast<uint8_t>(visualizerStyle) + 1U) %
        static_cast<uint8_t>(VISUALIZER_STYLE_COUNT));
    resetVisualizerDrawingState();
    lastVisualizerDrawTime = 0;
    drawVisualizerFrame(true);
  }

  bool ensureVisualizerSprite()
  {
    if (visualizerSpriteReady)
      return true;

    visualizerSprite.setColorDepth(8);
    if (visualizerSprite.createSprite(320, 200) == nullptr)
    {
      Serial.println("Visualizer frame buffer allocation failed; using direct draw");
      return false;
    }
    // New Y2K functions keep normal screen coordinates (content starts at y=40).
    visualizerSprite.setOrigin(0, -40);
    visualizerSprite.setTextWrap(false);
    visualizerSpriteReady = true;
    return true;
  }

  void releaseVisualizerSprite()
  {
    if (!visualizerSpriteReady)
      return;
    visualizerSprite.deleteSprite();
    visualizerSpriteReady = false;
  }

  void toggleVisualizerMode()
  {
    visualizerMode = !visualizerMode;
    clockMode = false;
    setTouchClockMode(false);
    setTouchVisualizerMode(visualizerMode);
    controlFeedbackAction = ACTION_NONE;

    if (visualizerMode)
    {
      setTouchSetupPortalMode(false);
      playerShellDrawn = false;
      lastVisualizerDrawTime = 0;
      resetVisualizerDrawingState();
      albumStartAllowed = false;
      tft.fillScreen(TFT_BLACK);
      audioVisualizer.setActive(true);
      drawVisualizerFrame(true);
      return;
    }

    audioVisualizer.setActive(false);
    releaseVisualizerSprite();
    if (wifiSetupMode)
    {
      setTouchSetupPortalMode(true);
      drawWifiSetupScreen();
      return;
    }
    if (albumDownloadInFlight)
      albumStartAllowed = true;
    requestDueTime = 0;
    restorePlayerScreen();
  }

  void serviceVisualizer()
  {
    // About 24 FPS keeps motion fluid. Buffered Y2K modes are pushed as one
    // completed frame, so the user never sees the black clear pass.
    if (!visualizerMode || millis() - lastVisualizerDrawTime < 42)
      return;

    lastVisualizerDrawTime = millis();
    drawVisualizerFrame(false);
  }

  void drawSpectrumWithPeaks(const uint8_t *levels, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int LEFT = 8;
    static constexpr int TOP = 44;
    static constexpr int BASELINE = 230;
    static constexpr int MAX_HEIGHT = BASELINE - TOP;
    static constexpr int GAP = 3;
    static constexpr int BAR_WIDTH = 16;
    static constexpr int STEP = BAR_WIDTH + GAP;
    const unsigned long now = millis();

    tft.startWrite();
    if (force)
      tft.drawFastHLine(LEFT, BASELINE, 304, HONDA_DARK);

    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int x = LEFT + static_cast<int>(i) * STEP;
      int height = map(levels[i], 0, 100, 0, MAX_HEIGHT);
      height = (height / 2) * 2;
      const int oldHeight = lastVisualizerHeight[i];
      const int oldPeak = visualizerPeakHeight[i];

      if (height > oldHeight)
        tft.fillRect(x, BASELINE - height, BAR_WIDTH, height - oldHeight, HONDA_GREEN);
      else if (height < oldHeight)
        tft.fillRect(x, BASELINE - oldHeight, BAR_WIDTH, oldHeight - height, TFT_BLACK);

      // Restore what belongs underneath the previous peak before moving it.
      if (oldPeak > 0)
      {
        const uint16_t underneath = oldPeak <= height ? HONDA_GREEN : TFT_BLACK;
        tft.fillRect(x, BASELINE - oldPeak, BAR_WIDTH, 2, underneath);
      }

      int newPeak = oldPeak;
      if (height >= oldPeak)
      {
        newPeak = height;
        visualizerPeakHoldUntil[i] = now + 180;
      }
      else if (now >= visualizerPeakHoldUntil[i] && oldPeak > 0)
      {
        newPeak = max(height, oldPeak - 2);
      }

      if (newPeak > 0)
        tft.fillRect(x, BASELINE - newPeak, BAR_WIDTH, 2, HONDA_BRIGHT);

      visualizerPeakHeight[i] = newPeak;
      lastVisualizerHeight[i] = height;
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawMirroredSpectrum(const uint8_t *levels, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int LEFT = 8;
    static constexpr int CENTRE_Y = 137;
    static constexpr int MAX_HALF_HEIGHT = 88;
    static constexpr int GAP = 3;
    static constexpr int BAR_WIDTH = 16;
    static constexpr int STEP = BAR_WIDTH + GAP;

    tft.startWrite();
    if (force)
      tft.drawFastHLine(LEFT, CENTRE_Y, 304, HONDA_DIM);

    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int x = LEFT + static_cast<int>(i) * STEP;
      int height = map(levels[i], 0, 100, 0, MAX_HALF_HEIGHT);
      height = (height / 2) * 2;
      const int oldHeight = lastVisualizerHeight[i];
      const int difference = abs(height - oldHeight);

      if (height > oldHeight)
      {
        tft.fillRect(x, CENTRE_Y - height, BAR_WIDTH, difference, HONDA_GREEN);
        tft.fillRect(x, CENTRE_Y + oldHeight + 1, BAR_WIDTH, difference, HONDA_GREEN);
      }
      else if (height < oldHeight)
      {
        tft.fillRect(x, CENTRE_Y - oldHeight, BAR_WIDTH, difference, TFT_BLACK);
        tft.fillRect(x, CENTRE_Y + height + 1, BAR_WIDTH, difference, TFT_BLACK);
      }
      lastVisualizerHeight[i] = height;
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawOscilloscope(const uint8_t *waveform, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int LEFT = 8;
    static constexpr int RIGHT = 311;
    static constexpr int TOP = 50;
    static constexpr int BOTTOM = 226;
    static constexpr int CENTRE_Y = (TOP + BOTTOM) / 2;

    int16_t nextY[AudioVisualizer::WAVEFORM_COUNT];
    for (size_t i = 0; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
      nextY[i] = map(waveform[i], 0, 100, BOTTOM, TOP);

    tft.startWrite();
    if (oscilloscopeFrameValid && !force)
    {
      for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
      {
        const int x0 = LEFT + static_cast<int>((i - 1) * (RIGHT - LEFT) /
                                               (AudioVisualizer::WAVEFORM_COUNT - 1));
        const int x1 = LEFT + static_cast<int>(i * (RIGHT - LEFT) /
                                               (AudioVisualizer::WAVEFORM_COUNT - 1));
        tft.drawLine(x0, lastOscilloscopeY[i - 1], x1, lastOscilloscopeY[i], TFT_BLACK);
        tft.drawLine(x0, lastOscilloscopeY[i - 1] + 1,
                     x1, lastOscilloscopeY[i] + 1, TFT_BLACK);
      }
    }

    tft.drawFastHLine(LEFT, CENTRE_Y, RIGHT - LEFT + 1, HONDA_DARK);
    for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
    {
      const int x0 = LEFT + static_cast<int>((i - 1) * (RIGHT - LEFT) /
                                             (AudioVisualizer::WAVEFORM_COUNT - 1));
      const int x1 = LEFT + static_cast<int>(i * (RIGHT - LEFT) /
                                             (AudioVisualizer::WAVEFORM_COUNT - 1));
      tft.drawLine(x0, nextY[i - 1], x1, nextY[i], HONDA_GREEN);
      tft.drawLine(x0, nextY[i - 1] + 1, x1, nextY[i] + 1, HONDA_DIM);
    }
    tft.endWrite();
    presentVisualizerCanvas();

    memcpy(lastOscilloscopeY, nextY, sizeof(lastOscilloscopeY));
    oscilloscopeFrameValid = true;
  }

  void drawBlockEqualizer(const uint8_t *levels)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int LEFT = 8;
    static constexpr int BASELINE = 230;
    static constexpr int GAP = 3;
    static constexpr int BAR_WIDTH = 16;
    static constexpr int STEP = BAR_WIDTH + GAP;
    static constexpr int SEGMENT_COUNT = 12;
    static constexpr int SEGMENT_HEIGHT = 11;
    static constexpr int SEGMENT_GAP = 4;

    tft.startWrite();
    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int x = LEFT + static_cast<int>(i) * STEP;
      int activeBlocks = levels[i] == 0 ? 0 : (levels[i] * SEGMENT_COUNT + 99) / 100;
      activeBlocks = constrain(activeBlocks, 0, SEGMENT_COUNT);
      const int oldBlocks = lastBlockCount[i];

      if (activeBlocks > oldBlocks)
      {
        if (oldBlocks > 0)
        {
          const int oldTopY = BASELINE - oldBlocks * (SEGMENT_HEIGHT + SEGMENT_GAP) + SEGMENT_GAP;
          tft.fillRect(x, oldTopY, BAR_WIDTH, SEGMENT_HEIGHT, HONDA_GREEN);
        }
        for (int segment = oldBlocks; segment < activeBlocks; ++segment)
        {
          const int y = BASELINE - (segment + 1) * (SEGMENT_HEIGHT + SEGMENT_GAP) + SEGMENT_GAP;
          tft.fillRect(x, y, BAR_WIDTH, SEGMENT_HEIGHT, HONDA_GREEN);
        }
      }
      else if (activeBlocks < oldBlocks)
      {
        for (int segment = activeBlocks; segment < oldBlocks; ++segment)
        {
          const int y = BASELINE - (segment + 1) * (SEGMENT_HEIGHT + SEGMENT_GAP) + SEGMENT_GAP;
          tft.fillRect(x, y, BAR_WIDTH, SEGMENT_HEIGHT, TFT_BLACK);
        }
      }

      if (activeBlocks > 0 && activeBlocks != oldBlocks)
      {
        const int topY = BASELINE - activeBlocks * (SEGMENT_HEIGHT + SEGMENT_GAP) + SEGMENT_GAP;
        tft.fillRect(x, topY, BAR_WIDTH, SEGMENT_HEIGHT, HONDA_BRIGHT);
      }
      lastBlockCount[i] = activeBlocks;
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawVuMeter(uint8_t overall, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int BAR_X = 22;
    static constexpr int BAR_Y = 102;
    static constexpr int BAR_WIDTH = 276;
    static constexpr int BAR_HEIGHT = 28;
    const unsigned long now = millis();
    const int width = map(overall, 0, 100, 0, BAR_WIDTH);

    if (force)
    {
      tft.drawRect(BAR_X - 2, BAR_Y - 2, BAR_WIDTH + 4, BAR_HEIGHT + 4, HONDA_DIM);
      for (int i = 0; i <= 10; ++i)
      {
        const int x = BAR_X + (BAR_WIDTH * i) / 10;
        tft.drawFastVLine(x, BAR_Y + BAR_HEIGHT + 8, (i % 5 == 0) ? 9 : 5, HONDA_DIM);
      }
    }

    tft.startWrite();
    if (width > lastVuWidth)
      tft.fillRect(BAR_X + lastVuWidth, BAR_Y, width - lastVuWidth, BAR_HEIGHT, HONDA_GREEN);
    else if (width < lastVuWidth)
      tft.fillRect(BAR_X + width, BAR_Y, lastVuWidth - width, BAR_HEIGHT, TFT_BLACK);

    if (vuPeakWidth > 0)
    {
      const uint16_t underneath = vuPeakWidth <= width ? HONDA_GREEN : TFT_BLACK;
      tft.fillRect(BAR_X + vuPeakWidth - 2, BAR_Y, 2, BAR_HEIGHT, underneath);
    }

    if (width >= vuPeakWidth)
    {
      vuPeakWidth = width;
      vuPeakHoldUntil = now + 220;
    }
    else if (now >= vuPeakHoldUntil && vuPeakWidth > 0)
    {
      vuPeakWidth = max(width, vuPeakWidth - 4);
    }

    if (vuPeakWidth > 0)
      tft.fillRect(BAR_X + vuPeakWidth - 2, BAR_Y, 2, BAR_HEIGHT, HONDA_BRIGHT);
    tft.endWrite();
    presentVisualizerCanvas();
    lastVuWidth = width;
  }

  void drawRadialSpectrum(const uint8_t *levels, uint8_t overall, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int CENTRE_X = 160;
    static constexpr int CENTRE_Y = 138;
    static constexpr int INNER_RADIUS = 34;
    static constexpr int MAX_LENGTH = 60;
    static const int16_t DX[AudioVisualizer::BAR_COUNT] = {
        0, 383, 707, 924, 1000, 924, 707, 383,
        0, -383, -707, -924, -1000, -924, -707, -383};
    static const int16_t DY[AudioVisualizer::BAR_COUNT] = {
        -1000, -924, -707, -383, 0, 383, 707, 924,
        1000, 924, 707, 383, 0, -383, -707, -924};

    tft.startWrite();

    // Erase every old spoke first, then redraw the entire current radial frame.
    // This prevents one shrinking spoke from cutting holes into a neighbour.
    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int oldLength = lastRadialLength[i];
      if (oldLength <= 0 || force)
        continue;

      const int startRadius = INNER_RADIUS + 3;
      const int endRadius = startRadius + oldLength;
      const int startX = CENTRE_X + (DX[i] * startRadius) / 1000;
      const int startY = CENTRE_Y + (DY[i] * startRadius) / 1000;
      const int endX = CENTRE_X + (DX[i] * endRadius) / 1000;
      const int endY = CENTRE_Y + (DY[i] * endRadius) / 1000;
      const int perpX = -DY[i];
      const int perpY = DX[i];
      for (int offset = -1; offset <= 1; ++offset)
      {
        const int ox = (perpX * offset) / 1000;
        const int oy = (perpY * offset) / 1000;
        tft.drawLine(startX + ox, startY + oy, endX + ox, endY + oy, TFT_BLACK);
      }
      tft.fillCircle(endX, endY, 2, TFT_BLACK);
    }

    // A compact pulsing core makes the mode read as one coherent radial meter.
    tft.fillCircle(CENTRE_X, CENTRE_Y, 31, TFT_BLACK);
    const int coreRadius = map(overall, 0, 100, 12, 27);
    tft.fillCircle(CENTRE_X, CENTRE_Y, coreRadius, HONDA_DARK);
    tft.drawCircle(CENTRE_X, CENTRE_Y, coreRadius, HONDA_GREEN);
    tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS, HONDA_DIM);

    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int length = map(levels[i], 0, 100, 0, MAX_LENGTH);
      const int startRadius = INNER_RADIUS + 3;
      if (length > 0)
      {
        const int endRadius = startRadius + length;
        const int startX = CENTRE_X + (DX[i] * startRadius) / 1000;
        const int startY = CENTRE_Y + (DY[i] * startRadius) / 1000;
        const int endX = CENTRE_X + (DX[i] * endRadius) / 1000;
        const int endY = CENTRE_Y + (DY[i] * endRadius) / 1000;
        const int perpX = -DY[i];
        const int perpY = DX[i];
        const uint16_t color = levels[i] > 72 ? HONDA_BRIGHT : HONDA_GREEN;
        for (int offset = -1; offset <= 1; ++offset)
        {
          const int ox = (perpX * offset) / 1000;
          const int oy = (perpY * offset) / 1000;
          tft.drawLine(startX + ox, startY + oy, endX + ox, endY + oy, color);
        }
        tft.fillCircle(endX, endY, 2, HONDA_BRIGHT);
      }
      lastRadialLength[i] = length;
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawPulseRing(const uint8_t *levels, uint8_t overall, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int CENTRE_X = 160;
    static constexpr int CENTRE_Y = 139;
    static constexpr int INNER_RADIUS = 47;
    static constexpr int START_RADIUS = INNER_RADIUS + 4;
    static constexpr int MAX_LENGTH = 45;
    static constexpr uint16_t NEON_GREEN = 0x2FEC;
    static constexpr uint16_t NEON_LIME = 0xAFE7;
    static constexpr uint16_t MINT_GREEN = 0x67F6;
    static constexpr uint16_t GLOW_GREEN = 0x03E7;
    static constexpr uint16_t DEEP_GREEN = 0x0183;
    static constexpr uint16_t CORE_DARK = 0x10C4;

    // 32 fixed directions keep this mode light enough for the ESP32 while
    // making the 16 FFT bands read as one smooth, mirrored circular spectrum.
    static const int16_t DX[PULSE_RING_BAR_COUNT] = {
        0, 195, 383, 556, 707, 831, 924, 981,
        1000, 981, 924, 831, 707, 556, 383, 195,
        0, -195, -383, -556, -707, -831, -924, -981,
        -1000, -981, -924, -831, -707, -556, -383, -195};
    static const int16_t DY[PULSE_RING_BAR_COUNT] = {
        -1000, -981, -924, -831, -707, -556, -383, -195,
        0, 195, 383, 556, 707, 831, 924, 981,
        1000, 981, 924, 831, 707, 556, 383, 195,
        0, -195, -383, -556, -707, -831, -924, -981};

    tft.startWrite();

    // Clear the previous ring before repainting it. Clearing every spoke first
    // prevents shrinking neighbouring bars from punching black gaps in each other.
    if (!force)
    {
      for (size_t i = 0; i < PULSE_RING_BAR_COUNT; ++i)
      {
        const int oldLength = lastPulseRingLength[i];
        if (oldLength <= 0)
          continue;

        const int endRadius = START_RADIUS + oldLength;
        const int startX = CENTRE_X + (DX[i] * START_RADIUS) / 1000;
        const int startY = CENTRE_Y + (DY[i] * START_RADIUS) / 1000;
        const int endX = CENTRE_X + (DX[i] * endRadius) / 1000;
        const int endY = CENTRE_Y + (DY[i] * endRadius) / 1000;
        const int perpX = -DY[i];
        const int perpY = DX[i];
        for (int offset = -1; offset <= 1; ++offset)
        {
          const int ox = (perpX * offset) / 1000;
          const int oy = (perpY * offset) / 1000;
          tft.drawLine(startX + ox, startY + oy, endX + ox, endY + oy, TFT_BLACK);
        }
        tft.fillCircle(endX, endY, 2, TFT_BLACK);
      }
    }

    for (size_t i = 0; i < PULSE_RING_BAR_COUNT; ++i)
    {
      const size_t sourceBand = i < AudioVisualizer::BAR_COUNT
                                    ? i
                                    : (PULSE_RING_BAR_COUNT - 1U - i);
      const uint8_t level = levels[sourceBand];
      const int length = 3 + map(level, 0, 100, 0, MAX_LENGTH - 3);
      const int endRadius = START_RADIUS + length;
      const int startX = CENTRE_X + (DX[i] * START_RADIUS) / 1000;
      const int startY = CENTRE_Y + (DY[i] * START_RADIUS) / 1000;
      const int endX = CENTRE_X + (DX[i] * endRadius) / 1000;
      const int endY = CENTRE_Y + (DY[i] * endRadius) / 1000;
      const int perpX = -DY[i];
      const int perpY = DX[i];

      uint16_t color = NEON_GREEN;
      uint16_t glowColor = DEEP_GREEN;
      const uint8_t colorPhase = i % 12U;
      if (colorPhase >= 4U && colorPhase < 9U)
      {
        color = NEON_LIME;
        glowColor = GLOW_GREEN;
      }
      else if (colorPhase >= 9U)
      {
        color = MINT_GREEN;
        glowColor = GLOW_GREEN;
      }
      if (level > 78)
        color = MINT_GREEN;

      for (int offset = -1; offset <= 1; ++offset)
      {
        const int ox = (perpX * offset) / 1000;
        const int oy = (perpY * offset) / 1000;
        tft.drawLine(startX + ox, startY + oy, endX + ox, endY + oy,
                     offset == 0 ? color : glowColor);
      }
      if (level > 68)
        tft.fillCircle(endX, endY, 2, color);
      lastPulseRingLength[i] = static_cast<uint8_t>(length);
    }

    // Brand-free geometric core. Low bands add movement, while the existing
    // microphone gate keeps idle cabin noise from making it constantly pulse.
    const uint16_t bassAverage =
        (static_cast<uint16_t>(levels[0]) + levels[1] + levels[2] + levels[3]) / 4U;
    const uint8_t coreEnergy = constrain((overall * 2U + bassAverage) / 3U, 0U, 100U);
    const int coreRadius = map(coreEnergy, 0, 100, 25, 38);
    tft.fillCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS - 2, TFT_BLACK);
    tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS, GLOW_GREEN);
    tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS - 2, DEEP_GREEN);
    tft.fillCircle(CENTRE_X, CENTRE_Y, coreRadius - 7, CORE_DARK);
    tft.drawCircle(CENTRE_X, CENTRE_Y, coreRadius, NEON_GREEN);
    tft.drawLine(CENTRE_X, CENTRE_Y - coreRadius,
                 CENTRE_X + coreRadius, CENTRE_Y, MINT_GREEN);
    tft.drawLine(CENTRE_X + coreRadius, CENTRE_Y,
                 CENTRE_X, CENTRE_Y + coreRadius, NEON_LIME);
    tft.drawLine(CENTRE_X, CENTRE_Y + coreRadius,
                 CENTRE_X - coreRadius, CENTRE_Y, NEON_GREEN);
    tft.drawLine(CENTRE_X - coreRadius, CENTRE_Y,
                 CENTRE_X, CENTRE_Y - coreRadius, MINT_GREEN);
    const int innerDiamond = max(8, coreRadius - 11);
    tft.drawLine(CENTRE_X, CENTRE_Y - innerDiamond,
                 CENTRE_X + innerDiamond, CENTRE_Y, GLOW_GREEN);
    tft.drawLine(CENTRE_X + innerDiamond, CENTRE_Y,
                 CENTRE_X, CENTRE_Y + innerDiamond, DEEP_GREEN);
    tft.drawLine(CENTRE_X, CENTRE_Y + innerDiamond,
                 CENTRE_X - innerDiamond, CENTRE_Y, DEEP_GREEN);
    tft.drawLine(CENTRE_X - innerDiamond, CENTRE_Y,
                 CENTRE_X, CENTRE_Y - innerDiamond, GLOW_GREEN);
    tft.fillCircle(CENTRE_X, CENTRE_Y, 3, MINT_GREEN);

    tft.endWrite();
    presentVisualizerCanvas();
  }

  TFT_eSPI &visualizerCanvas()
  {
    return visualizerSpriteReady
               ? static_cast<TFT_eSPI &>(visualizerSprite)
               : ::tft;
  }

  void presentVisualizerCanvas()
  {
    if (visualizerSpriteReady)
      visualizerSprite.pushSprite(0, 40);
  }

  void drawCyberGrid(const uint8_t *levels, uint8_t overall)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int HORIZON_Y = 92;
    static constexpr int BOTTOM_Y = 234;
    ++retroVisualizerFrame;

    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);

    const int sunRadius = map(overall, 0, 100, 10, 22);
    tft.drawCircle(160, 67, sunRadius, Y2K_GLOW);
    tft.drawCircle(160, 67, max(4, sunRadius - 4), Y2K_DEEP);

    // Audio skyline at the vanishing line.
    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int x = 8 + static_cast<int>(i) * 19;
      const int height = map(levels[i], 0, 100, 2, 43);
      const uint16_t color = levels[i] > 70 ? Y2K_LIME : Y2K_GLOW;
      tft.fillRect(x, HORIZON_Y - height, 12, height, Y2K_DEEP);
      tft.drawFastHLine(x, HORIZON_Y - height, 12, color);
    }

    tft.drawFastHLine(0, HORIZON_Y, 320, Y2K_NEON);
    for (int ray = 0; ray <= 10; ++ray)
    {
      const int bottomX = 8 + ray * 30;
      const uint8_t level = levels[(ray * 3) % AudioVisualizer::BAR_COUNT];
      tft.drawLine(160, HORIZON_Y, bottomX, BOTTOM_Y,
                   level > 55 ? Y2K_GLOW : Y2K_DEEP);
    }

    const int phase = retroVisualizerFrame % 16U;
    for (int row = 0; row < 10; ++row)
    {
      const int distance = (row * 16 + phase) % 143;
      const int y = HORIZON_Y + (distance * distance) / 143;
      if (y <= BOTTOM_Y)
        tft.drawFastHLine(7, y, 306, y > 190 ? Y2K_GLOW : Y2K_DEEP);
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawLaserTunnel(const uint8_t *levels, uint8_t overall)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int CENTRE_X = 160;
    static constexpr int CENTRE_Y = 139;
    ++retroVisualizerFrame;

    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);

    for (int layer = 0; layer < 7; ++layer)
    {
      int radius = 18 + ((layer * 17 + retroVisualizerFrame * 2U) % 76U);
      radius += map(levels[(layer * 2) % AudioVisualizer::BAR_COUNT], 0, 100, 0, 6);
      radius = min(radius, 94);
      const uint16_t color = layer % 3 == 0 ? Y2K_MINT
                               : layer % 3 == 1 ? Y2K_NEON
                                                : Y2K_GLOW;
      tft.drawLine(CENTRE_X, CENTRE_Y - radius,
                   CENTRE_X + radius, CENTRE_Y, color);
      tft.drawLine(CENTRE_X + radius, CENTRE_Y,
                   CENTRE_X, CENTRE_Y + radius, color);
      tft.drawLine(CENTRE_X, CENTRE_Y + radius,
                   CENTRE_X - radius, CENTRE_Y, color);
      tft.drawLine(CENTRE_X - radius, CENTRE_Y,
                   CENTRE_X, CENTRE_Y - radius, color);
    }

    const int core = map(overall, 0, 100, 5, 16);
    tft.fillCircle(CENTRE_X, CENTRE_Y, core, Y2K_DEEP);
    tft.drawCircle(CENTRE_X, CENTRE_Y, core, Y2K_LIME);
    tft.drawFastHLine(CENTRE_X - core, CENTRE_Y, core * 2 + 1, Y2K_NEON);
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawDataRain(const uint8_t *levels)
  {
    TFT_eSPI &tft = visualizerCanvas();
    ++retroVisualizerFrame;
    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);

    for (size_t column = 0; column < AudioVisualizer::BAR_COUNT; ++column)
    {
      const int x = 8 + static_cast<int>(column) * 19;
      const int speed = 2 + (column % 3U);
      const int headY = 46 + ((retroVisualizerFrame * speed + column * 31U) % 185U);
      const int trailLength = 4 + levels[column] / 20U;

      for (int trail = 0; trail < trailLength; ++trail)
      {
        int y = headY - trail * 9;
        while (y < 46)
          y += 185;
        const uint16_t color = trail == 0 ? Y2K_MINT
                                 : trail == 1 ? Y2K_NEON
                                 : trail < 4 ? Y2K_GLOW
                                             : Y2K_DEEP;
        const int width = trail == 0 ? 8 : 6;
        tft.fillRect(x + (8 - width) / 2, y, width, 3, color);
      }

      if (levels[column] > 72)
        tft.drawPixel(x + 4, headY - 2, Y2K_LIME);
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawNeonWave(const uint8_t *waveform, uint8_t overall)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int LEFT = 7;
    static constexpr int RIGHT = 312;
    static constexpr int CENTRE_Y = 139;
    ++retroVisualizerFrame;

    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);
    for (int y = 48; y <= 230; y += 12)
      tft.drawFastHLine(0, y, 320, Y2K_DEEP);
    tft.drawFastHLine(0, CENTRE_Y, 320, Y2K_GLOW);

    int previousX = LEFT;
    int previousY = map(waveform[0], 0, 100, 210, 68);
    int previousEchoY = CENTRE_Y * 2 - previousY;
    for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
    {
      const int x = LEFT + static_cast<int>(i) * (RIGHT - LEFT) /
                               (AudioVisualizer::WAVEFORM_COUNT - 1);
      const int y = map(waveform[i], 0, 100, 210, 68);
      const int echoY = CENTRE_Y * 2 - y;
      tft.drawLine(previousX, previousEchoY, x, echoY, Y2K_DEEP);
      tft.drawLine(previousX, previousY + 2, x, y + 2, Y2K_GLOW);
      tft.drawLine(previousX, previousY, x, y,
                   overall > 70 ? Y2K_LIME : Y2K_MINT);
      previousX = x;
      previousY = y;
      previousEchoY = echoY;
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawOrbitLink(const uint8_t *levels, uint8_t overall)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int CENTRE_X = 160;
    static constexpr int CENTRE_Y = 139;
    static const int16_t DX[32] = {
        0, 195, 383, 556, 707, 831, 924, 981,
        1000, 981, 924, 831, 707, 556, 383, 195,
        0, -195, -383, -556, -707, -831, -924, -981,
        -1000, -981, -924, -831, -707, -556, -383, -195};
    static const int16_t DY[32] = {
        -1000, -981, -924, -831, -707, -556, -383, -195,
        0, 195, 383, 556, 707, 831, 924, 981,
        1000, 981, 924, 831, 707, 556, 383, 195,
        0, -195, -383, -556, -707, -831, -924, -981};
    int16_t nodeX[AudioVisualizer::BAR_COUNT];
    int16_t nodeY[AudioVisualizer::BAR_COUNT];
    ++retroVisualizerFrame;
    const int rotation = (retroVisualizerFrame / 2U) % 32U;

    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);
    tft.drawCircle(CENTRE_X, CENTRE_Y, 34, Y2K_DEEP);
    tft.drawCircle(CENTRE_X, CENTRE_Y, 62, Y2K_GLOW);
    tft.drawCircle(CENTRE_X, CENTRE_Y, 88, Y2K_DEEP);

    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int direction = (static_cast<int>(i) * 2 + rotation) % 32;
      const int radius = 55 + map(levels[i], 0, 100, 0, 31);
      nodeX[i] = CENTRE_X + (DX[direction] * radius) / 1000;
      nodeY[i] = CENTRE_Y + (DY[direction] * radius) / 1000;
    }
    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const size_t next = (i + 1U) % AudioVisualizer::BAR_COUNT;
      tft.drawLine(nodeX[i], nodeY[i], nodeX[next], nodeY[next], Y2K_GLOW);
      const uint16_t color = levels[i] > 68 ? Y2K_LIME : Y2K_NEON;
      tft.fillCircle(nodeX[i], nodeY[i], levels[i] > 68 ? 3 : 2, color);
    }

    const int core = map(overall, 0, 100, 8, 19);
    tft.fillCircle(CENTRE_X, CENTRE_Y, core, Y2K_DEEP);
    tft.drawCircle(CENTRE_X, CENTRE_Y, core, Y2K_MINT);
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawPixelCity(const uint8_t *levels)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int BASELINE = 231;
    ++retroVisualizerFrame;
    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);
    tft.drawFastHLine(0, BASELINE, 320, Y2K_NEON);

    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const int x = 4 + static_cast<int>(i) * 20;
      const int height = 14 + map(levels[i], 0, 100, 0, 143);
      const int top = BASELINE - height;
      tft.fillRect(x, top, 15, height, Y2K_DEEP);
      tft.drawFastVLine(x, top, height, Y2K_GLOW);
      tft.drawFastVLine(x + 14, top, height, Y2K_GLOW);
      tft.drawFastHLine(x, top, 15, levels[i] > 70 ? Y2K_LIME : Y2K_NEON);

      int windowRow = 0;
      for (int y = BASELINE - 9; y > top + 4; y -= 14, ++windowRow)
      {
        const bool lit = (retroVisualizerFrame + i + windowRow) % 4U != 0U;
        const uint16_t windowColor = lit ? Y2K_GLOW : TFT_BLACK;
        tft.fillRect(x + 3, y, 3, 3, windowColor);
        tft.fillRect(x + 9, y, 3, 3, windowColor);
      }
      if (levels[i] > 82)
        tft.drawFastVLine(x + 7, top - 8, 8, Y2K_MINT);
    }
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawRadar2000(const uint8_t *levels, uint8_t overall)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int CENTRE_X = 160;
    static constexpr int CENTRE_Y = 139;
    static const int16_t DX[32] = {
        0, 195, 383, 556, 707, 831, 924, 981,
        1000, 981, 924, 831, 707, 556, 383, 195,
        0, -195, -383, -556, -707, -831, -924, -981,
        -1000, -981, -924, -831, -707, -556, -383, -195};
    static const int16_t DY[32] = {
        -1000, -981, -924, -831, -707, -556, -383, -195,
        0, 195, 383, 556, 707, 831, 924, 981,
        1000, 981, 924, 831, 707, 556, 383, 195,
        0, -195, -383, -556, -707, -831, -924, -981};
    ++retroVisualizerFrame;

    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);
    tft.drawCircle(CENTRE_X, CENTRE_Y, 28, Y2K_DEEP);
    tft.drawCircle(CENTRE_X, CENTRE_Y, 57, Y2K_GLOW);
    tft.drawCircle(CENTRE_X, CENTRE_Y, 88, Y2K_NEON);
    tft.drawFastHLine(CENTRE_X - 88, CENTRE_Y, 177, Y2K_DEEP);
    tft.drawFastVLine(CENTRE_X, CENTRE_Y - 88, 177, Y2K_DEEP);

    const int sweep = retroVisualizerFrame % 32U;
    for (int tail = 2; tail >= 0; --tail)
    {
      const int direction = (sweep + 32 - tail) % 32;
      const int endX = CENTRE_X + (DX[direction] * 87) / 1000;
      const int endY = CENTRE_Y + (DY[direction] * 87) / 1000;
      const uint16_t color = tail == 0 ? Y2K_MINT : tail == 1 ? Y2K_GLOW : Y2K_DEEP;
      tft.drawLine(CENTRE_X, CENTRE_Y, endX, endY, color);
    }

    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      if (levels[i] < 10)
        continue;
      const int direction = static_cast<int>(i) * 2;
      const int radius = 24 + map(levels[i], 0, 100, 0, 61);
      const int x = CENTRE_X + (DX[direction] * radius) / 1000;
      const int y = CENTRE_Y + (DY[direction] * radius) / 1000;
      tft.fillCircle(x, y, levels[i] > 70 ? 3 : 2,
                     levels[i] > 70 ? Y2K_LIME : Y2K_NEON);
    }
    tft.fillCircle(CENTRE_X, CENTRE_Y, map(overall, 0, 100, 2, 6), Y2K_MINT);
    tft.endWrite();
    presentVisualizerCanvas();
  }

  void drawDualDisc(const uint8_t *levels, uint8_t overall)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static const int16_t DX[16] = {
        0, 383, 707, 924, 1000, 924, 707, 383,
        0, -383, -707, -924, -1000, -924, -707, -383};
    static const int16_t DY[16] = {
        -1000, -924, -707, -383, 0, 383, 707, 924,
        1000, 924, 707, 383, 0, -383, -707, -924};
    static const int CENTRE_X[2] = {88, 232};
    static constexpr int CENTRE_Y = 137;
    ++retroVisualizerFrame;
    const int rotation = (retroVisualizerFrame / 2U) % 16U;

    tft.startWrite();
    tft.fillRect(0, 40, 320, 200, TFT_BLACK);
    for (int deck = 0; deck < 2; ++deck)
    {
      tft.drawCircle(CENTRE_X[deck], CENTRE_Y, 35, Y2K_DEEP);
      tft.drawCircle(CENTRE_X[deck], CENTRE_Y, 52, Y2K_GLOW);
      for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
      {
        const size_t source = deck == 0 ? i : AudioVisualizer::BAR_COUNT - 1U - i;
        const int direction = (static_cast<int>(i) + (deck == 0 ? rotation : 16 - rotation)) % 16;
        const int radius = 40 + map(levels[source], 0, 100, 0, 19);
        const int startX = CENTRE_X[deck] + (DX[direction] * 36) / 1000;
        const int startY = CENTRE_Y + (DY[direction] * 36) / 1000;
        const int endX = CENTRE_X[deck] + (DX[direction] * radius) / 1000;
        const int endY = CENTRE_Y + (DY[direction] * radius) / 1000;
        const uint16_t color = levels[source] > 68 ? Y2K_LIME : Y2K_NEON;
        tft.drawLine(startX, startY, endX, endY, color);
        if (levels[source] > 58)
          tft.fillCircle(endX, endY, 2, Y2K_MINT);
      }
      const int hub = map(overall, 0, 100, 7, 14);
      tft.fillCircle(CENTRE_X[deck], CENTRE_Y, hub, Y2K_DEEP);
      tft.drawCircle(CENTRE_X[deck], CENTRE_Y, hub, Y2K_MINT);
    }
    tft.drawFastHLine(139, 216, 43, Y2K_DEEP);
    const int sliderX = 139 + map(levels[7], 0, 100, 0, 42);
    tft.fillCircle(sliderX, 216, 3, Y2K_NEON);
    tft.endWrite();
    presentVisualizerCanvas();
  }

  uint16_t waterfallColor(uint8_t level) const
  {
    if (level < 12)
      return TFT_BLACK;
    if (level < 32)
      return HONDA_DARK;
    if (level < 55)
      return HONDA_DIM;
    if (level < 78)
      return HONDA_GREEN;
    return HONDA_BRIGHT;
  }

  void drawWaterfall(const uint8_t *levels, bool force)
  {
    TFT_eSPI &tft = visualizerCanvas();
    static constexpr int LEFT = 8;
    static constexpr int TOP = 64;
    static constexpr int BOTTOM = 228;
    static constexpr int BAND_COUNT = 8;
    static constexpr int BAND_WIDTH = 38;
    static constexpr int ROW_HEIGHT = 3;
    static constexpr int ROW_STEP = 4;
    const unsigned long now = millis();

    if (force)
    {
      waterfallWriteY = TOP;
      lastWaterfallAdvanceTime = 0;
      tft.fillRect(0, 40, 320, 200, TFT_BLACK);
      tft.setTextColor(HONDA_DIM, TFT_BLACK);
      tft.drawString("BASS", LEFT, 45, 2);
      tft.drawRightString("TREBLE", 312, 45, 2);
      for (int band = 1; band < BAND_COUNT; ++band)
        tft.drawFastVLine(LEFT + band * BAND_WIDTH, TOP, BOTTOM - TOP, HONDA_DARK);
    }

    // Ten history rows per second are easier to read and much lighter than
    // painting at the full visualizer frame rate.
    if (!force && now - lastWaterfallAdvanceTime < 100)
      return;
    lastWaterfallAdvanceTime = now;

    if (waterfallWriteY + ROW_STEP > BOTTOM)
    {
      tft.fillRect(LEFT, TOP, BAND_COUNT * BAND_WIDTH, BOTTOM - TOP, TFT_BLACK);
      for (int band = 1; band < BAND_COUNT; ++band)
        tft.drawFastVLine(LEFT + band * BAND_WIDTH, TOP, BOTTOM - TOP, HONDA_DARK);
      waterfallWriteY = TOP;
    }

    tft.startWrite();
    for (int band = 0; band < BAND_COUNT; ++band)
    {
      const uint8_t combined = static_cast<uint8_t>(
          (static_cast<uint16_t>(levels[band * 2]) + levels[band * 2 + 1]) / 2U);
      const int x = LEFT + band * BAND_WIDTH;
      tft.fillRect(x + 1, waterfallWriteY, BAND_WIDTH - 2, ROW_HEIGHT,
                   waterfallColor(combined));
    }
    tft.drawFastHLine(LEFT, waterfallWriteY + ROW_HEIGHT,
                      BAND_COUNT * BAND_WIDTH, HONDA_DIM);
    tft.endWrite();
    presentVisualizerCanvas();

    waterfallWriteY += ROW_STEP;
  }

  void drawVisualizerFrame(bool force)
  {
    if (!micReady)
    {
      if (force)
      {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(HONDA_GREEN, TFT_BLACK);
        tft.drawCentreString("MIC ERROR", screenCenterX, 92, 4);
        tft.setTextColor(HONDA_DIM, TFT_BLACK);
        tft.drawCentreString("Check INMP441 wiring", screenCenterX, 130, 2);
        drawOverlayCloseButton();
      }
      return;
    }

    uint8_t levels[AudioVisualizer::BAR_COUNT] = {};
    uint8_t waveform[AudioVisualizer::WAVEFORM_COUNT] = {};
    uint8_t overall = 0;
    const uint32_t frame = audioVisualizer.copyFrame(
        levels, AudioVisualizer::BAR_COUNT,
        waveform, AudioVisualizer::WAVEFORM_COUNT,
        &overall);
    const bool frameChanged = frame != lastVisualizerFrame;
    if (frameChanged)
      lastVisualizerFrame = frame;

    const bool continuousAnimationMode =
        visualizerStyle == VISUALIZER_SPECTRUM ||
        visualizerStyle == VISUALIZER_VU_METER ||
        visualizerStyle == VISUALIZER_PULSE_RING ||
        visualizerStyle >= VISUALIZER_CYBER_GRID;
    if (!force && !frameChanged && !continuousAnimationMode)
      return;

    if (force)
      tft.fillScreen(TFT_BLACK);

    ensureVisualizerSprite();
    if (force && visualizerSpriteReady)
      visualizerSprite.fillSprite(TFT_BLACK);

    switch (visualizerStyle)
    {
    case VISUALIZER_SPECTRUM:
      drawSpectrumWithPeaks(levels, force);
      break;
    case VISUALIZER_MIRRORED:
      drawMirroredSpectrum(levels, force);
      break;
    case VISUALIZER_OSCILLOSCOPE:
      drawOscilloscope(waveform, force);
      break;
    case VISUALIZER_BLOCK_EQ:
      drawBlockEqualizer(levels);
      break;
    case VISUALIZER_VU_METER:
      drawVuMeter(overall, force);
      break;
    case VISUALIZER_RADIAL:
      drawRadialSpectrum(levels, overall, force);
      break;
    case VISUALIZER_PULSE_RING:
      drawPulseRing(levels, overall, force);
      break;
    case VISUALIZER_WATERFALL:
      drawWaterfall(levels, force);
      break;
    case VISUALIZER_CYBER_GRID:
      drawCyberGrid(levels, overall);
      break;
    case VISUALIZER_LASER_TUNNEL:
      drawLaserTunnel(levels, overall);
      break;
    case VISUALIZER_DATA_RAIN:
      drawDataRain(levels);
      break;
    case VISUALIZER_NEON_WAVE:
      drawNeonWave(waveform, overall);
      break;
    case VISUALIZER_ORBIT_LINK:
      drawOrbitLink(levels, overall);
      break;
    case VISUALIZER_PIXEL_CITY:
      drawPixelCity(levels);
      break;
    case VISUALIZER_RADAR_2000:
      drawRadar2000(levels, overall);
      break;
    case VISUALIZER_DUAL_DISC:
      drawDualDisc(levels, overall);
      break;
    default:
      break;
    }

    if (force)
      drawVisualizerChrome();
  }

  void serviceFullscreenClock()
  {
    if (!clockMode || millis() - lastFullscreenClockDrawTime < 250)
      return;

    lastFullscreenClockDrawTime = millis();
    drawFullscreenClock(false);
  }

  void drawFullscreenClock(bool force)
  {
    char clockText[6] = "--:--";
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 5))
      strftime(clockText, sizeof(clockText), "%H:%M", &timeInfo);

    if (!force && strcmp(clockText, lastFullscreenClockText) == 0)
      return;

    strncpy(lastFullscreenClockText, clockText, sizeof(lastFullscreenClockText) - 1);
    lastFullscreenClockText[sizeof(lastFullscreenClockText) - 1] = '\0';

    // Intentionally minimal: pure black background and one large green clock.
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawCentreString(clockText, screenCenterX, 78, 8);
    drawOverlayCloseButton();
  }

  void copyCachedText(char *destination, size_t destinationSize, const char *source)
  {
    if (destinationSize == 0)
      return;

    strncpy(destination, safeText(source), destinationSize - 1);
    destination[destinationSize - 1] = '\0';
  }

  void drawCachedTrackInfo()
  {
    tft.fillRect(TEXT_X, CONTENT_Y, TEXT_WIDTH, CONTENT_HEIGHT, TFT_BLACK);

    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawString("NOW PLAYING", TEXT_X, CONTENT_Y + 3, 2);

    int titleFont = tft.textWidth(cachedTrackName, 4) <= TEXT_WIDTH ? 4 : 2;
    drawEllipsized(cachedTrackName, TEXT_X, CONTENT_Y + 23, TEXT_WIDTH, titleFont, HONDA_GREEN);
    drawEllipsized(cachedArtistName, TEXT_X, CONTENT_Y + 61, TEXT_WIDTH, 2, HONDA_GREEN);
    drawEllipsized(cachedAlbumName, TEXT_X, CONTENT_Y + 82, TEXT_WIDTH, 2, HONDA_DIM);
  }

  void drawWiFiConnectingBase()
  {
    clockMode = false;
    visualizerMode = false;
    audioVisualizer.setActive(false);
    setTouchClockMode(false);
    setTouchVisualizerMode(false);
    playerShellDrawn = false;

    tft.fillScreen(TFT_BLACK);
    tft.drawRect(3, 3, 314, 234, HONDA_DARK);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawCentreString("CONNECTING", screenCenterX, 72, 4);
  }

  void drawWiFiAnimationFrame(uint8_t frame)
  {
    static const int dotX[4] = {124, 148, 172, 196};
    const uint8_t active = frame % 4;

    tft.fillRect(108, 122, 104, 28, TFT_BLACK);
    for (uint8_t i = 0; i < 4; ++i)
    {
      const uint16_t color = (i == active) ? HONDA_BRIGHT : HONDA_DIM;
      const int radius = (i == active) ? 6 : 4;
      tft.fillCircle(dotX[i], 136, radius, color);
    }

    // Small animated signal bars below the dots.
    tft.fillRect(132, 164, 56, 30, TFT_BLACK);
    for (uint8_t i = 0; i < 4; ++i)
    {
      const int height = 5 + (i * 6);
      const bool lit = i <= active;
      tft.fillRect(134 + (i * 13), 192 - height, 8, height, lit ? HONDA_GREEN : HONDA_DARK);
    }
  }

  void wifiAnimationTask()
  {
    while (wifiAnimationActive)
    {
      drawWiFiAnimationFrame(wifiAnimationFrame++);
      vTaskDelay(pdMS_TO_TICKS(160));
    }

    wifiAnimationTaskHandle = nullptr;
    vTaskDelete(nullptr);
  }

  void drawSpotifyIcon(int centerX, int centerY, int radius, uint16_t color)
  {
    tft.drawCircle(centerX, centerY, radius, color);
    tft.drawLine(centerX - 5, centerY - 3, centerX + 5, centerY - 2, color);
    tft.drawLine(centerX - 4, centerY, centerX + 4, centerY + 1, color);
    tft.drawLine(centerX - 3, centerY + 3, centerX + 3, centerY + 4, color);
  }

  void drawWifiIndicator()
  {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const uint16_t color = connected ? HONDA_GREEN : HONDA_DIM;
    const int baseX = 287;
    const int baseY = 23;

    for (int i = 0; i < 4; i++)
    {
      const int height = 4 + (i * 3);
      tft.fillRect(baseX + (i * 6), baseY - height, 4, height, color);
    }
  }

  void drawAlbumPlaceholder()
  {
    clearImage();
    const int centerX = IMAGE_X + IMAGE_SIZE / 2;
    const int centerY = IMAGE_Y + 70;
    tft.drawCircle(centerX, centerY, 43, HONDA_DARK);
    tft.drawCircle(centerX, centerY, 36, HONDA_DIM);
    tft.drawCircle(centerX, centerY, 27, HONDA_DARK);
    tft.fillCircle(centerX, centerY, 8, HONDA_GREEN);
    tft.fillCircle(centerX, centerY, 3, TFT_BLACK);
    tft.drawLine(centerX + 27, centerY - 29, centerX + 42, centerY - 39, HONDA_GREEN);
    tft.drawLine(centerX + 42, centerY - 39, centerX + 48, centerY - 31, HONDA_GREEN);
    tft.fillCircle(centerX + 27, centerY - 29, 3, HONDA_GREEN);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawCentreString("NOW PLAYING", centerX, IMAGE_Y + 124, 2);
  }

  void drawTrackPlaceholder()
  {
    tft.fillRect(TEXT_X, CONTENT_Y, TEXT_WIDTH, CONTENT_HEIGHT, TFT_BLACK);
    tft.setTextColor(HONDA_DIM, TFT_BLACK);
    tft.drawString("NOW PLAYING", TEXT_X, CONTENT_Y + 3, 2);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawString("Waiting for", TEXT_X, CONTENT_Y + 33, 2);
    tft.drawString("Spotify...", TEXT_X, CONTENT_Y + 53, 2);
  }

  void drawSpotifyBadge(int x, int y)
  {
    drawSpotifyIcon(x + 9, y + 8, 8, HONDA_GREEN);
    tft.setTextColor(HONDA_GREEN, TFT_BLACK);
    tft.drawString("Spotify", x + 23, y, 2);
  }

  void drawEllipsized(const char *text, int x, int y, int maxWidth, int font, uint16_t color)
  {
    if (japaneseFontReady && SdJapaneseFont::containsNonAscii(text))
    {
      japaneseFont.drawText(tft, safeText(text), x, y, maxWidth, color, TFT_BLACK);
      return;
    }

    char buffer[101];
    strncpy(buffer, safeText(text), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    if (tft.textWidth(buffer, font) > maxWidth)
    {
      int len = strlen(buffer);
      while (len > 3)
      {
        buffer[len - 3] = '.';
        buffer[len - 2] = '.';
        buffer[len - 1] = '.';
        buffer[len] = '\0';

        if (tft.textWidth(buffer, font) <= maxWidth)
          break;

        len--;
        buffer[len] = '\0';
      }
    }

    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(buffer, x, y, font);
  }

  void formatDuration(long milliseconds, char *out, size_t outSize)
  {
    long totalSeconds = milliseconds / 1000;
    long minutes = totalSeconds / 60;
    long seconds = totalSeconds % 60;
    snprintf(out, outSize, "%ld:%02ld", minutes, seconds);
  }

  void drawSinglePlaybackControl(int8_t action, uint16_t color)
  {
    if (action == ACTION_PREVIOUS)
    {
      tft.fillRect(73, CONTROLS_CLEAR_Y, 36, CONTROLS_CLEAR_HEIGHT, TFT_BLACK);
      drawPreviousIcon(91, CONTROLS_Y, color);
    }
    else if (action == ACTION_PLAY_PAUSE)
    {
      tft.fillRect(142, CONTROLS_CLEAR_Y, 36, CONTROLS_CLEAR_HEIGHT, TFT_BLACK);
      drawPlayPauseIcon(160, CONTROLS_Y, color);
    }
    else if (action == ACTION_NEXT)
    {
      tft.fillRect(211, CONTROLS_CLEAR_Y, 36, CONTROLS_CLEAR_HEIGHT, TFT_BLACK);
      drawNextIcon(229, CONTROLS_Y, color);
    }
  }

  void serviceControlFeedback()
  {
    if (controlFeedbackAction == ACTION_NONE)
      return;

    if ((long)(millis() - controlFeedbackUntil) >= 0)
    {
      int8_t action = controlFeedbackAction;
      controlFeedbackAction = ACTION_NONE;
      drawSinglePlaybackControl(action, HONDA_GREEN);
    }
  }

  void drawPlaybackControls(int8_t activeAction)
  {
    tft.fillRect(4, CONTROLS_CLEAR_Y, 312, CONTROLS_CLEAR_HEIGHT, TFT_BLACK);

    const uint16_t previousColor = activeAction == ACTION_PREVIOUS ? HONDA_PRESS : HONDA_GREEN;
    const uint16_t playColor = activeAction == ACTION_PLAY_PAUSE ? HONDA_BRIGHT : HONDA_GREEN;
    const uint16_t nextColor = activeAction == ACTION_NEXT ? HONDA_PRESS : HONDA_GREEN;

    drawPreviousIcon(91, CONTROLS_Y, previousColor);
    drawPlayPauseIcon(160, CONTROLS_Y, playColor);
    drawNextIcon(229, CONTROLS_Y, nextColor);
  }

  void drawPreviousIcon(int centerX, int centerY, uint16_t color)
  {
    tft.fillRect(centerX - 13, centerY - 10, 3, 20, color);
    tft.fillTriangle(centerX - 10, centerY,
                     centerX + 7, centerY - 10,
                     centerX + 7, centerY + 10,
                     color);
  }

  void drawNextIcon(int centerX, int centerY, uint16_t color)
  {
    tft.fillRect(centerX + 10, centerY - 10, 3, 20, color);
    tft.fillTriangle(centerX + 10, centerY,
                     centerX - 7, centerY - 10,
                     centerX - 7, centerY + 10,
                     color);
  }

  void drawPlayPauseIcon(int centerX, int centerY, uint16_t color)
  {
    if (playbackIsPlaying)
    {
      tft.fillRect(centerX - 8, centerY - 10, 5, 20, color);
      tft.fillRect(centerX + 3, centerY - 10, 5, 20, color);
    }
    else
    {
      tft.fillTriangle(centerX - 7, centerY - 11,
                       centerX - 7, centerY + 11,
                       centerX + 11, centerY,
                       color);
    }
  }

  void playbackCommandTask()
  {
    int8_t action = ACTION_NONE;

    while (true)
    {
      if (xQueueReceive(playbackCommandQueue, &action, portMAX_DELAY) != pdTRUE)
        continue;

      bool commandOk = false;

      if (!spotifyControlReady)
      {
        // Retry the one-time token refresh in the background if startup failed.
        spotifyControlReady = spotifyControl.refreshAccessToken();
      }

      if (spotifyControlReady)
      {
        if (action == ACTION_PREVIOUS)
          commandOk = spotifyControl.previousTrack();
        else if (action == ACTION_NEXT)
          commandOk = spotifyControl.nextTrack();
        else if (action == ACTION_PLAY)
          commandOk = spotifyControl.play();
        else if (action == ACTION_PAUSE)
          commandOk = spotifyControl.pause();
      }

      if (!commandOk && (action == ACTION_PLAY || action == ACTION_PAUSE))
        playPauseRollbackRequested = true;

      if (commandOk)
      {
        // A few quick polls update text/progress as soon as Spotify exposes the
        // new state instead of waiting for the normal five-second interval.
        rapidRefreshesRemaining = 5;
        requestDueTime = millis() + 250;
      }
      else
      {
        requestDueTime = millis() + 800;
      }

      spotifyControlCommandPending = uxQueueMessagesWaiting(playbackCommandQueue) > 0;
    }
  }

  void queueAlbumDownload(const char *albumArtUrl)
  {
    if (albumRequestQueue == nullptr || albumDownloadTaskHandle == nullptr || albumArtUrl == nullptr)
      return;

    AlbumRequest request = {};
    strncpy(request.url, albumArtUrl, sizeof(request.url) - 1);
    request.url[sizeof(request.url) - 1] = '\0';
    request.generation = ++albumRequestGeneration;

    albumDownloadInFlight = true;
    albumStartAllowed = false;
    nextAlbumRetryTime = millis() + albumRetryInterval;

    xQueueOverwrite(albumRequestQueue, &request);
  }

  void albumDownloadTask()
  {
    AlbumRequest request = {};

    while (true)
    {
      while (albumReady || visualizerMode)
        vTaskDelay(pdMS_TO_TICKS(10));

      if (xQueueReceive(albumRequestQueue, &request, portMAX_DELAY) != pdTRUE)
        continue;

      while (visualizerMode || !albumStartAllowed || spotifyControlCommandPending)
        vTaskDelay(pdMS_TO_TICKS(5));
      albumStartAllowed = false;

      char jpegCachePath[64] = {};
      char rawCachePath[64] = {};
      buildAlbumCachePath(request.url, jpegCachePath, sizeof(jpegCachePath));
      buildAlbumRawCachePath(request.url, rawCachePath, sizeof(rawCachePath));

      bool ready = false;
      uint8_t readySource = ALBUM_SOURCE_NONE;
      const char *readyPath = "";

      // Fastest path: a pre-decoded 160x160 RGB565 cache can be pushed to the
      // screen directly, with no JPEG decoding and no SD->SPIFFS copy.
      if (sdCacheReady && isValidRawCache(rawCachePath))
      {
        Serial.print("Raw album cache hit: ");
        Serial.println(rawCachePath);
        ready = true;
        readySource = ALBUM_SOURCE_SD_RAW;
        readyPath = rawCachePath;
      }
      else if (sdCacheReady && SD.exists(jpegCachePath))
      {
        // Old v0.3 JPEG caches stay compatible. They are decoded directly from
        // SD now and converted to the raw fast-cache after the screen updates.
        Serial.print("JPEG album cache hit: ");
        Serial.println(jpegCachePath);
        ready = true;
        readySource = ALBUM_SOURCE_SD_JPEG;
        readyPath = jpegCachePath;
      }
      else
      {
        Serial.print("Album cache miss; downloading: ");
        Serial.println(request.url);
        ready = downloadAlbumToTemp(request.url);
        readySource = ready ? ALBUM_SOURCE_SPIFFS_TEMP : ALBUM_SOURCE_NONE;

        if (ready && sdCacheReady && saveTempToSdCache(jpegCachePath))
        {
          Serial.print("Saved album JPEG to SD cache: ");
          Serial.println(jpegCachePath);
        }
      }

      if (request.generation != albumRequestGeneration)
      {
        if (readySource == ALBUM_SOURCE_SPIFFS_TEMP && SPIFFS.exists(ALBUM_ART_TEMP))
          SPIFFS.remove(ALBUM_ART_TEMP);
        continue;
      }

      if (!ready)
      {
        albumDownloadInFlight = false;
        nextAlbumRetryTime = millis() + albumRetryInterval;
        Serial.println("Background album-art download failed");
        continue;
      }

      strncpy(albumReadyPath, readyPath, sizeof(albumReadyPath) - 1);
      albumReadyPath[sizeof(albumReadyPath) - 1] = '\0';
      albumReadySource = readySource;
      albumReadyGeneration = request.generation;
      albumReady = true;

      // Wait only until the main loop has consumed this cover. The UI remains
      // responsive because all work below stays on the background core.
      while (albumReady && request.generation == albumRequestGeneration)
        vTaskDelay(pdMS_TO_TICKS(10));

      if (request.generation != albumRequestGeneration || !sdCacheReady)
        continue;

      while (visualizerMode && request.generation == albumRequestGeneration)
        vTaskDelay(pdMS_TO_TICKS(15));

      if (request.generation != albumRequestGeneration)
        continue;

      // Convert legacy/new JPEG cache entries to a screen-native file. This is
      // what makes later cache hits genuinely quick rather than merely offline.
      if (!isValidRawCache(rawCachePath) && SD.exists(jpegCachePath))
      {
        Serial.print("Building fast raw cache: ");
        Serial.println(rawCachePath);
        createRawCacheFromJpeg(jpegCachePath, rawCachePath);
      }

      // Once the current cover is safely shown/cached, keep filling a rolling
      // window of queued covers. A track change aborts the old prefetch quickly.
      prefetchQueuedAlbumCovers(request.url, request.generation);
    }
  }

  uint32_t albumUrlHash(const char *text)
  {
    // FNV-1a: small, deterministic and plenty for cache filenames.
    uint32_t hash = 2166136261UL;
    while (text != nullptr && *text)
    {
      hash ^= static_cast<uint8_t>(*text++);
      hash *= 16777619UL;
    }
    return hash;
  }

  void buildAlbumCachePath(const char *url, char *out, size_t outSize)
  {
    snprintf(out, outSize, "%s/%08lx.jpg", ALBUM_CACHE_DIR,
             static_cast<unsigned long>(albumUrlHash(url)));
  }

  void buildAlbumRawCachePath(const char *url, char *out, size_t outSize)
  {
    snprintf(out, outSize, "%s/%08lx.rgb", ALBUM_CACHE_DIR,
             static_cast<unsigned long>(albumUrlHash(url)));
  }

  bool isValidRawCache(const char *rawPath)
  {
    if (!sdCacheReady || rawPath == nullptr || !SD.exists(rawPath))
      return false;

    fs::File file = SD.open(rawPath, FILE_READ);
    if (!file)
      return false;

    RawCacheHeader header = {};
    const size_t read = file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header));
    const size_t expectedSize = sizeof(RawCacheHeader) +
                                (size_t)CACHED_ART_SIZE * CACHED_ART_SIZE * sizeof(uint16_t);
    const bool valid = read == sizeof(header) &&
                       header.magic == RAW_CACHE_MAGIC &&
                       header.width == CACHED_ART_SIZE &&
                       header.height == CACHED_ART_SIZE &&
                       file.size() == expectedSize;
    file.close();
    return valid;
  }

  bool createRawCacheFromJpeg(const char *jpegPath, const char *rawPath)
  {
    if (!sdCacheReady || jpegPath == nullptr || rawPath == nullptr ||
        !SD.exists(jpegPath))
      return false;

    char tempRawPath[72];
    snprintf(tempRawPath, sizeof(tempRawPath), "%s.tmp", rawPath);
    if (SD.exists(tempRawPath))
      SD.remove(tempRawPath);

    cacheRawOutputFile = SD.open(tempRawPath, FILE_WRITE);
    if (!cacheRawOutputFile)
      return false;

    RawCacheHeader header = {RAW_CACHE_MAGIC, CACHED_ART_SIZE, CACHED_ART_SIZE};
    if (cacheRawOutputFile.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) !=
        sizeof(header))
    {
      cacheRawOutputFile.close();
      SD.remove(tempRawPath);
      return false;
    }

    // Pre-size the file so callback seeks do not leave a truncated last block.
    const size_t expectedSize = sizeof(RawCacheHeader) +
                                (size_t)CACHED_ART_SIZE * CACHED_ART_SIZE * sizeof(uint16_t);
    if (!cacheRawOutputFile.seek(expectedSize - 1) || cacheRawOutputFile.write((uint8_t)0) != 1)
    {
      cacheRawOutputFile.close();
      SD.remove(tempRawPath);
      return false;
    }
    cacheRawOutputFile.flush();

    cacheJpegInputFs = &SD;
    int openStatus = cacheJpeg.open(jpegPath, cacheJpegOpen, cacheJpegClose,
                                    cacheJpegRead, cacheJpegSeek, cacheJpegDraw);
    if (openStatus != 1)
    {
      cacheRawOutputFile.close();
      SD.remove(tempRawPath);
      return false;
    }

    cacheJpeg.setPixelType(1);
    int decodeStatus = cacheJpeg.decode(0, 0, JPEG_SCALE_QUARTER);
    cacheJpeg.close();
    cacheRawOutputFile.flush();
    cacheRawOutputFile.close();

    if (decodeStatus != 1)
    {
      SD.remove(tempRawPath);
      return false;
    }

    fs::File verify = SD.open(tempRawPath, FILE_READ);
    const bool validSize = verify && verify.size() == expectedSize;
    if (verify)
      verify.close();
    if (!validSize)
    {
      SD.remove(tempRawPath);
      return false;
    }

    if (SD.exists(rawPath))
      SD.remove(rawPath);
    const bool renamed = SD.rename(tempRawPath, rawPath);
    if (!renamed)
      SD.remove(tempRawPath);
    return renamed;
  }

  bool downloadAlbumDirectToSd(const char *url, const char *cachePath, uint32_t generation)
  {
    if (!sdCacheReady || WiFi.status() != WL_CONNECTED || url == nullptr || cachePath == nullptr)
      return false;

    char tempPath[72];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", cachePath);
    if (SD.exists(tempPath))
      SD.remove(tempPath);

    fs::File file = SD.open(tempPath, FILE_WRITE);
    if (!file)
      return false;

    WiFiClientSecure imageClient;
    imageClient.setInsecure();
    imageClient.setTimeout(8000);

    HTTPClient http;
    http.setTimeout(8000);
    bool success = false;

    if (http.begin(imageClient, url))
    {
      int code = http.GET();
      if (code == HTTP_CODE_OK)
      {
        WiFiClient *stream = http.getStreamPtr();
        int remaining = http.getSize();
        uint8_t buffer[2048];
        size_t total = 0;
        unsigned long lastDataAt = millis();

        while (http.connected() && (remaining > 0 || remaining == -1))
        {
          // A real track change or a button command always wins over prefetch.
          if (visualizerMode || generation != albumRequestGeneration ||
              spotifyControlCommandPending)
            break;

          size_t available = stream->available();
          if (available > 0)
          {
            size_t toRead = min(available, sizeof(buffer));
            int bytesRead = stream->readBytes(buffer, toRead);
            if (bytesRead <= 0)
              break;
            if (file.write(buffer, bytesRead) != (size_t)bytesRead)
              break;

            total += bytesRead;
            if (remaining > 0)
              remaining -= bytesRead;
            lastDataAt = millis();
            taskYIELD();
          }
          else
          {
            if (millis() - lastDataAt > 8000)
              break;
            vTaskDelay(pdMS_TO_TICKS(5));
          }
        }

        file.flush();
        success = total > 0 && !visualizerMode &&
                  generation == albumRequestGeneration &&
                  !spotifyControlCommandPending && (remaining <= 0 || !http.connected());
      }
      else
      {
        Serial.print("Prefetch album HTTP error: ");
        Serial.println(code);
      }
      http.end();
    }

    file.close();
    imageClient.stop();

    if (!success)
    {
      if (SD.exists(tempPath))
        SD.remove(tempPath);
      return false;
    }

    if (SD.exists(cachePath))
      SD.remove(cachePath);
    if (!SD.rename(tempPath, cachePath))
    {
      SD.remove(tempPath);
      return false;
    }
    return true;
  }

  void prefetchQueuedAlbumCovers(const char *currentUrl, uint32_t generation)
  {
    if (!sdCacheReady || visualizerMode ||
        generation != albumRequestGeneration || spotifyControlCommandPending)
      return;

    const size_t queuedCount = spotifyGetQueuedAlbumArtUrls(
        queuedAlbumUrls, QUEUE_PREFETCH_LIMIT);

    if (queuedCount == 0)
      return;

    Serial.print("Queue covers found: ");
    Serial.println(queuedCount);

    for (size_t index = 0; index < queuedCount; index++)
    {
      if (visualizerMode || generation != albumRequestGeneration ||
          spotifyControlCommandPending)
      {
        Serial.println("Queue-cover prefetch stopped for overlay/track/control change");
        return;
      }

      const char *nextUrl = queuedAlbumUrls[index];
      if (nextUrl[0] == '\0' || strcmp(nextUrl, currentUrl) == 0)
        continue;

      char nextJpegPath[64] = {};
      char nextRawPath[64] = {};
      buildAlbumCachePath(nextUrl, nextJpegPath, sizeof(nextJpegPath));
      buildAlbumRawCachePath(nextUrl, nextRawPath, sizeof(nextRawPath));

      // A cache hit must not stop the chain; skip it and continue further ahead.
      if (isValidRawCache(nextRawPath))
      {
        Serial.print("Queued cover already cached: ");
        Serial.println(index + 1);
        continue;
      }

      if (!SD.exists(nextJpegPath))
      {
        Serial.print("Prefetching queued cover ");
        Serial.print(index + 1);
        Serial.print("/");
        Serial.println(queuedCount);

        if (!downloadAlbumDirectToSd(nextUrl, nextJpegPath, generation))
        {
          Serial.println("Queued-cover prefetch cancelled/failed");
          return;
        }
      }

      if (visualizerMode || generation != albumRequestGeneration ||
          spotifyControlCommandPending)
        return;

      if (SD.exists(nextJpegPath) && !isValidRawCache(nextRawPath))
      {
        if (createRawCacheFromJpeg(nextJpegPath, nextRawPath))
          Serial.println("Queued cover ready in fast cache");
      }

      // Be polite to the UI/SD task scheduling between covers.
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    Serial.println("Rolling queue cover cache is filled");
  }

  bool copyStream(fs::File &source, fs::File &destination)
  {
    uint8_t buffer[4096];
    size_t total = 0;

    while (source.available())
    {
      size_t bytesRead = source.read(buffer, sizeof(buffer));
      if (bytesRead == 0)
        break;

      size_t bytesWritten = destination.write(buffer, bytesRead);
      if (bytesWritten != bytesRead)
        return false;
      total += bytesWritten;
      taskYIELD();
    }

    destination.flush();
    return total > 0;
  }

  bool copySdCacheToTemp(const char *cachePath)
  {
    if (SPIFFS.exists(ALBUM_ART_TEMP))
      SPIFFS.remove(ALBUM_ART_TEMP);

    fs::File source = SD.open(cachePath, FILE_READ);
    if (!source)
      return false;

    fs::File destination = SPIFFS.open(ALBUM_ART_TEMP, "w");
    if (!destination)
    {
      source.close();
      return false;
    }

    bool success = copyStream(source, destination);
    source.close();
    destination.close();

    if (!success && SPIFFS.exists(ALBUM_ART_TEMP))
      SPIFFS.remove(ALBUM_ART_TEMP);
    return success;
  }

  bool saveTempToSdCache(const char *cachePath)
  {
    if (!sdCacheReady || !SPIFFS.exists(ALBUM_ART_TEMP))
      return false;

    if (SD.exists(cachePath))
      return true;

    fs::File source = SPIFFS.open(ALBUM_ART_TEMP, "r");
    if (!source)
      return false;

    fs::File destination = SD.open(cachePath, FILE_WRITE);
    if (!destination)
    {
      source.close();
      return false;
    }

    bool success = copyStream(source, destination);
    source.close();
    destination.close();

    if (!success && SD.exists(cachePath))
      SD.remove(cachePath);
    return success;
  }

  bool downloadAlbumToTemp(const char *albumArtUrl)
  {
    if (WiFi.status() != WL_CONNECTED)
      return false;

    if (SPIFFS.exists(ALBUM_ART_TEMP))
      SPIFFS.remove(ALBUM_ART_TEMP);

    fs::File file = SPIFFS.open(ALBUM_ART_TEMP, "w");
    if (!file)
    {
      Serial.println("Could not open album temp file");
      return false;
    }

    WiFiClientSecure imageClient;
    imageClient.setInsecure();
    imageClient.setTimeout(12000);

    HTTPClient http;
    http.setTimeout(12000);

    bool success = false;
    if (http.begin(imageClient, albumArtUrl))
    {
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK)
      {
        WiFiClient *stream = http.getStreamPtr();
        int remaining = http.getSize();
        uint8_t buffer[2048];
        size_t total = 0;
        unsigned long lastDataAt = millis();

        while (!visualizerMode && http.connected() &&
               (remaining > 0 || remaining == -1))
        {
          const size_t available = stream->available();
          if (available > 0)
          {
            const size_t toRead = min(available, sizeof(buffer));
            const int bytesRead = stream->readBytes(buffer, toRead);
            if (bytesRead <= 0 || file.write(buffer, bytesRead) != (size_t)bytesRead)
              break;

            total += bytesRead;
            if (remaining > 0)
              remaining -= bytesRead;
            lastDataAt = millis();
            taskYIELD();
          }
          else
          {
            if (millis() - lastDataAt > 8000)
              break;
            vTaskDelay(pdMS_TO_TICKS(5));
          }
        }

        file.flush();
        success = !visualizerMode && total > 0 && file.size() > 0 &&
                  (remaining <= 0 || !http.connected());

        Serial.print("Album-art bytes: ");
        Serial.println(total);
      }
      else
      {
        Serial.print("Album-art HTTP error: ");
        Serial.println(httpCode);
      }
      http.end();
    }
    else
    {
      Serial.println("Could not begin album-art HTTPS request");
    }

    file.close();
    imageClient.stop();

    if (!success && SPIFFS.exists(ALBUM_ART_TEMP))
      SPIFFS.remove(ALBUM_ART_TEMP);

    return success;
  }

  int drawImagefromFs(fs::FS &sourceFs, const char *imageFileUri)
  {
    unsigned long started = millis();
    displayJpegFs = &sourceFs;

    int openStatus = jpeg.open((const char *)imageFileUri, myOpen, myClose, myRead, mySeek, JPEGDraw);
    if (openStatus != 1)
    {
      Serial.println("JPEG open failed");
      displayJpegFs = &SPIFFS;
      return 0;
    }

    jpeg.setPixelType(1);
    int decodeStatus = jpeg.decode(IMAGE_X, IMAGE_Y, JPEG_SCALE_QUARTER);
    jpeg.close();
    displayJpegFs = &SPIFFS;
    tft.drawRect(IMAGE_X - 1, IMAGE_Y - 1, IMAGE_SIZE + 2, IMAGE_SIZE + 2, HONDA_GREEN);

    Serial.print("Time taken to decode and display JPEG (ms): ");
    Serial.println(millis() - started);
    return decodeStatus;
  }

  bool drawRawCacheFile(const char *rawPath)
  {
    if (!isValidRawCache(rawPath))
      return false;

    unsigned long started = millis();
    fs::File file = SD.open(rawPath, FILE_READ);
    if (!file)
      return false;

    RawCacheHeader header = {};
    if (file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header))
    {
      file.close();
      return false;
    }

    static uint16_t pixelRows[CACHED_ART_SIZE * 4];
    int y = 0;
    while (y < CACHED_ART_SIZE)
    {
      const int rows = min(4, CACHED_ART_SIZE - y);
      const size_t bytesNeeded = (size_t)CACHED_ART_SIZE * rows * sizeof(uint16_t);
      if (file.read(reinterpret_cast<uint8_t *>(pixelRows), bytesNeeded) != bytesNeeded)
      {
        file.close();
        return false;
      }

      tft.pushImage(IMAGE_X, IMAGE_Y + y, CACHED_ART_SIZE, rows, pixelRows);
      y += rows;
    }

    file.close();
    tft.drawRect(IMAGE_X - 1, IMAGE_Y - 1, IMAGE_SIZE + 2, IMAGE_SIZE + 2, HONDA_GREEN);
    Serial.print("Time taken to display RAW cache (ms): ");
    Serial.println(millis() - started);
    return true;
  }

  bool drawCurrentAlbumArt()
  {
    if (overlayModeActive())
      return false;

    clearImage();
    bool drawn = false;

    if (currentAlbumSource == ALBUM_SOURCE_SD_RAW && currentAlbumPath[0] != '\0')
      drawn = drawRawCacheFile(currentAlbumPath);
    else if (currentAlbumSource == ALBUM_SOURCE_SD_JPEG && currentAlbumPath[0] != '\0')
      drawn = drawImagefromFs(SD, currentAlbumPath) == 1;
    else if (currentAlbumSource == ALBUM_SOURCE_SPIFFS_CURRENT && currentAlbumPath[0] != '\0')
      drawn = drawImagefromFs(SPIFFS, currentAlbumPath) == 1;

    // Backward-compatible startup fallback for the most recently downloaded cover.
    if (!drawn && currentAlbumSource == ALBUM_SOURCE_NONE && SPIFFS.exists(ALBUM_ART))
    {
      currentAlbumSource = ALBUM_SOURCE_SPIFFS_CURRENT;
      strncpy(currentAlbumPath, ALBUM_ART, sizeof(currentAlbumPath) - 1);
      currentAlbumPath[sizeof(currentAlbumPath) - 1] = '\0';
      drawn = drawImagefromFs(SPIFFS, ALBUM_ART) == 1;
    }

    return drawn;
  }

};
