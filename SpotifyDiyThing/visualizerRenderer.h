#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "audioVisualizer.h"
#include "cydTheme.h"

// ---------------------------------------------------------------------------
// The sixteen microphone visualizers.
//
// v0.3.15 kept the mode list in four places that all had to agree: an enum, a
// name switch, a dispatch switch, and an `style >= CYBER_GRID` range test for
// which modes animate on their own clock. Reordering the enum silently changed
// behaviour. Here the table below is the single list - adding a mode is one
// row, and nothing else needs editing.
//
// Rendering goes through canvas(): an off-screen 8-bit sprite when the
// allocation succeeds, and the panel directly when it does not. Differential
// modes (spectrum, block EQ, oscilloscope...) repaint only what changed;
// buffered Y2K modes clear and redraw, which is why they are pushed as one
// completed frame instead of letting the user see the clear pass.
// ---------------------------------------------------------------------------

class VisualizerRenderer
{
public:
  struct Frame
  {
    const uint8_t *levels;
    const uint8_t *waveform;
    uint8_t overall;
  };

  // Starts the I2S microphone. Returns false if the INMP441 is not wired up;
  // the overlay then shows MIC ERROR instead of animating.
  bool begin();
  bool micReady() const { return micOk; }

  void open();
  void close();
  bool isOpen() const { return openFlag; }

  // Advances to the next mode and repaints immediately.
  void nextStyle();
  const char *styleName() const;

  // Called from the Arduino loop; rate-limits itself to the target frame rate.
  void service();

  // Repaints the whole overlay, including the chrome row.
  void redraw();

private:
  static constexpr size_t PULSE_RING_BAR_COUNT = AudioVisualizer::BAR_COUNT * 2;
  static constexpr size_t MODE_COUNT = 16;

  // ~24 FPS. Fluid enough to read as motion, cheap enough to leave the ESP32
  // room for Wi-Fi and the Spotify clients.
  static constexpr unsigned long FRAME_INTERVAL_MS = 42;

  using DrawFn = void (VisualizerRenderer::*)(const Frame &, bool);

  struct ModeDef
  {
    const char *name;
    DrawFn draw;
    // Modes that animate on their own clock - scrolling grids, orbiting nodes,
    // decaying peak markers - must be redrawn even when the microphone frame
    // has not advanced. Purely differential modes must not, or they flicker.
    bool continuous;
  };

  static const ModeDef MODES[MODE_COUNT];

  AudioVisualizer audioVisualizer;
  bool micOk = false;
  bool openFlag = false;

  // Intentionally survives close/reopen so the chosen mode sticks.
  uint8_t styleIndex = 0;

  TFT_eSprite sprite = TFT_eSprite(&tft);
  bool spriteReady = false;

  unsigned long lastDrawTime = 0;
  uint32_t lastFrameCounter = 0;

  // Per-mode differential state.
  uint16_t lastVisualizerHeight[AudioVisualizer::BAR_COUNT] = {};
  uint16_t visualizerPeakHeight[AudioVisualizer::BAR_COUNT] = {};
  unsigned long visualizerPeakHoldUntil[AudioVisualizer::BAR_COUNT] = {};
  uint8_t lastBlockCount[AudioVisualizer::BAR_COUNT] = {};
  int16_t lastOscilloscopeY[AudioVisualizer::WAVEFORM_COUNT] = {};
  bool oscilloscopeFrameValid = false;
  uint16_t lastRadialLength[AudioVisualizer::BAR_COUNT] = {};
  uint8_t lastPulseRingLength[PULSE_RING_BAR_COUNT] = {};
  uint16_t retroVisualizerFrame = 0;
  int lastVuWidth = 0;
  int vuPeakWidth = 0;
  unsigned long vuPeakHoldUntil = 0;
  int waterfallWriteY = 64;
  unsigned long lastWaterfallAdvanceTime = 0;

  TFT_eSPI &canvas();
  void present();

  // startWrite()/endWrite() are not virtual in TFT_eSPI, so calling them
  // through a base reference that actually points at a sprite opens a real SPI
  // transaction against the panel for drawing that never reaches it. Skip them
  // whenever the sprite is the target.
  void beginFrame(TFT_eSPI &target);
  void endFrame(TFT_eSPI &target);

  bool ensureSprite();
  void releaseSprite();

  void resetDrawingState();
  void drawFrame(bool force);
  void drawChrome();
  void drawMicError();

  uint16_t waterfallColor(uint8_t level) const;

  void drawSpectrumWithPeaks(const Frame &frame, bool force);
  void drawMirroredSpectrum(const Frame &frame, bool force);
  void drawOscilloscope(const Frame &frame, bool force);
  void drawBlockEqualizer(const Frame &frame, bool force);
  void drawVuMeter(const Frame &frame, bool force);
  void drawRadialSpectrum(const Frame &frame, bool force);
  void drawPulseRing(const Frame &frame, bool force);
  void drawWaterfall(const Frame &frame, bool force);
  void drawCyberGrid(const Frame &frame, bool force);
  void drawLaserTunnel(const Frame &frame, bool force);
  void drawDataRain(const Frame &frame, bool force);
  void drawNeonWave(const Frame &frame, bool force);
  void drawOrbitLink(const Frame &frame, bool force);
  void drawPixelCity(const Frame &frame, bool force);
  void drawRadar2000(const Frame &frame, bool force);
  void drawDualDisc(const Frame &frame, bool force);
};

// Shared by both full-screen overlays (clock and visualizer).
void drawOverlayCloseButton();
