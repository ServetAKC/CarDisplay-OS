#ifndef SPOTIFYDISPLAY_H
#define SPOTIFYDISPLAY_H

#include <Arduino.h>
#include <SpotifyArduino.h>
#include <WiFiManager.h>

#include "deviceConfig.h"

class SpotifyDisplay {
  public:
    virtual void displaySetup(SpotifyArduino *spotifyObj) = 0;

    virtual void showDefaultScreen() = 0;

    // Track related
    virtual void displayTrackProgress(long progress, long duration) = 0;
    virtual void printCurrentlyPlayingToScreen(CurrentlyPlaying currentlyPlaying) = 0;

    //Probably Touch screen related
    virtual void checkForInput() =0;

    // Playback state changed (used by displays with play/pause controls).
    virtual void setPlaybackState(bool isPlaying) {}

    // Called from loop() for display-specific background work.
    // Default is a no-op so non-CYD displays keep their existing behaviour.
    virtual void service() {}

    // Optional startup animation shown while WiFiManager connects to a saved AP.
    // Non-CYD displays can safely ignore these hooks.
    //
    // setupOpensInMs is how long the caller will keep trying before it gives up
    // and opens the setup portal, so the screen can show the wait running down.
    // Zero means there is no such deadline and no progress bar is drawn.
    virtual void startWiFiConnectingAnimation(unsigned long setupOpensInMs = 0) {}
    virtual void stopWiFiConnectingAnimation() {}

    // Wi-Fi setup can run non-blocking so CYD touch/audio visualizers remain
    // available while the captive portal continues serving the phone.
    virtual void serviceConfigPortal() {}
    virtual void finishConfigPortal() {}

    // True while the display wants the radio genuinely off rather than merely
    // idle. The CYD returns this for the offline visualizer, where the AP, the
    // DNS responder and the web server are all pure overhead - and where they
    // cost frames, because the Wi-Fi stack shares core 0 with the microphone
    // task. The portal loop honours it by shutting the radio down and bringing
    // it back when the overlay closes.
    virtual bool wantsRadioSilence() const { return false; }

    //Image Related
    virtual void clearImage()= 0;
    virtual boolean processImageInfo (CurrentlyPlaying currentlyPlaying)=0;
    virtual int displayImage() = 0;

    //NFC tag messages
    virtual void markDisplayAsTagRead() = 0;
    virtual void markDisplayAsTagWritten() = 0;

    virtual void drawWifiManagerMessage(WiFiManager *myWiFiManager) = 0;
    virtual void drawRefreshTokenMessage() = 0;

    void setAlbumArtUrl(const char* albumArtUrl){
      copyField(_albumArtUrl, albumArtUrl);
    }

    char* getAlbumArtUrl(){
      return _albumArtUrl;
    }

    bool isSameAlbum(const char* albumArtUrl){
      return strcmp(_albumArtUrl, albumArtUrl) == 0;
    }

    void setWidth(int w) {
      screenWidth = w;
      screenCenterX = screenWidth / 2;
    }

    void setHeight(int h) {
      screenHeight = h;
    }

    void setImageHeight(int h) {
      imageHeight = h;
    }

    void setImageWidth(int w) {
      imageWidth = w;
    }

  protected:
    int screenWidth;
    int screenHeight;
    int screenCenterX;
    int imageWidth;
    int imageHeight;
    SpotifyArduino *spotify_display;
    char _albumArtUrl[256] = {0};
    boolean albumDisplayed = false;
};
#endif
