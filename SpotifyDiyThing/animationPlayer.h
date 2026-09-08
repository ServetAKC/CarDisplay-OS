#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// ---------------------------------------------------------------------------
// Frame-by-frame animation playback from the SD card.
//
// Every other visualizer in this firmware is procedural: it reads the FFT and
// draws lines. The Pioneer head units this project keeps being compared to are
// not - their dolphins and race cars are pre-drawn frame sequences played back
// like video. No amount of trigonometry produces that, so this plays frames.
//
// The original displays were monochrome OEL panels, so the format is 1 bit per
// pixel. That is not a compromise, it is what makes this affordable: a 320x200
// frame is 8000 bytes against 128000 for RGB565, and at 24 FPS the difference
// is 192 KB/s off the card versus 3 MB/s. The single bit picks between the
// background and one palette colour, which also means the animation inherits
// the product's green-cyan rather than fighting it.
//
// Packs live in /anim/*.anm on the same card the album cache already uses.
// tools/make_animation.py builds them from a PNG sequence or an animated GIF.
// Adding an animation is copying a file onto the card - no reflash.
// ---------------------------------------------------------------------------

class AnimationPlayer
{
public:
  // Largest frame the row expander can take, and the size of the heap buffer
  // allocated while a pack is open. Matches the overlay area.
  static constexpr int MAX_WIDTH = 320;
  static constexpr int MAX_HEIGHT = 200;
  static constexpr size_t MAX_FRAME_BYTES = (MAX_WIDTH / 8) * MAX_HEIGHT;

  // How many packs to remember. More than this on the card is not an error;
  // the rest are simply not played.
  static constexpr size_t MAX_PACKS = 8;

  // Scans /anim for packs. Cheap and safe to call when there is no card.
  // Returns the number found.
  size_t begin();

  size_t packCount() const { return packTotal; }
  bool ready() const { return packTotal > 0; }

  // Allocates the frame buffer and opens the first pack. Call when the mode
  // becomes visible; the buffer is 8 KB and there is no reason to hold it while
  // some other mode is running.
  bool open();
  void close();

  // Advances to the next frame if enough time has passed, and draws it.
  // `target` is the renderer's canvas - the off-screen sprite when one exists.
  // Returns false when there is nothing to play, so the caller can say so on
  // screen rather than leaving it black.
  bool service(TFT_eSPI &target, int originX, int originY);

  // Redraws the frame already loaded without advancing. This is the paused
  // state: the mode shows frame zero as a poster behind its play button.
  void drawCurrent(TFT_eSPI &target, int originX, int originY) const;

  // Name of the pack now playing, for the chrome row. Empty when idle.
  const char *currentPackName() const;

  // The selector needs the whole list, not just the one open: a name per row
  // and the index to tick. Names come back even before open() has allocated
  // the frame buffer, because begin() fills them from the directory scan.
  const char *packNameAt(size_t index) const;
  size_t currentPackIndex() const { return packIndex; }

  // Jump straight to one pack instead of waiting for the show to reach it.
  // Returns false if the index is out of range or the card has gone away, in
  // which case whatever was open stays open.
  bool selectPack(size_t index);

  // Size of the frames in the open pack, so the caller can centre it and decide
  // whether there is room left for anything else. Zero when nothing is open.
  int frameWidth() const { return packOpen ? header.width : 0; }
  int frameHeight() const { return packOpen ? header.height : 0; }

private:
  struct Header
  {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t frameCount;
    uint8_t frameRate;
    uint8_t flags;
    uint32_t reserved;
  };

  static constexpr uint32_t ANM_MAGIC = 0x314D4E41UL; // "ANM1", little endian

  char packPath[MAX_PACKS][40] = {};
  char packName[MAX_PACKS][24] = {};
  size_t packTotal = 0;
  size_t packIndex = 0;

  uint8_t *frameBuffer = nullptr;
  bool packOpen = false;
  Header header = {};
  uint16_t frameIndex = 0;
  size_t rowBytes = 0;
  unsigned long nextFrameTime = 0;

  bool openPack(size_t index);
  void closePack();
  bool readFrame(uint16_t index);
  void drawFrame(TFT_eSPI &target, int originX, int originY) const;
};
