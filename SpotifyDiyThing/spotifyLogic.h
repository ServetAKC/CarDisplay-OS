#pragma once

#include <Arduino.h>
#include <SpotifyArduino.h>

#include "deviceConfig.h"
#include "spotifyDisplay.h"

// Maximum length of one queued album-art URL kept by the prefetch window.
constexpr size_t SPOTIFY_QUEUE_URL_SIZE = 256;

// Three independent Spotify clients, each with its own TLS session:
//   spotify        - now-playing polling (can block for seconds)
//   spotifyControl - playback commands, so a tap is never stuck behind a poll
//   spotifyQueue   - queue prefetch, so cover downloads never block either
extern SpotifyArduino spotify;
extern SpotifyArduino spotifyControl;
extern SpotifyArduino spotifyQueue;

extern char lastTrackUri[200];
extern char lastTrackContextUri[200];

// Shared with the CYD display tasks. While true, normal polling pauses so
// playback commands get network priority.
extern volatile bool spotifyControlCommandPending;
extern bool spotifyControlReady;

void spotifySetup(SpotifyDisplay *display, const DeviceConfig &config);
void spotifyControlSetup(const DeviceConfig &config);
void spotifyQueueSetup(const DeviceConfig &config);
void spotifyRefreshToken(const char *refreshToken);

// Reads several queued cover URLs in one API request.
size_t spotifyGetQueuedAlbumArtUrls(char out[][SPOTIFY_QUEUE_URL_SIZE], size_t maxUrls);

// Drops the queue-prefetch TLS session and its buffers. The prefetch client is
// idle while a full-screen overlay is open, and handing ~40 KB back is often
// what lets the visualizer allocate its 64 KB frame buffer. The session is
// re-established transparently on the next queue request.
void spotifyReleaseQueueConnection();

void updateCurrentlyPlaying(bool forceUpdate);
void updateProgressBar();

// Ask the poller to check back sooner than the normal interval. Spotify can
// briefly return the previous track right after accepting a command, so a short
// burst of quick polls settles the UI faster than one delayed poll.
void requestRapidRefresh(uint8_t extraPolls, unsigned long firstDelayMs);
void scheduleNextPoll(unsigned long delayMs);
