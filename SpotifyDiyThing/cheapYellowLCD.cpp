#include "cheapYellowLCD.h"

#include <SD.h>
#include <WiFi.h>
#include <time.h>

#include "spotifyLogic.h"
#include "timing.h"
#include "version.h"

// The one TFT_eSPI instance for the whole firmware (declared in cydTheme.h).
TFT_eSPI tft = TFT_eSPI();

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void CheapYellowDisplay::displaySetup(SpotifyArduino *spotifyObj)
{
  spotify_display = spotifyObj;
  _albumArtUrl[0] = '\0';

  touchSetup();

  Serial.println(F(CARDISPLAY_BUILD_TAG " - display setup"));
  setWidth(layout::SCREEN_WIDTH);
  setHeight(layout::SCREEN_HEIGHT);
  setImageHeight(layout::IMAGE_SIZE);
  setImageWidth(layout::IMAGE_SIZE);

  tft.init();
  tft.setRotation(1);
  tft.setTextWrap(false);

  // Backlight comes up at the stored level, so a dark cabin never gets a
  // full-brightness flash. Coordinates drive the automatic sunset dim.
  backlight.begin(TFT_BL, deviceConfig.latitude, deviceConfig.longitude);

  tft.fillScreen(TFT_BLACK);

  visualizer.begin();

  albumCache.begin();
  if (albumCache.sdReady())
  {
    japaneseFontReady = japaneseFont.begin(SD);
    Serial.println(japaneseFontReady ? F("Installed SD Japanese font ready")
                                     : F("Japanese font not installed yet"));
  }

  playbackCommandQueue = xQueueCreate(4, sizeof(int8_t));
  if (playbackCommandQueue == nullptr)
  {
    Serial.println(F("Failed to create playback-command queue"));
    return;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      playbackCommandTaskTrampoline, "spotifyControls", 6144, this, 3,
      &playbackCommandTaskHandle, 0);

  if (result != pdPASS)
  {
    Serial.println(F("Failed to start playback-command task"));
    playbackCommandTaskHandle = nullptr;
  }
}

const char *CheapYellowDisplay::safeText(const char *text)
{
  return (text == nullptr || text[0] == '\0') ? "-" : text;
}

// ---------------------------------------------------------------------------
// Player screen
// ---------------------------------------------------------------------------

void CheapYellowDisplay::showDefaultScreen()
{
  drawPlayerShell();

  if (!drawAlbumArt())
    drawAlbumPlaceholder();

  drawTrackPlaceholder();
  displayTrackProgress(0, 1);
  drawPlaybackControls(Action::None);
}

void CheapYellowDisplay::ensurePlayerShell()
{
  if (!playerShellDrawn)
    drawPlayerShell();
}

void CheapYellowDisplay::drawPlayerShell()
{
  wifiSetupMode = false;
  clockMode = false;
  setTouchSetupPortalMode(false);
  setTouchClockMode(false);
  setTouchVisualizerMode(false);
  if (visualizer.isOpen())
    visualizer.close();
  albumCache.setPaused(false);

  tft.fillScreen(TFT_BLACK);
  tft.drawRect(3, 3, 314, 234, theme::DARK);
  tft.drawFastHLine(8, layout::HEADER_HEIGHT, 304, theme::DARK);
  tft.drawRect(layout::IMAGE_X - 1, layout::IMAGE_Y - 1,
               layout::IMAGE_SIZE + 2, layout::IMAGE_SIZE + 2, theme::GREEN);
  drawHeader(true);
  drawPlaybackControls(Action::None);

  lastProgressBarWidth = -1;
  lastProgressDuration = -1;
  lastElapsedText[0] = '\0';
  lastDurationText[0] = '\0';
  playerShellDrawn = true;
}

void CheapYellowDisplay::restorePlayerScreen()
{
  playerShellDrawn = false;
  drawPlayerShell();

  if (!drawAlbumArt())
    drawAlbumPlaceholder();

  if (cachedTrackValid)
    drawCachedTrackInfo();
  else
    drawTrackPlaceholder();

  displayTrackProgress(cachedProgress, cachedDuration);
  drawPlaybackControls(Action::None);
}

void CheapYellowDisplay::drawHeader(bool force)
{
  char clockText[6] = "--:--";
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 5))
    strftime(clockText, sizeof(clockText), "%H:%M", &timeInfo);

  if (!force && strcmp(clockText, lastClockText) == 0)
    return;

  copyField(lastClockText, clockText);

  tft.fillRect(5, 5, 310, layout::HEADER_HEIGHT - 6, TFT_BLACK);
  drawSpotifyIcon(17, 16, 9, theme::GREEN);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawString("Spotify", 31, 8, 2);

  const int clockWidth = tft.textWidth(clockText, 2);
  tft.drawString(clockText, layout::CENTRE_X - (clockWidth / 2), 8, 2);

  drawWifiIndicator();
  tft.drawFastHLine(8, layout::HEADER_HEIGHT, 304, theme::DARK);
}

void CheapYellowDisplay::drawSpotifyIcon(int centerX, int centerY, int radius, uint16_t color)
{
  tft.drawCircle(centerX, centerY, radius, color);
  tft.drawLine(centerX - 5, centerY - 3, centerX + 5, centerY - 2, color);
  tft.drawLine(centerX - 4, centerY, centerX + 4, centerY + 1, color);
  tft.drawLine(centerX - 3, centerY + 3, centerX + 3, centerY + 4, color);
}

void CheapYellowDisplay::drawWifiIndicator()
{
  const uint16_t color = WiFi.status() == WL_CONNECTED ? theme::GREEN : theme::DIM;
  constexpr int baseX = 287;
  constexpr int baseY = 23;

  for (int i = 0; i < 4; i++)
  {
    const int height = 4 + (i * 3);
    tft.fillRect(baseX + (i * 6), baseY - height, 4, height, color);
  }
}

void CheapYellowDisplay::drawAlbumPlaceholder()
{
  clearImage();
  const int centerX = layout::IMAGE_X + layout::IMAGE_SIZE / 2;
  const int centerY = layout::IMAGE_Y + 70;
  tft.drawCircle(centerX, centerY, 43, theme::DARK);
  tft.drawCircle(centerX, centerY, 36, theme::DIM);
  tft.drawCircle(centerX, centerY, 27, theme::DARK);
  tft.fillCircle(centerX, centerY, 8, theme::GREEN);
  tft.fillCircle(centerX, centerY, 3, TFT_BLACK);
  tft.drawLine(centerX + 27, centerY - 29, centerX + 42, centerY - 39, theme::GREEN);
  tft.drawLine(centerX + 42, centerY - 39, centerX + 48, centerY - 31, theme::GREEN);
  tft.fillCircle(centerX + 27, centerY - 29, 3, theme::GREEN);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString("NOW PLAYING", centerX, layout::IMAGE_Y + 124, 2);
}

void CheapYellowDisplay::drawTrackPlaceholder()
{
  tft.fillRect(layout::TEXT_X, layout::CONTENT_Y,
               layout::TEXT_WIDTH, layout::CONTENT_HEIGHT, TFT_BLACK);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawString("NOW PLAYING", layout::TEXT_X, layout::CONTENT_Y + 3, 2);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawString("Waiting for", layout::TEXT_X, layout::CONTENT_Y + 33, 2);
  tft.drawString("Spotify...", layout::TEXT_X, layout::CONTENT_Y + 53, 2);
}

void CheapYellowDisplay::drawCachedTrackInfo()
{
  tft.fillRect(layout::TEXT_X, layout::CONTENT_Y,
               layout::TEXT_WIDTH, layout::CONTENT_HEIGHT, TFT_BLACK);

  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawString("NOW PLAYING", layout::TEXT_X, layout::CONTENT_Y + 3, 2);

  const int titleFont = tft.textWidth(cachedTrackName, 4) <= layout::TEXT_WIDTH ? 4 : 2;
  drawEllipsized(cachedTrackName, layout::TEXT_X, layout::CONTENT_Y + 23,
                 layout::TEXT_WIDTH, titleFont, theme::GREEN);
  drawEllipsized(cachedArtistName, layout::TEXT_X, layout::CONTENT_Y + 61,
                 layout::TEXT_WIDTH, 2, theme::GREEN);
  drawEllipsized(cachedAlbumName, layout::TEXT_X, layout::CONTENT_Y + 82,
                 layout::TEXT_WIDTH, 2, theme::DIM);
}

void CheapYellowDisplay::drawEllipsized(const char *text, int x, int y, int maxWidth,
                                        int font, uint16_t color)
{
  if (japaneseFontReady && SdJapaneseFont::containsNonAscii(text))
  {
    japaneseFont.drawText(tft, safeText(text), x, y, maxWidth, color, TFT_BLACK);
    return;
  }

  char buffer[101];
  copyField(buffer, safeText(text));

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

void CheapYellowDisplay::printCurrentlyPlayingToScreen(CurrentlyPlaying currentlyPlaying)
{
  playbackIsPlaying = currentlyPlaying.isPlaying;

  copyField(cachedTrackName, safeText(currentlyPlaying.trackName));
  copyField(cachedArtistName,
            currentlyPlaying.numArtists > 0
                ? safeText(currentlyPlaying.artists[0].artistName)
                : "Unknown Artist");
  copyField(cachedAlbumName, safeText(currentlyPlaying.albumName));
  cachedTrackValid = true;

  if (overlayModeActive())
    return;

  ensurePlayerShell();
  drawCachedTrackInfo();
  drawPlaybackControls(Action::None);
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

void CheapYellowDisplay::formatDuration(long milliseconds, char *out, size_t outSize)
{
  const long totalSeconds = milliseconds / 1000;
  snprintf(out, outSize, "%ld:%02ld", totalSeconds / 60, totalSeconds % 60);
}

void CheapYellowDisplay::displayTrackProgress(long progress, long duration)
{
  // Keep the latest state even while a full-screen overlay covers the UI.
  cachedProgress = progress;
  cachedDuration = duration > 0 ? duration : 1;

  if (overlayModeActive())
    return;

  ensurePlayerShell();

  if (duration <= 0)
    duration = 1;

  progress = constrain(progress, 0L, duration);
  const int lineWidth = map(progress, 0, duration, 0, layout::PROGRESS_WIDTH);

  // Thin Car-Thing style progress line: a visible dark track with a bright
  // played section. Only changed pixels are repainted to avoid flicker.
  if (lastProgressBarWidth < 0 || duration != lastProgressDuration)
  {
    tft.fillRect(layout::PROGRESS_X, layout::PROGRESS_Y,
                 layout::PROGRESS_WIDTH, layout::PROGRESS_HEIGHT, theme::PROGRESS_TRACK);
    lastProgressBarWidth = 0;
    lastProgressDuration = duration;
  }

  if (lineWidth > lastProgressBarWidth)
  {
    tft.fillRect(layout::PROGRESS_X + lastProgressBarWidth, layout::PROGRESS_Y,
                 lineWidth - lastProgressBarWidth, layout::PROGRESS_HEIGHT, theme::GREEN);
  }
  else if (lineWidth < lastProgressBarWidth)
  {
    tft.fillRect(layout::PROGRESS_X + lineWidth, layout::PROGRESS_Y,
                 lastProgressBarWidth - lineWidth, layout::PROGRESS_HEIGHT,
                 theme::PROGRESS_TRACK);
  }

  // Elapsed and total time sit above the right end of the line. Redraw that
  // small area only when the displayed second actually changes.
  char elapsedText[8];
  char durationText[8];
  formatDuration(progress, elapsedText, sizeof(elapsedText));
  formatDuration(duration, durationText, sizeof(durationText));

  if (strcmp(elapsedText, lastElapsedText) != 0 ||
      strcmp(durationText, lastDurationText) != 0)
  {
    copyField(lastElapsedText, elapsedText);
    copyField(lastDurationText, durationText);

    char progressTimeText[20];
    snprintf(progressTimeText, sizeof(progressTimeText), "%s / %s",
             elapsedText, durationText);

    tft.fillRect(layout::PROGRESS_TIME_X, layout::PROGRESS_TIME_Y,
                 layout::PROGRESS_TIME_WIDTH, layout::PROGRESS_TIME_HEIGHT, TFT_BLACK);
    tft.setTextColor(theme::DIM, TFT_BLACK);
    tft.drawRightString(progressTimeText,
                        layout::PROGRESS_X + layout::PROGRESS_WIDTH,
                        layout::PROGRESS_TIME_Y, 2);
  }

  lastProgressBarWidth = lineWidth;
}

// ---------------------------------------------------------------------------
// Playback controls
// ---------------------------------------------------------------------------

void CheapYellowDisplay::setPlaybackState(bool isPlaying)
{
  if (playbackIsPlaying == isPlaying)
    return;

  playbackIsPlaying = isPlaying;
  if (playerShellDrawn && !overlayModeActive())
    drawPlaybackControls(Action::None);
}

void CheapYellowDisplay::drawPreviousIcon(int centerX, int centerY, uint16_t color)
{
  tft.fillRect(centerX - 13, centerY - 10, 3, 20, color);
  tft.fillTriangle(centerX - 10, centerY,
                   centerX + 7, centerY - 10,
                   centerX + 7, centerY + 10, color);
}

void CheapYellowDisplay::drawNextIcon(int centerX, int centerY, uint16_t color)
{
  tft.fillRect(centerX + 10, centerY - 10, 3, 20, color);
  tft.fillTriangle(centerX + 10, centerY,
                   centerX - 7, centerY - 10,
                   centerX - 7, centerY + 10, color);
}

void CheapYellowDisplay::drawPlayPauseIcon(int centerX, int centerY, uint16_t color)
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
                     centerX + 11, centerY, color);
  }
}

void CheapYellowDisplay::drawSinglePlaybackControl(Action action, uint16_t color)
{
  switch (action)
  {
  case Action::Previous:
    tft.fillRect(73, layout::CONTROLS_CLEAR_Y, 36, layout::CONTROLS_CLEAR_HEIGHT, TFT_BLACK);
    drawPreviousIcon(91, layout::CONTROLS_Y, color);
    break;
  case Action::PlayPause:
    tft.fillRect(142, layout::CONTROLS_CLEAR_Y, 36, layout::CONTROLS_CLEAR_HEIGHT, TFT_BLACK);
    drawPlayPauseIcon(160, layout::CONTROLS_Y, color);
    break;
  case Action::Next:
    tft.fillRect(211, layout::CONTROLS_CLEAR_Y, 36, layout::CONTROLS_CLEAR_HEIGHT, TFT_BLACK);
    drawNextIcon(229, layout::CONTROLS_Y, color);
    break;
  default:
    break;
  }
}

void CheapYellowDisplay::drawPlaybackControls(Action activeAction)
{
  tft.fillRect(4, layout::CONTROLS_CLEAR_Y, 312, layout::CONTROLS_CLEAR_HEIGHT, TFT_BLACK);

  drawPreviousIcon(91, layout::CONTROLS_Y,
                   activeAction == Action::Previous ? theme::PRESS : theme::GREEN);
  drawPlayPauseIcon(160, layout::CONTROLS_Y,
                    activeAction == Action::PlayPause ? theme::BRIGHT : theme::GREEN);
  drawNextIcon(229, layout::CONTROLS_Y,
               activeAction == Action::Next ? theme::PRESS : theme::GREEN);
}

void CheapYellowDisplay::serviceControlFeedback()
{
  if (controlFeedbackAction == Action::None)
    return;

  if (timeReached(controlFeedbackUntil))
  {
    const Action action = controlFeedbackAction;
    controlFeedbackAction = Action::None;
    drawSinglePlaybackControl(action, theme::GREEN);
  }
}

void CheapYellowDisplay::handlePlaybackTouch(Action action)
{
  // Playback controls are hidden and disabled under either overlay.
  if (overlayModeActive())
    return;

  // Optimistic feedback: the icon reacts immediately and the HTTPS request is
  // sent by a dedicated high-priority task. The screen loop never waits for it.
  bool toggledPlayState = false;
  Action command = action;
  if (action == Action::PlayPause)
  {
    playbackIsPlaying = !playbackIsPlaying;
    toggledPlayState = true;
    command = playbackIsPlaying ? Action::Play : Action::Pause;
  }

  // Restore any previously highlighted icon before showing the new tap.
  if (controlFeedbackAction != Action::None && controlFeedbackAction != action)
    drawSinglePlaybackControl(controlFeedbackAction, theme::GREEN);

  drawSinglePlaybackControl(
      action,
      (action == Action::Previous || action == Action::Next) ? theme::PRESS : theme::BRIGHT);
  controlFeedbackAction = action;
  controlFeedbackUntil = deadlineIn(CONTROL_FEEDBACK_MS);

  bool queued = false;
  if (playbackCommandQueue != nullptr && playbackCommandTaskHandle != nullptr)
  {
    spotifyControlCommandPending = true;
    const int8_t raw = static_cast<int8_t>(command);
    queued = xQueueSend(playbackCommandQueue, &raw, 0) == pdTRUE;
    if (!queued)
      spotifyControlCommandPending = uxQueueMessagesWaiting(playbackCommandQueue) > 0;
  }

  if (queued)
    return;

  // Very unlikely fallback if the task could not be created.
  bool commandOk = false;
  switch (command)
  {
  case Action::Previous:
    commandOk = spotify_display->previousTrack();
    break;
  case Action::Next:
    commandOk = spotify_display->nextTrack();
    break;
  case Action::Play:
    commandOk = spotify_display->play();
    break;
  case Action::Pause:
    commandOk = spotify_display->pause();
    break;
  default:
    break;
  }

  if (!commandOk && toggledPlayState)
    playbackIsPlaying = !playbackIsPlaying;

  requestRapidRefresh(commandOk ? 5 : 0, commandOk ? 250 : 1000);
}

void CheapYellowDisplay::playbackCommandTaskTrampoline(void *parameter)
{
  static_cast<CheapYellowDisplay *>(parameter)->playbackCommandTask();
}

void CheapYellowDisplay::playbackCommandTask()
{
  int8_t raw = 0;

  while (true)
  {
    if (xQueueReceive(playbackCommandQueue, &raw, portMAX_DELAY) != pdTRUE)
      continue;

    const Action action = static_cast<Action>(raw);
    bool commandOk = false;

    if (!spotifyControlReady)
    {
      // Retry the one-time token refresh in the background if startup failed.
      spotifyControlReady = spotifyControl.refreshAccessToken();
    }

    if (spotifyControlReady)
    {
      switch (action)
      {
      case Action::Previous:
        commandOk = spotifyControl.previousTrack();
        break;
      case Action::Next:
        commandOk = spotifyControl.nextTrack();
        break;
      case Action::Play:
        commandOk = spotifyControl.play();
        break;
      case Action::Pause:
        commandOk = spotifyControl.pause();
        break;
      default:
        break;
      }
    }

    if (!commandOk && (action == Action::Play || action == Action::Pause))
      playPauseRollbackRequested = true;

    // A few quick polls update text and progress as soon as Spotify exposes the
    // new state, instead of waiting out the normal five-second interval.
    if (commandOk)
      requestRapidRefresh(5, 250);
    else
      scheduleNextPoll(800);

    spotifyControlCommandPending = uxQueueMessagesWaiting(playbackCommandQueue) > 0;
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void CheapYellowDisplay::checkForInput()
{
  if (!timeReached(touchCooldownUntil))
    return;

  const TouchAction action = takeTouchAction();
  if (action == TouchAction::None)
    return;

  touchCooldownUntil = deadlineIn(TOUCH_COOLDOWN_MS);

  switch (action)
  {
  case TouchAction::CycleBrightness:
    cycleBrightness();
    break;
  case TouchAction::NextVisualizerStyle:
    visualizer.nextStyle();
    break;
  case TouchAction::ToggleVisualizer:
    toggleVisualizerMode();
    break;
  case TouchAction::OpenOfflineVisualizer:
    if (wifiSetupMode && !visualizer.isOpen())
    {
      setTouchSetupPortalMode(false);
      toggleVisualizerMode();
    }
    break;
  case TouchAction::ToggleClock:
    toggleClockMode();
    break;
  case TouchAction::Previous:
    handlePlaybackTouch(Action::Previous);
    break;
  case TouchAction::PlayPause:
    handlePlaybackTouch(Action::PlayPause);
    break;
  case TouchAction::Next:
    handlePlaybackTouch(Action::Next);
    break;
  case TouchAction::None:
    break;
  }
}

// ---------------------------------------------------------------------------
// Per-loop service
// ---------------------------------------------------------------------------

void CheapYellowDisplay::service()
{
  serviceClock();
  serviceBrightness();

  if (clockMode)
    serviceFullscreenClock();
  else if (visualizer.isOpen())
    visualizer.service();
  else
    serviceControlFeedback();

  if (playPauseRollbackRequested)
  {
    playPauseRollbackRequested = false;
    playbackIsPlaying = !playbackIsPlaying;
    if (!overlayModeActive())
      drawSinglePlaybackControl(Action::PlayPause, theme::GREEN);
  }

  serviceAlbumArt();
}

void CheapYellowDisplay::serviceAlbumArt()
{
  // The visualizer owns the render loop while it is open. Deferring the album
  // hand-off keeps SD and network work out of the animation. Everything resumes
  // immediately after the overlay closes.
  if (visualizer.isOpen())
    return;

  switch (albumCache.serviceHandoff())
  {
  case AlbumArtCache::Handoff::Ready:
    if (overlayModeActive())
    {
      // Keep the newest cover ready, but do not draw over the clock overlay.
      albumDisplayed = false;
    }
    else
    {
      albumDisplayed = drawAlbumArt();
    }
    break;

  case AlbumArtCache::Handoff::Failed:
    albumDisplayed = false;
    break;

  default:
    break;
  }
}

// ---------------------------------------------------------------------------
// Album art
// ---------------------------------------------------------------------------

void CheapYellowDisplay::clearImage()
{
  if (overlayModeActive())
    return;

  tft.fillRect(layout::IMAGE_X, layout::IMAGE_Y,
               layout::IMAGE_SIZE, layout::IMAGE_SIZE, TFT_BLACK);
  tft.drawRect(layout::IMAGE_X - 1, layout::IMAGE_Y - 1,
               layout::IMAGE_SIZE + 2, layout::IMAGE_SIZE + 2, theme::GREEN);
}

bool CheapYellowDisplay::drawAlbumArt()
{
  if (overlayModeActive())
    return false;

  clearImage();
  return albumCache.drawCurrent();
}

boolean CheapYellowDisplay::processImageInfo(CurrentlyPlaying currentlyPlaying)
{
  if (currentlyPlaying.numImages <= 0)
    return false;

  // Spotify returns images largest-first. Use the 640 px cover so
  // JPEG_SCALE_QUARTER produces a crisp 160 x 160 image.
  const SpotifyImage image = currentlyPlaying.albumImages[0];
  if (image.url == nullptr || image.url[0] == '\0')
    return false;

  if (!isSameAlbum(image.url))
  {
    setImageHeight(layout::IMAGE_SIZE);
    setImageWidth(layout::IMAGE_SIZE);
    setAlbumArtUrl(image.url);
    albumDisplayed = false;
    albumCache.request(image.url);
  }
  else if (!albumDisplayed && !albumCache.downloadInFlight() &&
           !albumCache.handoffPending() && albumCache.retryDue())
  {
    albumCache.request(image.url);
  }

  // Returning false keeps spotifyLogic off its old blocking path.
  return false;
}

int CheapYellowDisplay::displayImage()
{
  if (overlayModeActive())
    return 0;
  albumDisplayed = drawAlbumArt();
  return albumDisplayed ? 1 : 0;
}

void CheapYellowDisplay::markDisplayAsTagRead()
{
  if (overlayModeActive())
    return;
  tft.drawRect(layout::IMAGE_X - 2, layout::IMAGE_Y - 2,
               layout::IMAGE_SIZE + 4, layout::IMAGE_SIZE + 4, TFT_BLUE);
}

void CheapYellowDisplay::markDisplayAsTagWritten()
{
  if (overlayModeActive())
    return;
  tft.drawRect(layout::IMAGE_X - 2, layout::IMAGE_Y - 2,
               layout::IMAGE_SIZE + 4, layout::IMAGE_SIZE + 4, theme::GREEN);
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

void CheapYellowDisplay::serviceClock()
{
  if (!clockConfigured && WiFi.status() == WL_CONNECTED)
  {
    // Timezone comes from the device config, so the header clock is right
    // wherever the car is rather than pinned to one city at compile time.
    configTzTime(deviceConfig.timezone, "pool.ntp.org", "time.nist.gov");
    clockConfigured = true;
    lastClockDrawTime = 0;
  }

  if (overlayModeActive() || !playerShellDrawn || !elapsed(lastClockDrawTime, CLOCK_REDRAW_MS))
    return;

  lastClockDrawTime = millis();
  drawHeader();
}

void CheapYellowDisplay::toggleClockMode()
{
  clockMode = !clockMode;
  if (visualizer.isOpen())
  {
    visualizer.close();
    albumCache.setPaused(false);
  }
  setTouchVisualizerMode(false);
  setTouchClockMode(clockMode);
  controlFeedbackAction = Action::None;

  if (clockMode)
  {
    // Spotify polling and album downloads continue in the background; only the
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

void CheapYellowDisplay::serviceFullscreenClock()
{
  if (!clockMode || !elapsed(lastFullscreenClockDrawTime, FULLSCREEN_CLOCK_REDRAW_MS))
    return;

  lastFullscreenClockDrawTime = millis();
  drawFullscreenClock(false);
}

void CheapYellowDisplay::drawFullscreenClock(bool force)
{
  char clockText[6] = "--:--";
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 5))
    strftime(clockText, sizeof(clockText), "%H:%M", &timeInfo);

  if (!force && strcmp(clockText, lastFullscreenClockText) == 0)
    return;

  copyField(lastFullscreenClockText, clockText);

  // Intentionally minimal: pure black background and one large green clock.
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString(clockText, layout::CENTRE_X, 78, 8);
  drawOverlayCloseButton();
}

// ---------------------------------------------------------------------------
// Visualizer overlay
// ---------------------------------------------------------------------------

void CheapYellowDisplay::toggleVisualizerMode()
{
  const bool opening = !visualizer.isOpen();
  clockMode = false;
  setTouchClockMode(false);
  controlFeedbackAction = Action::None;

  if (opening)
  {
    setTouchSetupPortalMode(false);
    setTouchVisualizerMode(true);
    playerShellDrawn = false;
    albumCache.setPaused(true);
    visualizer.open();
    return;
  }

  visualizer.close();
  setTouchVisualizerMode(false);
  albumCache.setPaused(false);

  if (wifiSetupMode)
  {
    setTouchSetupPortalMode(true);
    drawWifiSetupScreen();
    return;
  }

  if (albumCache.downloadInFlight())
    albumCache.allowNetworkStart();
  scheduleNextPoll(0);
  restorePlayerScreen();
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

void CheapYellowDisplay::cycleBrightness()
{
  backlight.cycleManual();
  drawBrightnessToast();
}

void CheapYellowDisplay::drawBrightnessToast()
{
  if (overlayModeActive() || !playerShellDrawn)
    return;

  char value[8];
  snprintf(value, sizeof(value), "%u%%", backlight.effectivePercent());
  tft.fillRect(5, 5, 92, layout::HEADER_HEIGHT - 6, TFT_BLACK);
  tft.setTextColor(theme::BRIGHT, TFT_BLACK);
  tft.drawCentreString(value, 49, 8, 2);
  brightnessToastVisible = true;
  brightnessToastUntil = deadlineIn(BRIGHTNESS_TOAST_MS);
}

void CheapYellowDisplay::serviceBrightness()
{
  if (backlight.service())
    drawBrightnessToast();

  if (brightnessToastVisible && timeReached(brightnessToastUntil))
  {
    brightnessToastVisible = false;
    if (!overlayModeActive() && playerShellDrawn)
      drawHeader(true);
  }
}

// ---------------------------------------------------------------------------
// Setup screens
// ---------------------------------------------------------------------------

void CheapYellowDisplay::drawWifiManagerMessage(WiFiManager *myWiFiManager)
{
  wifiSetupMode = true;
  copyField(wifiSetupSsid, myWiFiManager->getConfigPortalSSID().c_str());
  copyField(wifiSetupIp, WiFi.softAPIP().toString().c_str());

  clockMode = false;
  if (visualizer.isOpen())
    visualizer.close();
  albumCache.setPaused(false);
  setTouchClockMode(false);
  setTouchVisualizerMode(false);
  setTouchSetupPortalMode(true);

  Serial.println(F("Entered config mode"));
  drawWifiSetupScreen();
}

void CheapYellowDisplay::drawWifiSetupScreen()
{
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(3, 3, 314, 234, theme::GREEN);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString("WI-FI SETUP", layout::CENTRE_X, 8, 4);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString("Connect phone or use offline mode", layout::CENTRE_X, 39, 2);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawString("Network:", 18, 68, 2);
  tft.drawString(wifiSetupSsid, 104, 68, 2);
  tft.drawString("Password:", 18, 91, 2);
  tft.drawString("thing123", 104, 91, 2);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawString("Setup address:", 18, 120, 2);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString(wifiSetupIp, layout::CENTRE_X, 139, 4);
  tft.drawRect(32, 176, 256, 56, theme::VIZ_GLOW);
  tft.drawRect(34, 178, 252, 52, theme::VIZ_MID);
  tft.setTextColor(theme::VIZ_BRIGHT, TFT_BLACK);
  tft.drawCentreString("OFFLINE VISUALIZER", layout::CENTRE_X, 186, 2);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString("MIC MODE - NO INTERNET", layout::CENTRE_X, 207, 1);
}

void CheapYellowDisplay::drawRefreshTokenMessage()
{
  wifiSetupMode = false;
  clockMode = false;
  if (visualizer.isOpen())
    visualizer.close();
  albumCache.setPaused(false);
  setTouchSetupPortalMode(false);
  setTouchClockMode(false);
  setTouchVisualizerMode(false);

  Serial.println(F("Refresh token mode"));
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(3, 3, 314, 234, theme::GREEN);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString("SPOTIFY AUTH", layout::CENTRE_X, 16, 4);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString("Open the address below", layout::CENTRE_X, 66, 2);
  tft.drawCentreString("and authorize this device", layout::CENTRE_X, 86, 2);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString(WiFi.localIP().toString(), layout::CENTRE_X, 128, 4);
}

void CheapYellowDisplay::serviceConfigPortal()
{
  checkForInput();
  service();
}

void CheapYellowDisplay::finishConfigPortal()
{
  wifiSetupMode = false;
  setTouchSetupPortalMode(false);
  setTouchVisualizerMode(false);
  visualizer.close();
  albumCache.setPaused(false);
  playerShellDrawn = false;
}

// ---------------------------------------------------------------------------
// Startup Wi-Fi animation
// ---------------------------------------------------------------------------

void CheapYellowDisplay::drawWiFiConnectingBase()
{
  clockMode = false;
  if (visualizer.isOpen())
    visualizer.close();
  setTouchClockMode(false);
  setTouchVisualizerMode(false);
  playerShellDrawn = false;

  tft.fillScreen(TFT_BLACK);
  tft.drawRect(3, 3, 314, 234, theme::DARK);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString("CONNECTING", layout::CENTRE_X, 72, 4);
}

void CheapYellowDisplay::drawWiFiAnimationFrame(uint8_t frame)
{
  static const int dotX[4] = {124, 148, 172, 196};
  const uint8_t active = frame % 4;

  tft.fillRect(108, 122, 104, 28, TFT_BLACK);
  for (uint8_t i = 0; i < 4; ++i)
  {
    const uint16_t color = (i == active) ? theme::BRIGHT : theme::DIM;
    const int radius = (i == active) ? 6 : 4;
    tft.fillCircle(dotX[i], 136, radius, color);
  }

  // Small animated signal bars below the dots.
  tft.fillRect(132, 164, 56, 30, TFT_BLACK);
  for (uint8_t i = 0; i < 4; ++i)
  {
    const int height = 5 + (i * 6);
    tft.fillRect(134 + (i * 13), 192 - height, 8, height,
                 i <= active ? theme::GREEN : theme::DARK);
  }
}

void CheapYellowDisplay::wifiAnimationTaskTrampoline(void *parameter)
{
  static_cast<CheapYellowDisplay *>(parameter)->wifiAnimationTask();
}

void CheapYellowDisplay::wifiAnimationTask()
{
  while (wifiAnimationActive)
  {
    drawWiFiAnimationFrame(wifiAnimationFrame++);

    // Sleep in short slices so a stop request is honoured within ~20 ms and the
    // task is never asked to die while it holds the display SPI bus.
    for (int i = 0; i < 8 && wifiAnimationActive; ++i)
      vTaskDelay(pdMS_TO_TICKS(WIFI_ANIMATION_FRAME_MS / 8));
  }

  wifiAnimationRunning = false;
  vTaskDelete(nullptr);
}

void CheapYellowDisplay::startWiFiConnectingAnimation()
{
  stopWiFiConnectingAnimation();

  wifiAnimationActive = true;
  wifiAnimationRunning = true;
  wifiAnimationFrame = 0;
  drawWiFiConnectingBase();

  const BaseType_t result = xTaskCreatePinnedToCore(
      wifiAnimationTaskTrampoline, "wifiAnim", 2048, this, 1,
      &wifiAnimationTaskHandle, 0);

  if (result != pdPASS)
  {
    wifiAnimationTaskHandle = nullptr;
    wifiAnimationActive = false;
    wifiAnimationRunning = false;
    Serial.println(F("Failed to start Wi-Fi animation task"));
  }
}

void CheapYellowDisplay::stopWiFiConnectingAnimation()
{
  if (!wifiAnimationRunning)
  {
    wifiAnimationTaskHandle = nullptr;
    return;
  }

  wifiAnimationActive = false;

  // Wait for the task to leave on its own. v0.3.15 force-deleted it with
  // vTaskDelete() after 250 ms; if the task happened to be inside a TFT call at
  // that moment, the display SPI bus lock was destroyed with it and every later
  // draw deadlocked. The task now checks the flag every 20 ms, so this loop
  // completes well inside its budget.
  const unsigned long deadline = deadlineIn(2000);
  while (wifiAnimationRunning && !timeReached(deadline))
    delay(5);

  if (wifiAnimationRunning)
    Serial.println(F("Wi-Fi animation task did not stop in time"));
  else
    wifiAnimationTaskHandle = nullptr;
}
