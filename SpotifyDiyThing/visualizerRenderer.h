#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

#include "animationPlayer.h"
#include "audioVisualizer.h"
#include "cydTheme.h"

// ---------------------------------------------------------------------------
// The microphone visualizers.
//
// v0.3.15 kept the mode list in four places that all had to agree: an enum, a
// name switch, a dispatch switch, and an `style >= CYBER_GRID` range test for
// which modes animate on their own clock. Reordering the enum silently changed
// behaviour. Here the table below is the single list - adding a mode is one
// row, and nothing else needs editing. v0.4 added twenty rows to it and changed
// nothing else about the dispatch, which is the whole point of the table.
//
// Rendering goes through canvas(): an off-screen 8-bit sprite when the
// allocation succeeds, and the panel directly when it does not. Differential
// modes (spectrum, block EQ, oscilloscope...) repaint only what changed;
// buffered modes clear and redraw, which is why they are pushed as one
// completed frame instead of letting the user see the clear pass.
//
// Colour comes from theme::VIZ_* and theme::vizHue(). No mode picks a raw
// colour: the palette is the one place that decides what green-cyan means, and
// keeping it that way is what let v0.4 recolour all forty modes at once.
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

  // Steps through the mode list and repaints immediately. A tap on the right
  // half of the overlay advances, the left half goes back - with forty modes,
  // one-way cycling made an overshoot a thirty-nine tap mistake.
  void nextStyle();
  void previousStyle();
  const char *styleName() const;

  // True while the frame-playback mode is the one on screen, so the display can
  // arm the play/stop hotspot only when there is something to play.
  bool isAnimationMode() const;

  // Starts or stops frame playback. Playback switches the microphone off: the
  // I2S task and the FFT share core 0 with the SD reads, and the animation does
  // not need either of them.
  void toggleAnimationPlayback();

  // The overlay covers the player screen, so brightness is cycled from a sun in
  // the chrome row. The value is pushed in rather than read, because the
  // backlight belongs to the display, not to the renderer.
  void setBrightnessPercent(uint8_t percent);

  // Called from the Arduino loop; rate-limits itself to the target frame rate.
  void service();

  // Repaints the whole overlay, including the chrome row.
  void redraw();

private:
  static constexpr size_t PULSE_RING_BAR_COUNT = AudioVisualizer::BAR_COUNT * 2;
  static constexpr size_t MODE_COUNT = 41;

  // Particle counts for the modes that carry their own state.
  static constexpr size_t STAR_COUNT = 30;
  static constexpr size_t RIPPLE_COUNT = 5;
  static constexpr size_t JET_COUNT = 24;

  // ~24 FPS. Fluid enough to read as motion, cheap enough to leave the ESP32
  // room for Wi-Fi and the Spotify clients.
  static constexpr unsigned long FRAME_INTERVAL_MS = 42;

  // How long after the last mode change to write the choice to NVS. Cycling
  // through forty modes is a burst of taps; writing on each one would put forty
  // erase cycles through the flash for what is really one decision.
  static constexpr unsigned long STYLE_SAVE_DELAY_MS = 4000;

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

  // Size is deduced from the definition in the .cpp; begin() static_asserts that
  // it matches MODE_COUNT, so a row added without bumping the count is a build
  // error rather than a null draw pointer at runtime.
  static const ModeDef MODES[];

  AudioVisualizer audioVisualizer;

  // Frame playback from SD. Scanned once at begin(); the 8 KB frame buffer is
  // only held while the PIONEER mode is the one on screen.
  AnimationPlayer animation;
  bool animationOpen = false;
  bool animationPlaying = false;
  uint8_t brightnessPercent = 100;

  bool micOk = false;
  bool openFlag = false;

  // Intentionally survives close/reopen so the chosen mode sticks, and since
  // v0.4 it survives a power cycle too: the car unit is switched off after every
  // drive, and coming back on a different mode each time read as a bug.
  uint8_t styleIndex = 0;
  Preferences preferences;
  bool preferencesReady = false;
  unsigned long styleSaveDue = 0;

  TFT_eSprite sprite = TFT_eSprite(&tft);
  bool spriteReady = false;

  unsigned long lastDrawTime = 0;
  uint32_t lastFrameCounter = 0;

  // Per-mode state. Only one mode runs at a time and resetDrawingState() clears
  // all of it on every switch, so modes share these buffers rather than each
  // carrying its own. On a 320 KB part that is the difference between forty
  // modes and about a dozen.
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
  uint8_t starAngle[STAR_COUNT] = {};
  uint8_t starRadius[STAR_COUNT] = {};
  uint8_t rippleRadius[RIPPLE_COUNT] = {};
  uint8_t rippleStrength[RIPPLE_COUNT] = {};
  bool rippleArmed = true;
  unsigned long lastWaterfallAdvanceTime = 0;

  // v0.4 mode state.
  uint8_t dolphinPhase = 0;    // 0..255 position along the leap
  int8_t dolphinDirection = 1; // +1 travelling right, -1 travelling left
  uint8_t dolphinArc = 0;      // jump height, smoothed from loudness
  uint8_t dolphinSplash = 0;   // splash countdown at the water line
  uint8_t jetX[JET_COUNT] = {};
  uint8_t jetY[JET_COUNT] = {};
  uint8_t jetSpeed[JET_COUNT] = {};
  int16_t gaugeAngle = 0;
  int16_t gaugePeakAngle = 0;
  unsigned long gaugePeakHoldUntil = 0;
  uint8_t scrollColumn = 0;
  uint8_t bounceHeight[AudioVisualizer::BAR_COUNT] = {};
  int8_t bounceVelocity[AudioVisualizer::BAR_COUNT] = {};

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

  void loadStyle();
  void serviceStyleSave();
  void stepStyle(int8_t direction);

  void resetDrawingState();
  void drawFrame(bool force);
  void drawChrome();
  void drawBrightnessSun();
  void drawAnimationPlayButton(TFT_eSPI &target);
  void drawMicError();

  uint16_t waterfallColor(uint8_t level) const;

  // v0.3 modes.
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
  void drawStarfield(const Frame &frame, bool force);
  void drawDnaHelix(const Frame &frame, bool force);
  void drawRipplePool(const Frame &frame, bool force);
  void drawPhaseScope(const Frame &frame, bool force);

  // v0.4 modes.
  void drawDolphin(const Frame &frame, bool force);
  void drawSpectrumArc(const Frame &frame, bool force);
  void drawTwinTowers(const Frame &frame, bool force);
  void drawWaveTunnel(const Frame &frame, bool force);
  void drawMirrorX(const Frame &frame, bool force);
  void drawCometTrail(const Frame &frame, bool force);
  void drawEqStairs(const Frame &frame, bool force);
  void drawSonarSweep(const Frame &frame, bool force);
  void drawFreqRibbon(const Frame &frame, bool force);
  void drawVoicePrint(const Frame &frame, bool force);
  void drawHexPulse(const Frame &frame, bool force);
  void drawLedMatrix(const Frame &frame, bool force);
  void drawSpiralArm(const Frame &frame, bool force);
  void drawBounceBalls(const Frame &frame, bool force);
  void drawScanLines(const Frame &frame, bool force);
  void drawKaleido(const Frame &frame, bool force);
  void drawNeedleGauge(const Frame &frame, bool force);
  void drawParticleJet(const Frame &frame, bool force);
  void drawWaveGrid(const Frame &frame, bool force);
  void drawChromaRings(const Frame &frame, bool force);
  void drawPioneer(const Frame &frame, bool force);
};

// Shared by both full-screen overlays (clock and visualizer).
void drawOverlayCloseButton();
