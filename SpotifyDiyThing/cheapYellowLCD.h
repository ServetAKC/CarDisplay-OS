#pragma once

#include "albumArtCache.h"
#include "backlightController.h"
#include "cydTheme.h"
#include "sdJapaneseFont.h"
#include "spotifyDisplay.h"
#include "touchScreen.h"
#include "visualizerRenderer.h"

// ---------------------------------------------------------------------------
// The Cheap Yellow Display implementation of SpotifyDisplay.
//
// This class used to be 3400 lines and owned everything: TFT drawing, touch
// routing, SD and SPIFFS caching, HTTP downloads, PWM brightness, solar
// geometry, NTP, and sixteen visualizers. It now owns only the player screen
// and the overlays, and delegates the rest:
//
//   AlbumArtCache        - acquisition, caching and drawing of covers
//   BacklightController  - manual levels, NVS, automatic sunset dim
//   VisualizerRenderer   - the microphone modes and their frame buffer
//
// Everything that touches the TFT still runs on the Arduino loop task; the
// background tasks below only do network and SD work.
// ---------------------------------------------------------------------------

class CheapYellowDisplay : public SpotifyDisplay
{
public:
  bool isVisualizerActive() const { return visualizer.isOpen(); }

  void displaySetup(SpotifyArduino *spotifyObj) override;
  void showDefaultScreen() override;

  void displayTrackProgress(long progress, long duration) override;
  void printCurrentlyPlayingToScreen(CurrentlyPlaying currentlyPlaying) override;
  void setPlaybackState(bool isPlaying) override;

  void checkForInput() override;
  void service() override;

  void clearImage() override;
  boolean processImageInfo(CurrentlyPlaying currentlyPlaying) override;
  int displayImage() override;

  void markDisplayAsTagRead() override;
  void markDisplayAsTagWritten() override;

  void drawWifiManagerMessage(WiFiManager *myWiFiManager) override;
  void drawRefreshTokenMessage() override;

  void startWiFiConnectingAnimation() override;
  void stopWiFiConnectingAnimation() override;

  void serviceConfigPortal() override;
  void finishConfigPortal() override;

private:
  // Playback actions, kept as a small enum so the command queue and the icon
  // highlighting cannot disagree about what a value means.
  enum class Action : int8_t
  {
    None = 0,
    Previous,
    Next,
    PlayPause, // UI-level toggle
    Play,      // resolved command sent to Spotify
    Pause
  };

  static constexpr unsigned long CONTROL_FEEDBACK_MS = 220;
  static constexpr unsigned long TOUCH_COOLDOWN_MS = 150;
  static constexpr unsigned long BRIGHTNESS_TOAST_MS = 900;
  static constexpr unsigned long CLOCK_REDRAW_MS = 1000;
  static constexpr unsigned long FULLSCREEN_CLOCK_REDRAW_MS = 250;
  static constexpr unsigned long WIFI_ANIMATION_FRAME_MS = 160;

  AlbumArtCache albumCache;
  BacklightController backlight;
  VisualizerRenderer visualizer;

  SdJapaneseFont japaneseFont;
  bool japaneseFontReady = false;

  // Player screen state.
  bool playerShellDrawn = false;
  bool playbackIsPlaying = true;
  int lastProgressBarWidth = -1;
  long lastProgressDuration = -1;
  char lastElapsedText[8] = "";
  char lastDurationText[8] = "";

  // Cached player state, so returning from an overlay is instant.
  char cachedTrackName[101] = "";
  char cachedArtistName[101] = "";
  char cachedAlbumName[101] = "";
  bool cachedTrackValid = false;
  long cachedProgress = 0;
  long cachedDuration = 1;

  // Clock.
  bool clockConfigured = false;
  unsigned long lastClockDrawTime = 0;
  char lastClockText[6] = "";
  bool clockMode = false;
  unsigned long lastFullscreenClockDrawTime = 0;
  char lastFullscreenClockText[6] = "";

  // Wi-Fi setup screen.
  bool wifiSetupMode = false;
  char wifiSetupSsid[34] = "SpotifyDIY";
  char wifiSetupIp[20] = "192.168.4.1";

  // Brightness toast.
  unsigned long brightnessToastUntil = 0;
  bool brightnessToastVisible = false;

  // Touch and control feedback.
  unsigned long touchCooldownUntil = 0;
  Action controlFeedbackAction = Action::None;
  unsigned long controlFeedbackUntil = 0;

  // Playback command task.
  QueueHandle_t playbackCommandQueue = nullptr;
  TaskHandle_t playbackCommandTaskHandle = nullptr;
  volatile bool playPauseRollbackRequested = false;

  // Startup Wi-Fi animation task.
  TaskHandle_t wifiAnimationTaskHandle = nullptr;
  volatile bool wifiAnimationActive = false;
  volatile bool wifiAnimationRunning = false;
  volatile uint8_t wifiAnimationFrame = 0;

  bool overlayModeActive() const { return clockMode || visualizer.isOpen(); }
  static const char *safeText(const char *text);

  // Screen composition.
  void ensurePlayerShell();
  void drawPlayerShell();
  void restorePlayerScreen();
  void drawHeader(bool force = false);
  void drawWifiIndicator();
  void drawSpotifyIcon(int centerX, int centerY, int radius, uint16_t color);
  void drawAlbumPlaceholder();
  void drawTrackPlaceholder();
  void drawCachedTrackInfo();
  void drawEllipsized(const char *text, int x, int y, int maxWidth, int font, uint16_t color);
  void drawWifiSetupScreen();
  bool drawAlbumArt();

  // Playback controls.
  void drawPlaybackControls(Action activeAction);
  void drawSinglePlaybackControl(Action action, uint16_t color);
  void drawPreviousIcon(int centerX, int centerY, uint16_t color);
  void drawNextIcon(int centerX, int centerY, uint16_t color);
  void drawPlayPauseIcon(int centerX, int centerY, uint16_t color);
  void serviceControlFeedback();
  void handlePlaybackTouch(Action action);

  // Overlays.
  void toggleClockMode();
  void toggleVisualizerMode();
  void serviceFullscreenClock();
  void drawFullscreenClock(bool force);
  void serviceClock();

  // Brightness.
  void cycleBrightness();
  void drawBrightnessToast();
  void serviceBrightness();

  // Album art hand-off.
  void serviceAlbumArt();

  // Tasks.
  static void playbackCommandTaskTrampoline(void *parameter);
  void playbackCommandTask();
  static void wifiAnimationTaskTrampoline(void *parameter);
  void wifiAnimationTask();
  void drawWiFiConnectingBase();
  void drawWiFiAnimationFrame(uint8_t frame);

  static void formatDuration(long milliseconds, char *out, size_t outSize);
};
