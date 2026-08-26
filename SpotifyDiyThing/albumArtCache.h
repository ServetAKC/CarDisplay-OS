#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "spotifyLogic.h"

// ---------------------------------------------------------------------------
// Album art acquisition and caching.
//
// Three tiers, fastest first:
//   1. SD  .rgb - a pre-decoded 160x160 RGB565 blob, pushed straight to the TFT
//   2. SD  .jpg - decoded from the card, then promoted to a .rgb on the way out
//   3. network  - downloaded to SPIFFS, shown, then written to both SD caches
//
// All network and SD work happens on a background task pinned to core 0. The
// Arduino loop only performs the final hand-off and the actual drawing, so the
// TFT driver is still touched from exactly one task.
//
// Every request carries a generation number. A track change bumps it, which
// makes any in-flight download or prefetch abandon its work at the next check
// instead of painting a cover that is already stale.
// ---------------------------------------------------------------------------

class AlbumArtCache
{
public:
  // Pre-decoded cache tile size; matches layout::IMAGE_SIZE.
  static constexpr int CACHED_ART_SIZE = 160;

  // How many unique covers to keep ready ahead of the current track.
  static constexpr size_t QUEUE_PREFETCH_LIMIT = 8;

  // Hard ceiling for one cover download. Spotify 640px covers are ~40-90 KB;
  // this only exists so a truncated or hostile response cannot fill the card.
  static constexpr size_t MAX_COVER_BYTES = 512UL * 1024UL;

  enum class Handoff : uint8_t
  {
    Nothing,   // no completed download waiting
    Discarded, // the result belonged to a track we have already left
    Ready,     // a fresh cover is current and can be drawn
    Failed     // the file did not survive the hand-off; retry later
  };

  // Mounts the SD card (optional) and starts the background task.
  bool begin();

  bool sdReady() const { return sdCacheReady; }

  // Queue a cover for download. Cancels whatever was in flight.
  void request(const char *url);

  // Pauses all network and SD work while a full-screen overlay owns the
  // renderer, so visualizer frames are never interrupted by I/O.
  void setPaused(bool paused);

  // Lets the background task open its TLS connection. Called once the Spotify
  // polling response has been fully consumed, so the two do not overlap.
  void allowNetworkStart();

  bool downloadInFlight() const { return downloadActive; }
  bool handoffPending() const { return handoffReady; }
  bool retryDue() const;

  // Completes a finished background download. Must run on the Arduino loop.
  Handoff serviceHandoff();

  // Draws the current cover into the album rectangle. False if there is none.
  bool drawCurrent();

private:
  enum class Source : uint8_t
  {
    None,
    SpiffsTemp,
    SpiffsCurrent,
    SdJpeg,
    SdRaw
  };

  struct Request
  {
    char url[SPOTIFY_QUEUE_URL_SIZE];
    uint32_t generation;
  };

  struct TransferOptions
  {
    uint32_t generation;
    // Prefetch yields to a playback command; the cover for the track actually
    // on screen does not, or a tap would leave the user staring at a blank box.
    bool abortOnControlCommand;
    unsigned long timeoutMs;
  };

  static constexpr unsigned long RETRY_INTERVAL_MS = 15000;

  QueueHandle_t requestQueue = nullptr;
  TaskHandle_t taskHandle = nullptr;

  bool sdCacheReady = false;
  volatile bool pausedFlag = false;
  volatile bool downloadActive = false;
  volatile bool handoffReady = false;
  volatile bool networkStartAllowed = false;
  volatile uint32_t requestGeneration = 0;
  volatile uint32_t handoffGeneration = 0;
  volatile Source handoffSource = Source::None;
  char handoffPath[64] = "";

  Source currentSource = Source::None;
  char currentPath[64] = "";

  unsigned long nextRetryTime = 0;

  char queuedUrls[QUEUE_PREFETCH_LIMIT][SPOTIFY_QUEUE_URL_SIZE] = {};

  static void taskTrampoline(void *parameter);
  void taskLoop();

  bool cancelled(const TransferOptions &options) const;
  bool streamToFile(const char *url, fs::File &destination,
                    const TransferOptions &options);
  bool downloadToSpiffsTemp(const char *url, uint32_t generation);
  bool downloadToSdCache(const char *url, const char *cachePath, uint32_t generation);

  bool copyTempToSdCache(const char *cachePath);
  bool isValidRawCache(const char *rawPath) const;
  bool createRawCacheFromJpeg(const char *jpegPath, const char *rawPath);
  void prefetchQueuedCovers(const char *currentUrl, uint32_t generation);

  bool drawRawCacheFile(const char *rawPath);
  bool drawJpegFile(fs::FS &sourceFs, const char *path);

  void buildJpegCachePath(const char *url, char *out, size_t outSize) const;
  void buildRawCachePath(const char *url, char *out, size_t outSize) const;
};
