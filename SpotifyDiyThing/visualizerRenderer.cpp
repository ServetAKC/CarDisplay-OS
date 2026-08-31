#include "visualizerRenderer.h"

#include <string.h>

#include "spotifyLogic.h"
#include "timing.h"
#include "visualizerGeometry.h"

// The one list of visualizer modes. Order here is the order the on-screen
// "n/N" counter and the tap-to-cycle sequence follow.
const VisualizerRenderer::ModeDef VisualizerRenderer::MODES[] = {
    {"SPECTRUM", &VisualizerRenderer::drawSpectrumWithPeaks, true},
    {"MIRRORED", &VisualizerRenderer::drawMirroredSpectrum, false},
    {"OSCILLOSCOPE", &VisualizerRenderer::drawOscilloscope, false},
    {"BLOCK EQ", &VisualizerRenderer::drawBlockEqualizer, false},
    {"VU METER", &VisualizerRenderer::drawVuMeter, true},
    {"RADIAL", &VisualizerRenderer::drawRadialSpectrum, false},
    {"PULSE RING", &VisualizerRenderer::drawPulseRing, true},
    {"WATERFALL", &VisualizerRenderer::drawWaterfall, false},
    {"CYBER GRID", &VisualizerRenderer::drawCyberGrid, true},
    {"LASER TUNNEL", &VisualizerRenderer::drawLaserTunnel, true},
    {"DATA RAIN", &VisualizerRenderer::drawDataRain, true},
    {"NEON WAVE", &VisualizerRenderer::drawNeonWave, true},
    {"ORBIT LINK", &VisualizerRenderer::drawOrbitLink, true},
    {"PIXEL CITY", &VisualizerRenderer::drawPixelCity, true},
    {"RADAR 2000", &VisualizerRenderer::drawRadar2000, true},
    {"DUAL DISC", &VisualizerRenderer::drawDualDisc, true},
    {"STARFIELD", &VisualizerRenderer::drawStarfield, true},
    {"DNA HELIX", &VisualizerRenderer::drawDnaHelix, true},
    {"RIPPLE POOL", &VisualizerRenderer::drawRipplePool, true},
    {"PHASE SCOPE", &VisualizerRenderer::drawPhaseScope, true},
    // v0.4. Twenty more, all reached through the same table; see
    // visualizerModes.cpp for the draw functions.
    {"DOLPHIN", &VisualizerRenderer::drawDolphin, true},
    {"SPECTRUM ARC", &VisualizerRenderer::drawSpectrumArc, false},
    {"TWIN TOWERS", &VisualizerRenderer::drawTwinTowers, false},
    {"WAVE TUNNEL", &VisualizerRenderer::drawWaveTunnel, true},
    {"MIRROR X", &VisualizerRenderer::drawMirrorX, false},
    {"COMET TRAIL", &VisualizerRenderer::drawCometTrail, true},
    {"EQ STAIRS", &VisualizerRenderer::drawEqStairs, false},
    {"SONAR SWEEP", &VisualizerRenderer::drawSonarSweep, true},
    {"FREQ RIBBON", &VisualizerRenderer::drawFreqRibbon, false},
    {"VOICE PRINT", &VisualizerRenderer::drawVoicePrint, true},
    {"HEX PULSE", &VisualizerRenderer::drawHexPulse, false},
    {"LED MATRIX", &VisualizerRenderer::drawLedMatrix, false},
    {"SPIRAL ARM", &VisualizerRenderer::drawSpiralArm, true},
    {"BOUNCE BALLS", &VisualizerRenderer::drawBounceBalls, true},
    {"SCAN LINES", &VisualizerRenderer::drawScanLines, false},
    {"KALEIDO", &VisualizerRenderer::drawKaleido, false},
    {"NEEDLE GAUGE", &VisualizerRenderer::drawNeedleGauge, true},
    {"PARTICLE JET", &VisualizerRenderer::drawParticleJet, true},
    {"WAVE GRID", &VisualizerRenderer::drawWaveGrid, true},
    {"CHROMA RINGS", &VisualizerRenderer::drawChromaRings, false},
    // Not procedural like every row above it: this one plays frame packs off
    // the SD card. See animationPlayer.h for why that needs to exist at all.
    {"PIONEER", &VisualizerRenderer::drawPioneer, true},
};

namespace
{
// The direction tables moved to visualizerGeometry.h in v0.4 so that
// visualizerModes.cpp sees the same numbers instead of keeping a second copy.
using vizgeom::UNIT_DX;
using vizgeom::UNIT_DY;

// The mode that a fresh device starts on. Looked up by name rather than index
// so reordering the table above cannot silently change the default.
constexpr const char *DEFAULT_STYLE_NAME = "PULSE RING";
} // namespace

void drawOverlayCloseButton()
{
  constexpr int X = 286;
  constexpr int Y = 6;
  constexpr int SIZE = 26;
  tft.fillRect(X, Y, SIZE, SIZE, TFT_BLACK);
  tft.drawRect(X, Y, SIZE, SIZE, theme::DIM);
  tft.drawLine(X + 7, Y + 7, X + SIZE - 8, Y + SIZE - 8, theme::GREEN);
  tft.drawLine(X + SIZE - 8, Y + 7, X + 7, Y + SIZE - 8, theme::GREEN);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool VisualizerRenderer::begin()
{
  static_assert(sizeof(MODES) / sizeof(MODES[0]) == MODE_COUNT,
                "MODE_COUNT must match the number of rows in the MODES table");

  loadStyle();

  // Scanning the card once here means the PIONEER mode knows whether it has
  // anything to play before the user ever selects it.
  animation.begin();

  micOk = audioVisualizer.begin();
  if (!micOk)
    Serial.println(F("Visualizer will show MIC ERROR until I2S setup is fixed"));
  return micOk;
}

// Restores the mode chosen on the last drive, falling back to the default when
// NVS is empty or holds an index from a build with fewer modes.
void VisualizerRenderer::loadStyle()
{
  for (size_t i = 0; i < MODE_COUNT; ++i)
  {
    if (strcmp(MODES[i].name, DEFAULT_STYLE_NAME) == 0)
    {
      styleIndex = static_cast<uint8_t>(i);
      break;
    }
  }

  preferencesReady = preferences.begin("hondathing", false);
  if (!preferencesReady)
  {
    Serial.println(F("Visualizer: NVS unavailable, mode will not persist"));
    return;
  }

  // 0xFF is the "never written" marker: a real index can never be that, and it
  // keeps a fresh device on DEFAULT_STYLE_NAME rather than on mode zero.
  const uint8_t saved = preferences.getUChar("vizmode", 0xFF);
  if (saved < MODE_COUNT)
    styleIndex = saved;
}

// Writes the chosen mode once the user has stopped cycling. Called from
// service(), so it only runs while the overlay is actually open.
void VisualizerRenderer::serviceStyleSave()
{
  if (styleSaveDue == 0 || !timeReached(styleSaveDue))
    return;

  styleSaveDue = 0;
  if (!preferencesReady)
    return;

  if (preferences.getUChar("vizmode", 0xFF) != styleIndex)
    preferences.putUChar("vizmode", styleIndex);
}

const char *VisualizerRenderer::styleName() const
{
  return MODES[styleIndex % MODE_COUNT].name;
}

void VisualizerRenderer::open()
{
  openFlag = true;
  lastDrawTime = 0;
  resetDrawingState();
  tft.fillScreen(TFT_BLACK);
  audioVisualizer.setActive(true);
  drawFrame(true);
}

void VisualizerRenderer::close()
{
  openFlag = false;
  audioVisualizer.setActive(false);
  releaseSprite();

  if (animationOpen)
  {
    animation.close();
    animationOpen = false;
  }

  // Leaving the overlay is the last chance to persist the mode; the user may
  // switch the ignition off from the player screen.
  if (styleSaveDue != 0)
  {
    styleSaveDue = 1; // any already-reached deadline
    serviceStyleSave();
  }
}

// Both directions share this. Tapping the right half of the screen advances and
// the left half goes back, so a mode overshot in a set of forty is one tap away
// instead of thirty-nine.
void VisualizerRenderer::stepStyle(int8_t direction)
{
  if (!openFlag)
    return;

  const int next = (static_cast<int>(styleIndex) + direction + static_cast<int>(MODE_COUNT)) %
                   static_cast<int>(MODE_COUNT);
  styleIndex = static_cast<uint8_t>(next);
  styleSaveDue = deadlineIn(STYLE_SAVE_DELAY_MS);

  resetDrawingState();
  lastDrawTime = 0;
  drawFrame(true);
}

void VisualizerRenderer::nextStyle()
{
  stepStyle(1);
}

void VisualizerRenderer::previousStyle()
{
  stepStyle(-1);
}

void VisualizerRenderer::redraw()
{
  if (!openFlag)
    return;
  lastDrawTime = 0;
  drawFrame(true);
}

void VisualizerRenderer::service()
{
  if (!openFlag)
    return;

  serviceStyleSave();

  if (!elapsed(lastDrawTime, FRAME_INTERVAL_MS))
    return;

  lastDrawTime = millis();
  drawFrame(false);
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

TFT_eSPI &VisualizerRenderer::canvas()
{
  return spriteReady ? static_cast<TFT_eSPI &>(sprite) : tft;
}

void VisualizerRenderer::present()
{
  if (spriteReady)
    sprite.pushSprite(0, layout::OVERLAY_TOP);
}

void VisualizerRenderer::beginFrame(TFT_eSPI &target)
{
  if (!spriteReady)
    target.startWrite();
}

void VisualizerRenderer::endFrame(TFT_eSPI &target)
{
  if (!spriteReady)
    target.endWrite();
}

bool VisualizerRenderer::ensureSprite()
{
  if (spriteReady)
    return true;

  // Retrying a 64 KB allocation on every frame achieves nothing except a log
  // line every 42 ms, which is what v0.4.0 shipped doing. Back off instead: the
  // memory this needs is normally freed by something else finishing - the setup
  // portal closing, the radio going off, a Spotify TLS session handed back - so
  // a slow poll picks it up without the spam and without the wasted mallocs.
  if (spriteFailed && !timeReached(nextSpriteAttempt))
    return false;
  nextSpriteAttempt = deadlineIn(SPRITE_RETRY_MS);

  // 320 x 200 at 8 bpp is 64 KB, which is a large slice of the ESP32 heap once
  // three TLS sessions are up. The queue-prefetch client is idle for as long as
  // the overlay is open, so hand its session back first; that is often the
  // difference between a buffered frame and falling back to direct draw.
  spotifyReleaseQueueConnection();

  sprite.setColorDepth(8);
  if (sprite.createSprite(layout::SCREEN_WIDTH, layout::OVERLAY_HEIGHT) == nullptr)
  {
    if (!spriteFailed)
    {
      spriteFailed = true;
      // The largest free block matters here, not the total: the heap can hold
      // 100 KB and still refuse 64 KB contiguous. Printing both is the
      // difference between diagnosing fragmentation and guessing at it.
      Serial.printf("Visualizer frame buffer allocation failed; using direct draw "
                    "(free %u, largest block %u, needed %u)\n",
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()),
                    static_cast<unsigned>(layout::SCREEN_WIDTH * layout::OVERLAY_HEIGHT));
    }
    return false;
  }

  if (spriteFailed)
    Serial.println(F("Visualizer frame buffer allocated; buffered frames resumed"));
  spriteFailed = false;

  // Draw functions keep normal screen coordinates; content starts at y = 40.
  sprite.setOrigin(0, -layout::OVERLAY_TOP);
  sprite.setTextWrap(false);
  spriteReady = true;
  return true;
}

void VisualizerRenderer::releaseSprite()
{
  // Cleared even when no sprite was held, so reopening the overlay always gets
  // one immediate attempt rather than waiting out a stale backoff.
  spriteFailed = false;
  nextSpriteAttempt = 0;

  if (!spriteReady)
    return;
  sprite.deleteSprite();
  spriteReady = false;
  spriteReady = false;
}

// ---------------------------------------------------------------------------
// Frame dispatch
// ---------------------------------------------------------------------------

void VisualizerRenderer::resetDrawingState()
{
  lastFrameCounter = 0;
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

  // Seed the starfield deterministically: 11 is coprime with 32, so stepping by
  // it walks every direction before repeating, and staggering the radii means
  // the first frame is already a spread field rather than one expanding ring.
  for (size_t i = 0; i < STAR_COUNT; ++i)
  {
    starAngle[i] = static_cast<uint8_t>((i * 11U) & 31U);
    starRadius[i] = static_cast<uint8_t>(4 + (i * 113U) % 145U);
  }

  memset(rippleRadius, 0, sizeof(rippleRadius));
  memset(rippleStrength, 0, sizeof(rippleStrength));
  rippleArmed = true;

  // v0.4 modes. Same contract as everything above: one mode runs at a time and
  // switching must never leave it reading another mode's leftovers.
  dolphinPhase = 0;
  dolphinDirection = 1;
  dolphinArc = 26;
  dolphinSplash = 0;
  memset(jetX, 0, sizeof(jetX));
  memset(jetY, 0, sizeof(jetY));
  memset(jetSpeed, 0, sizeof(jetSpeed));
  gaugeAngle = 0;
  gaugePeakAngle = 0;
  gaugePeakHoldUntil = 0;
  scrollColumn = 0;
  memset(bounceHeight, 0, sizeof(bounceHeight));
  memset(bounceVelocity, 0, sizeof(bounceVelocity));
}

void VisualizerRenderer::drawMicError()
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(theme::GREEN, TFT_BLACK);
  tft.drawCentreString("MIC ERROR", layout::CENTRE_X, 92, 4);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString("Check INMP441 wiring", layout::CENTRE_X, 130, 2);
  drawOverlayCloseButton();
}

void VisualizerRenderer::drawChrome()
{
  // The angle brackets are the only hint that the overlay is split left/right,
  // and with forty modes it is a hint worth the eight pixels it costs.
  char label[40];
  snprintf(label, sizeof(label), "< %u/%u  %s >",
           static_cast<unsigned>(styleIndex) + 1U,
           static_cast<unsigned>(MODE_COUNT),
           styleName());
  tft.fillRect(0, 0, 278, layout::OVERLAY_TOP, TFT_BLACK);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawString(label, 8, 12, 2);
  drawBrightnessSun();
  drawOverlayCloseButton();
}

// The brightness control for the overlay.
//
// The player screen cycles brightness from the Spotify badge, and the overlay
// covers that badge - so until v0.4 the only way to dim the offline visualizer
// at night was to close it, dim, and reopen it.
//
// A plain sun: filled core, eight straight rays off the direction table. No
// gradient and no glow. It is eleven pixels across and has to read at a glance
// from a driving position.
void VisualizerRenderer::drawBrightnessSun()
{
  constexpr int CX = 238;
  constexpr int CY = 19;
  constexpr int CORE = 5;
  constexpr int RAY_INNER = 8;
  constexpr int RAY_OUTER = 11;

  tft.fillRect(224, 0, 52, layout::OVERLAY_TOP, TFT_BLACK);
  tft.fillCircle(CX, CY, CORE, theme::BRIGHT);
  for (int i = 0; i < 32; i += 4) // eight rays, every fourth direction
  {
    const int x0 = CX + (UNIT_DX[i] * RAY_INNER) / 1000;
    const int y0 = CY + (UNIT_DY[i] * RAY_INNER) / 1000;
    const int x1 = CX + (UNIT_DX[i] * RAY_OUTER) / 1000;
    const int y1 = CY + (UNIT_DY[i] * RAY_OUTER) / 1000;
    tft.drawLine(x0, y0, x1, y1, theme::BRIGHT);
  }

  char value[8];
  snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(brightnessPercent));
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawString(value, 252, 13, 1);
}

void VisualizerRenderer::setBrightnessPercent(uint8_t percent)
{
  brightnessPercent = percent;
  // Only the sun is repainted. Redrawing the whole chrome would be a visible
  // flash on a screen whose whole job is to be looked at.
  if (openFlag && micOk)
    drawBrightnessSun();
}

bool VisualizerRenderer::isAnimationMode() const
{
  return MODES[styleIndex % MODE_COUNT].draw == &VisualizerRenderer::drawPioneer;
}

void VisualizerRenderer::toggleAnimationPlayback()
{
  if (!openFlag || !isAnimationMode() || !animationOpen)
    return;

  animationPlaying = !animationPlaying;

  // Playback takes the machine. The I2S capture task and the FFT sit on core 0,
  // which is also where the SD reads for the frames land, and a pre-drawn
  // sequence needs neither of them - so the microphone goes off for the
  // duration and the frames get the core to themselves.
  audioVisualizer.setActive(!animationPlaying);

  lastDrawTime = 0;
  drawFrame(true);
}

void VisualizerRenderer::drawFrame(bool force)
{
  if (!micOk)
  {
    if (force)
      drawMicError();
    return;
  }

  uint8_t levels[AudioVisualizer::BAR_COUNT] = {};
  uint8_t waveform[AudioVisualizer::WAVEFORM_COUNT] = {};
  uint8_t overall = 0;
  const uint32_t frameCounter = audioVisualizer.copyFrame(
      levels, AudioVisualizer::BAR_COUNT,
      waveform, AudioVisualizer::WAVEFORM_COUNT,
      &overall);

  const bool frameChanged = frameCounter != lastFrameCounter;
  if (frameChanged)
    lastFrameCounter = frameCounter;

  const ModeDef &mode = MODES[styleIndex % MODE_COUNT];

  // The animation pack holds an 8 KB frame buffer and an SD handle, so it is
  // opened only while its own mode is the one being drawn and handed straight
  // back on the way out. Comparing the draw pointer keeps this out of the mode
  // table, which stays a plain list.
  const bool wantsAnimation = mode.draw == &VisualizerRenderer::drawPioneer;
  if (wantsAnimation && !animationOpen)
  {
    animationOpen = animation.open();
    animationPlaying = false; // always arrives paused, showing frame zero
  }
  else if (!wantsAnimation && animationOpen)
  {
    // Leaving the mode has to hand the microphone back, or every mode after
    // this one would draw a flat line and look broken.
    if (animationPlaying)
    {
      audioVisualizer.setActive(true);
      animationPlaying = false;
    }
    animation.close();
    animationOpen = false;
  }

  if (!force && !frameChanged && !mode.continuous)
    return;

  if (force)
    tft.fillScreen(TFT_BLACK);

  ensureSprite();
  if (force && spriteReady)
    sprite.fillSprite(TFT_BLACK);

  const Frame frame = {levels, waveform, overall};
  (this->*mode.draw)(frame, force);

  if (force)
    drawChrome();
}

// ---------------------------------------------------------------------------
// Mode implementations
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawSpectrumWithPeaks(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int LEFT = 8;
  static constexpr int TOP = 44;
  static constexpr int BASELINE = 230;
  static constexpr int MAX_HEIGHT = BASELINE - TOP;
  static constexpr int GAP = 3;
  static constexpr int BAR_WIDTH = 16;
  static constexpr int STEP = BAR_WIDTH + GAP;
  const unsigned long now = millis();

  beginFrame(tft);
  if (force)
    tft.drawFastHLine(LEFT, BASELINE, 304, theme::VIZ_DEEP);

  for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
  {
    const int x = LEFT + static_cast<int>(i) * STEP;
    int height = map(levels[i], 0, 100, 0, MAX_HEIGHT);
    height = (height / 2) * 2;
    const int oldHeight = lastVisualizerHeight[i];
    const int oldPeak = visualizerPeakHeight[i];

    if (height > oldHeight)
      tft.fillRect(x, BASELINE - height, BAR_WIDTH, height - oldHeight, theme::GREEN);
    else if (height < oldHeight)
      tft.fillRect(x, BASELINE - oldHeight, BAR_WIDTH, oldHeight - height, TFT_BLACK);

    // Restore what belongs underneath the previous peak before moving it.
    if (oldPeak > 0)
    {
      const uint16_t underneath = oldPeak <= height ? theme::GREEN : TFT_BLACK;
      tft.fillRect(x, BASELINE - oldPeak, BAR_WIDTH, 2, underneath);
    }

    int newPeak = oldPeak;
    if (height >= oldPeak)
    {
      newPeak = height;
      visualizerPeakHoldUntil[i] = now + 180;
    }
    else if (timeReached(visualizerPeakHoldUntil[i]) && oldPeak > 0)
    {
      newPeak = max(height, oldPeak - 2);
    }

    if (newPeak > 0)
      tft.fillRect(x, BASELINE - newPeak, BAR_WIDTH, 2, theme::BRIGHT);

    visualizerPeakHeight[i] = newPeak;
    lastVisualizerHeight[i] = height;
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawMirroredSpectrum(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int LEFT = 8;
  static constexpr int CENTRE_Y = 137;
  static constexpr int MAX_HALF_HEIGHT = 88;
  static constexpr int GAP = 3;
  static constexpr int BAR_WIDTH = 16;
  static constexpr int STEP = BAR_WIDTH + GAP;

  beginFrame(tft);
  if (force)
    tft.drawFastHLine(LEFT, CENTRE_Y, 304, theme::VIZ_GLOW);

  for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
  {
    const int x = LEFT + static_cast<int>(i) * STEP;
    int height = map(levels[i], 0, 100, 0, MAX_HALF_HEIGHT);
    height = (height / 2) * 2;
    const int oldHeight = lastVisualizerHeight[i];
    const int difference = abs(height - oldHeight);

    if (height > oldHeight)
    {
      tft.fillRect(x, CENTRE_Y - height, BAR_WIDTH, difference, theme::GREEN);
      tft.fillRect(x, CENTRE_Y + oldHeight + 1, BAR_WIDTH, difference, theme::GREEN);
    }
    else if (height < oldHeight)
    {
      tft.fillRect(x, CENTRE_Y - oldHeight, BAR_WIDTH, difference, TFT_BLACK);
      tft.fillRect(x, CENTRE_Y + height + 1, BAR_WIDTH, difference, TFT_BLACK);
    }
    lastVisualizerHeight[i] = height;
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawOscilloscope(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int LEFT = 8;
  static constexpr int RIGHT = 311;
  static constexpr int TOP = 50;
  static constexpr int BOTTOM = 226;
  static constexpr int CENTRE_Y = (TOP + BOTTOM) / 2;

  int16_t nextY[AudioVisualizer::WAVEFORM_COUNT];
  for (size_t i = 0; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
    nextY[i] = map(waveform[i], 0, 100, BOTTOM, TOP);

  beginFrame(tft);
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

  tft.drawFastHLine(LEFT, CENTRE_Y, RIGHT - LEFT + 1, theme::VIZ_DEEP);
  for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
  {
    const int x0 = LEFT + static_cast<int>((i - 1) * (RIGHT - LEFT) /
                                           (AudioVisualizer::WAVEFORM_COUNT - 1));
    const int x1 = LEFT + static_cast<int>(i * (RIGHT - LEFT) /
                                           (AudioVisualizer::WAVEFORM_COUNT - 1));
    tft.drawLine(x0, nextY[i - 1], x1, nextY[i], theme::GREEN);
    tft.drawLine(x0, nextY[i - 1] + 1, x1, nextY[i] + 1, theme::VIZ_GLOW);
  }
  endFrame(tft);
  present();

  memcpy(lastOscilloscopeY, nextY, sizeof(lastOscilloscopeY));
  oscilloscopeFrameValid = true;
}

void VisualizerRenderer::drawBlockEqualizer(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int LEFT = 8;
  static constexpr int BASELINE = 230;
  static constexpr int GAP = 3;
  static constexpr int BAR_WIDTH = 16;
  static constexpr int STEP = BAR_WIDTH + GAP;
  static constexpr int SEGMENT_COUNT = 12;
  static constexpr int SEGMENT_HEIGHT = 11;
  static constexpr int SEGMENT_GAP = 4;

  beginFrame(tft);
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
        tft.fillRect(x, oldTopY, BAR_WIDTH, SEGMENT_HEIGHT, theme::GREEN);
      }
      for (int segment = oldBlocks; segment < activeBlocks; ++segment)
      {
        const int y = BASELINE - (segment + 1) * (SEGMENT_HEIGHT + SEGMENT_GAP) + SEGMENT_GAP;
        tft.fillRect(x, y, BAR_WIDTH, SEGMENT_HEIGHT, theme::GREEN);
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
      tft.fillRect(x, topY, BAR_WIDTH, SEGMENT_HEIGHT, theme::BRIGHT);
    }
    lastBlockCount[i] = activeBlocks;
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawVuMeter(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int BAR_X = 22;
  static constexpr int BAR_Y = 102;
  static constexpr int BAR_WIDTH = 276;
  static constexpr int BAR_HEIGHT = 28;
  const unsigned long now = millis();
  const int width = map(overall, 0, 100, 0, BAR_WIDTH);

  if (force)
  {
    tft.drawRect(BAR_X - 2, BAR_Y - 2, BAR_WIDTH + 4, BAR_HEIGHT + 4, theme::VIZ_GLOW);
    for (int i = 0; i <= 10; ++i)
    {
      const int x = BAR_X + (BAR_WIDTH * i) / 10;
      tft.drawFastVLine(x, BAR_Y + BAR_HEIGHT + 8, (i % 5 == 0) ? 9 : 5, theme::VIZ_GLOW);
    }
  }

  beginFrame(tft);
  if (width > lastVuWidth)
    tft.fillRect(BAR_X + lastVuWidth, BAR_Y, width - lastVuWidth, BAR_HEIGHT, theme::GREEN);
  else if (width < lastVuWidth)
    tft.fillRect(BAR_X + width, BAR_Y, lastVuWidth - width, BAR_HEIGHT, TFT_BLACK);

  if (vuPeakWidth > 0)
  {
    const uint16_t underneath = vuPeakWidth <= width ? theme::GREEN : TFT_BLACK;
    tft.fillRect(BAR_X + vuPeakWidth - 2, BAR_Y, 2, BAR_HEIGHT, underneath);
  }

  if (width >= vuPeakWidth)
  {
    vuPeakWidth = width;
    vuPeakHoldUntil = now + 220;
  }
  else if (timeReached(vuPeakHoldUntil) && vuPeakWidth > 0)
  {
    vuPeakWidth = max(width, vuPeakWidth - 4);
  }

  if (vuPeakWidth > 0)
    tft.fillRect(BAR_X + vuPeakWidth - 2, BAR_Y, 2, BAR_HEIGHT, theme::BRIGHT);
  endFrame(tft);
  present();
  lastVuWidth = width;
}

void VisualizerRenderer::drawRadialSpectrum(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
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

  beginFrame(tft);

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
  tft.fillCircle(CENTRE_X, CENTRE_Y, coreRadius, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, coreRadius, theme::GREEN);
  tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS, theme::VIZ_GLOW);

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
      const uint16_t color = levels[i] > 72 ? theme::BRIGHT : theme::GREEN;
      for (int offset = -1; offset <= 1; ++offset)
      {
        const int ox = (perpX * offset) / 1000;
        const int oy = (perpY * offset) / 1000;
        tft.drawLine(startX + ox, startY + oy, endX + ox, endY + oy, color);
      }
      tft.fillCircle(endX, endY, 2, theme::BRIGHT);
    }
    lastRadialLength[i] = length;
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawPulseRing(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int CENTRE_X = 160;
  static constexpr int CENTRE_Y = 139;
  static constexpr int INNER_RADIUS = 47;
  static constexpr int START_RADIUS = INNER_RADIUS + 4;
  static constexpr int MAX_LENGTH = 45;

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

  beginFrame(tft);

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

    // The ring used to rotate through three hues. Inside one green family that
    // becomes a brightness rotation, which keeps the banding without the
    // flashing-white effect a near-white step would give.
    uint16_t color = theme::VIZ_MID;
    uint16_t glowColor = theme::VIZ_DEEP;
    const uint8_t colorPhase = i % 12U;
    if (colorPhase >= 4U && colorPhase < 9U)
    {
      color = theme::VIZ_BRIGHT;
      glowColor = theme::VIZ_GLOW;
    }
    else if (colorPhase >= 9U)
    {
      color = theme::VIZ_GLOW;
      glowColor = theme::VIZ_DEEP;
    }
    if (level > 78)
      color = theme::VIZ_PEAK;

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
  tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS, theme::VIZ_GLOW);
  tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS - 2, theme::VIZ_DEEP);
  tft.fillCircle(CENTRE_X, CENTRE_Y, coreRadius - 7, theme::VIZ_CORE);
  tft.drawCircle(CENTRE_X, CENTRE_Y, coreRadius, theme::VIZ_MID);
  tft.drawLine(CENTRE_X, CENTRE_Y - coreRadius,
               CENTRE_X + coreRadius, CENTRE_Y, theme::VIZ_BRIGHT);
  tft.drawLine(CENTRE_X + coreRadius, CENTRE_Y,
               CENTRE_X, CENTRE_Y + coreRadius, theme::VIZ_PEAK);
  tft.drawLine(CENTRE_X, CENTRE_Y + coreRadius,
               CENTRE_X - coreRadius, CENTRE_Y, theme::VIZ_MID);
  tft.drawLine(CENTRE_X - coreRadius, CENTRE_Y,
               CENTRE_X, CENTRE_Y - coreRadius, theme::VIZ_BRIGHT);
  const int innerDiamond = max(8, coreRadius - 11);
  tft.drawLine(CENTRE_X, CENTRE_Y - innerDiamond,
               CENTRE_X + innerDiamond, CENTRE_Y, theme::VIZ_GLOW);
  tft.drawLine(CENTRE_X + innerDiamond, CENTRE_Y,
               CENTRE_X, CENTRE_Y + innerDiamond, theme::VIZ_DEEP);
  tft.drawLine(CENTRE_X, CENTRE_Y + innerDiamond,
               CENTRE_X - innerDiamond, CENTRE_Y, theme::VIZ_DEEP);
  tft.drawLine(CENTRE_X - innerDiamond, CENTRE_Y,
               CENTRE_X, CENTRE_Y - innerDiamond, theme::VIZ_GLOW);
  tft.fillCircle(CENTRE_X, CENTRE_Y, 3, theme::VIZ_BRIGHT);

  endFrame(tft);
  present();
}

void VisualizerRenderer::drawCyberGrid(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int HORIZON_Y = 92;
  static constexpr int BOTTOM_Y = 234;
  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  const int sunRadius = map(overall, 0, 100, 10, 22);
  tft.drawCircle(160, 67, sunRadius, theme::VIZ_GLOW);
  tft.drawCircle(160, 67, max(4, sunRadius - 4), theme::VIZ_DEEP);

  // Audio skyline at the vanishing line.
  for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
  {
    const int x = 8 + static_cast<int>(i) * 19;
    const int height = map(levels[i], 0, 100, 2, 43);
    const uint16_t color = levels[i] > 70 ? theme::VIZ_PEAK : theme::VIZ_GLOW;
    tft.fillRect(x, HORIZON_Y - height, 12, height, theme::VIZ_DEEP);
    tft.drawFastHLine(x, HORIZON_Y - height, 12, color);
  }

  tft.drawFastHLine(0, HORIZON_Y, 320, theme::VIZ_MID);
  for (int ray = 0; ray <= 10; ++ray)
  {
    const int bottomX = 8 + ray * 30;
    const uint8_t level = levels[(ray * 3) % AudioVisualizer::BAR_COUNT];
    tft.drawLine(160, HORIZON_Y, bottomX, BOTTOM_Y,
                 level > 55 ? theme::VIZ_GLOW : theme::VIZ_DEEP);
  }

  const int phase = retroVisualizerFrame % 16U;
  for (int row = 0; row < 10; ++row)
  {
    const int distance = (row * 16 + phase) % 143;
    const int y = HORIZON_Y + (distance * distance) / 143;
    if (y <= BOTTOM_Y)
      tft.drawFastHLine(7, y, 306, y > 190 ? theme::VIZ_GLOW : theme::VIZ_DEEP);
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawLaserTunnel(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int CENTRE_X = 160;
  static constexpr int CENTRE_Y = 139;
  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  for (int layer = 0; layer < 7; ++layer)
  {
    int radius = 18 + ((layer * 17 + retroVisualizerFrame * 2U) % 76U);
    radius += map(levels[(layer * 2) % AudioVisualizer::BAR_COUNT], 0, 100, 0, 6);
    radius = min(radius, 94);
    const uint16_t color = layer % 3 == 0 ? theme::VIZ_BRIGHT
                             : layer % 3 == 1 ? theme::VIZ_MID
                                              : theme::VIZ_GLOW;
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
  tft.fillCircle(CENTRE_X, CENTRE_Y, core, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, core, theme::VIZ_PEAK);
  tft.drawFastHLine(CENTRE_X - core, CENTRE_Y, core * 2 + 1, theme::VIZ_MID);
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawDataRain(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  ++retroVisualizerFrame;
  beginFrame(tft);
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
      const uint16_t color = trail == 0 ? theme::VIZ_BRIGHT
                               : trail == 1 ? theme::VIZ_MID
                               : trail < 4 ? theme::VIZ_GLOW
                                           : theme::VIZ_DEEP;
      const int width = trail == 0 ? 8 : 6;
      tft.fillRect(x + (8 - width) / 2, y, width, 3, color);
    }

    if (levels[column] > 72)
      tft.drawPixel(x + 4, headY - 2, theme::VIZ_PEAK);
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawNeonWave(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int LEFT = 7;
  static constexpr int RIGHT = 312;
  static constexpr int CENTRE_Y = 139;
  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);
  for (int y = 48; y <= 230; y += 12)
    tft.drawFastHLine(0, y, 320, theme::VIZ_DEEP);
  tft.drawFastHLine(0, CENTRE_Y, 320, theme::VIZ_GLOW);

  int previousX = LEFT;
  int previousY = map(waveform[0], 0, 100, 210, 68);
  int previousEchoY = CENTRE_Y * 2 - previousY;
  for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
  {
    const int x = LEFT + static_cast<int>(i) * (RIGHT - LEFT) /
                             (AudioVisualizer::WAVEFORM_COUNT - 1);
    const int y = map(waveform[i], 0, 100, 210, 68);
    const int echoY = CENTRE_Y * 2 - y;
    tft.drawLine(previousX, previousEchoY, x, echoY, theme::VIZ_DEEP);
    tft.drawLine(previousX, previousY + 2, x, y + 2, theme::VIZ_GLOW);
    tft.drawLine(previousX, previousY, x, y,
                 overall > 70 ? theme::VIZ_PEAK : theme::VIZ_BRIGHT);
    previousX = x;
    previousY = y;
    previousEchoY = echoY;
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawOrbitLink(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
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

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 34, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 62, theme::VIZ_GLOW);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 88, theme::VIZ_DEEP);

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
    tft.drawLine(nodeX[i], nodeY[i], nodeX[next], nodeY[next], theme::VIZ_GLOW);
    const uint16_t color = levels[i] > 68 ? theme::VIZ_PEAK : theme::VIZ_MID;
    tft.fillCircle(nodeX[i], nodeY[i], levels[i] > 68 ? 3 : 2, color);
  }

  const int core = map(overall, 0, 100, 8, 19);
  tft.fillCircle(CENTRE_X, CENTRE_Y, core, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, core, theme::VIZ_BRIGHT);
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawPixelCity(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
  static constexpr int BASELINE = 231;
  ++retroVisualizerFrame;
  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);
  tft.drawFastHLine(0, BASELINE, 320, theme::VIZ_MID);

  for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
  {
    const int x = 4 + static_cast<int>(i) * 20;
    const int height = 14 + map(levels[i], 0, 100, 0, 143);
    const int top = BASELINE - height;
    tft.fillRect(x, top, 15, height, theme::VIZ_DEEP);
    tft.drawFastVLine(x, top, height, theme::VIZ_GLOW);
    tft.drawFastVLine(x + 14, top, height, theme::VIZ_GLOW);
    tft.drawFastHLine(x, top, 15, levels[i] > 70 ? theme::VIZ_PEAK : theme::VIZ_MID);

    int windowRow = 0;
    for (int y = BASELINE - 9; y > top + 4; y -= 14, ++windowRow)
    {
      const bool lit = (retroVisualizerFrame + i + windowRow) % 4U != 0U;
      const uint16_t windowColor = lit ? theme::VIZ_GLOW : TFT_BLACK;
      tft.fillRect(x + 3, y, 3, 3, windowColor);
      tft.fillRect(x + 9, y, 3, 3, windowColor);
    }
    if (levels[i] > 82)
      tft.drawFastVLine(x + 7, top - 8, 8, theme::VIZ_BRIGHT);
  }
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawRadar2000(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
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

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 28, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 57, theme::VIZ_GLOW);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 88, theme::VIZ_MID);
  tft.drawFastHLine(CENTRE_X - 88, CENTRE_Y, 177, theme::VIZ_DEEP);
  tft.drawFastVLine(CENTRE_X, CENTRE_Y - 88, 177, theme::VIZ_DEEP);

  const int sweep = retroVisualizerFrame % 32U;
  for (int tail = 2; tail >= 0; --tail)
  {
    const int direction = (sweep + 32 - tail) % 32;
    const int endX = CENTRE_X + (DX[direction] * 87) / 1000;
    const int endY = CENTRE_Y + (DY[direction] * 87) / 1000;
    const uint16_t color = tail == 0 ? theme::VIZ_BRIGHT : tail == 1 ? theme::VIZ_GLOW : theme::VIZ_DEEP;
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
                   levels[i] > 70 ? theme::VIZ_PEAK : theme::VIZ_MID);
  }
  tft.fillCircle(CENTRE_X, CENTRE_Y, map(overall, 0, 100, 2, 6), theme::VIZ_BRIGHT);
  endFrame(tft);
  present();
}

void VisualizerRenderer::drawDualDisc(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
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

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);
  for (int deck = 0; deck < 2; ++deck)
  {
    tft.drawCircle(CENTRE_X[deck], CENTRE_Y, 35, theme::VIZ_DEEP);
    tft.drawCircle(CENTRE_X[deck], CENTRE_Y, 52, theme::VIZ_GLOW);
    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const size_t source = deck == 0 ? i : AudioVisualizer::BAR_COUNT - 1U - i;
      const int direction = (static_cast<int>(i) + (deck == 0 ? rotation : 16 - rotation)) % 16;
      const int radius = 40 + map(levels[source], 0, 100, 0, 19);
      const int startX = CENTRE_X[deck] + (DX[direction] * 36) / 1000;
      const int startY = CENTRE_Y + (DY[direction] * 36) / 1000;
      const int endX = CENTRE_X[deck] + (DX[direction] * radius) / 1000;
      const int endY = CENTRE_Y + (DY[direction] * radius) / 1000;
      const uint16_t color = levels[source] > 68 ? theme::VIZ_PEAK : theme::VIZ_MID;
      tft.drawLine(startX, startY, endX, endY, color);
      if (levels[source] > 58)
        tft.fillCircle(endX, endY, 2, theme::VIZ_BRIGHT);
    }
    const int hub = map(overall, 0, 100, 7, 14);
    tft.fillCircle(CENTRE_X[deck], CENTRE_Y, hub, theme::VIZ_DEEP);
    tft.drawCircle(CENTRE_X[deck], CENTRE_Y, hub, theme::VIZ_BRIGHT);
  }
  tft.drawFastHLine(139, 216, 43, theme::VIZ_DEEP);
  const int sliderX = 139 + map(levels[7], 0, 100, 0, 42);
  tft.fillCircle(sliderX, 216, 3, theme::VIZ_MID);
  endFrame(tft);
  present();
}

uint16_t VisualizerRenderer::waterfallColor(uint8_t level) const
{
  if (level < 12)
    return TFT_BLACK;
  if (level < 32)
    return theme::VIZ_DEEP;
  if (level < 55)
    return theme::VIZ_GLOW;
  if (level < 78)
    return theme::GREEN;
  return theme::BRIGHT;
}

void VisualizerRenderer::drawWaterfall(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;
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
    tft.setTextColor(theme::VIZ_GLOW, TFT_BLACK);
    tft.drawString("BASS", LEFT, 45, 2);
    tft.drawRightString("TREBLE", 312, 45, 2);
    for (int band = 1; band < BAND_COUNT; ++band)
      tft.drawFastVLine(LEFT + band * BAND_WIDTH, TOP, BOTTOM - TOP, theme::VIZ_DEEP);
  }

  // Ten history rows per second are easier to read and much lighter than
  // painting at the full visualizer frame rate.
  if (!force && !elapsed(lastWaterfallAdvanceTime, 100))
    return;
  lastWaterfallAdvanceTime = now;

  if (waterfallWriteY + ROW_STEP > BOTTOM)
  {
    tft.fillRect(LEFT, TOP, BAND_COUNT * BAND_WIDTH, BOTTOM - TOP, TFT_BLACK);
    for (int band = 1; band < BAND_COUNT; ++band)
      tft.drawFastVLine(LEFT + band * BAND_WIDTH, TOP, BOTTOM - TOP, theme::VIZ_DEEP);
    waterfallWriteY = TOP;
  }

  beginFrame(tft);
  for (int band = 0; band < BAND_COUNT; ++band)
  {
    const uint8_t combined = static_cast<uint8_t>(
        (static_cast<uint16_t>(levels[band * 2]) + levels[band * 2 + 1]) / 2U);
    const int x = LEFT + band * BAND_WIDTH;
    tft.fillRect(x + 1, waterfallWriteY, BAND_WIDTH - 2, ROW_HEIGHT,
                 waterfallColor(combined));
  }
  tft.drawFastHLine(LEFT, waterfallWriteY + ROW_HEIGHT,
                    BAND_COUNT * BAND_WIDTH, theme::VIZ_GLOW);
  endFrame(tft);
  present();

  waterfallWriteY += ROW_STEP;
}

// ---------------------------------------------------------------------------
// Modes added in v0.3.17. All four run on their own clock, so they are marked
// continuous in the table and clear their frame each pass.
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawStarfield(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;

  static constexpr int CENTRE_X = 160;
  static constexpr int CENTRE_Y = 139;
  // The overlay is 320 x 200, so a circular field would clip top and bottom.
  // Stars travel on an ellipse instead: full width, 62% of it vertically.
  static constexpr int MAX_RADIUS = 150;
  static constexpr int Y_SQUASH = 62; // percent
  static constexpr int SPAWN_RADIUS = 4;

  ++retroVisualizerFrame;

  // Loudness drives how fast the field rushes past. The floor of 2 keeps the
  // mode alive in a quiet cabin instead of freezing into a still image.
  const int speed = 2 + overall / 14;

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  for (size_t i = 0; i < STAR_COUNT; ++i)
  {
    const int previousRadius = starRadius[i];
    int radius = previousRadius + speed;

    if (radius >= MAX_RADIUS)
    {
      // Respawn near the centre pointing somewhere new.
      radius = SPAWN_RADIUS;
      starAngle[i] = static_cast<uint8_t>((starAngle[i] + 11U + (retroVisualizerFrame & 7U)) & 31U);
      starRadius[i] = static_cast<uint8_t>(radius);
      continue; // no streak across the respawn, it would cut the screen in half
    }
    starRadius[i] = static_cast<uint8_t>(radius);

    const int8_t angle = starAngle[i] & 31;
    const int x = CENTRE_X + (UNIT_DX[angle] * radius) / 1000;
    const int y = CENTRE_Y + (UNIT_DY[angle] * radius * Y_SQUASH) / 100000;
    const int trailX = CENTRE_X + (UNIT_DX[angle] * previousRadius) / 1000;
    const int trailY = CENTRE_Y + (UNIT_DY[angle] * previousRadius * Y_SQUASH) / 100000;

    // Distance stands in for depth: far stars are faint, near ones are bright.
    uint16_t color = theme::VIZ_DEEP;
    if (radius > 118)
      color = theme::VIZ_PEAK;
    else if (radius > 85)
      color = theme::VIZ_BRIGHT;
    else if (radius > 48)
      color = theme::VIZ_MID;
    else if (radius > 24)
      color = theme::VIZ_GLOW;

    tft.drawLine(trailX, trailY, x, y, color);
    if (radius > 85)
      tft.fillCircle(x, y, radius > 126 ? 2 : 1, color);
  }

  // A small core so the vanishing point reads as a point, not an empty hole.
  const int core = map(overall, 0, 100, 2, 7);
  tft.fillCircle(CENTRE_X, CENTRE_Y, core, theme::VIZ_CORE);
  tft.drawCircle(CENTRE_X, CENTRE_Y, core, theme::VIZ_MID);

  endFrame(tft);
  present();
}

void VisualizerRenderer::drawDnaHelix(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;

  static constexpr int LEFT = 10;
  static constexpr int RIGHT = 310;
  static constexpr int CENTRE_Y = 139;
  static constexpr int COLUMN_STEP = 6;
  static constexpr int COLUMNS = (RIGHT - LEFT) / COLUMN_STEP + 1;

  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);
  tft.drawFastHLine(LEFT, CENTRE_Y, RIGHT - LEFT, theme::VIZ_DEEP);

  const uint8_t phase = static_cast<uint8_t>(retroVisualizerFrame & 31U);

  int previousTopY = CENTRE_Y;
  int previousBottomY = CENTRE_Y;

  for (int column = 0; column < COLUMNS; ++column)
  {
    const int x = LEFT + column * COLUMN_STEP;
    const size_t band = static_cast<size_t>(column) % AudioVisualizer::BAR_COUNT;
    const uint8_t level = levels[band];

    // Each strand is one sine, half a turn apart; the band level sets how wide
    // the helix opens at this column.
    const int amplitude = 16 + (level * 62) / 100;
    const uint8_t angle = static_cast<uint8_t>((column * 2U + phase) & 31U);
    const int offset = (UNIT_DX[angle] * amplitude) / 1000;

    const int topY = CENTRE_Y - offset;
    const int bottomY = CENTRE_Y + offset;

    const uint16_t strandColor = level > 62 ? theme::VIZ_BRIGHT : theme::VIZ_MID;

    if (column > 0)
    {
      tft.drawLine(x - COLUMN_STEP, previousTopY, x, topY, strandColor);
      tft.drawLine(x - COLUMN_STEP, previousBottomY, x, bottomY, theme::VIZ_GLOW);
    }

    // Rungs every third column, brightest where the strands are furthest apart.
    if (column % 3 == 0)
    {
      const uint16_t rungColor = level > 70 ? theme::VIZ_PEAK
                                 : level > 34 ? theme::VIZ_GLOW
                                              : theme::VIZ_DEEP;
      tft.drawLine(x, topY, x, bottomY, rungColor);
    }

    tft.fillCircle(x, topY, level > 62 ? 2 : 1, strandColor);
    tft.fillCircle(x, bottomY, level > 62 ? 2 : 1, theme::VIZ_MID);

    previousTopY = topY;
    previousBottomY = bottomY;
  }

  endFrame(tft);
  present();
}

void VisualizerRenderer::drawRipplePool(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;

  static constexpr int CENTRE_X = 160;
  static constexpr int CENTRE_Y = 139;
  // Capped so the outermost ring still fits the 200 px overlay height.
  static constexpr int MAX_RADIUS = 96;
  static constexpr uint8_t SPAWN_THRESHOLD = 34;
  static constexpr uint8_t REARM_THRESHOLD = 20;

  ++retroVisualizerFrame;

  // Low bands only: a ripple should mean a kick, not a cymbal. Edge-triggered
  // through rippleArmed so one long bass note emits one ring, not a wall.
  const uint8_t bassEnergy = static_cast<uint8_t>(
      (static_cast<uint16_t>(levels[0]) + levels[1] + levels[2] + levels[3] + levels[4]) / 5U);

  if (bassEnergy < REARM_THRESHOLD)
    rippleArmed = true;

  if (rippleArmed && bassEnergy >= SPAWN_THRESHOLD)
  {
    for (size_t i = 0; i < RIPPLE_COUNT; ++i)
    {
      if (rippleStrength[i] == 0)
      {
        rippleRadius[i] = 6;
        rippleStrength[i] = bassEnergy;
        rippleArmed = false;
        break;
      }
    }
  }

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  // Still surface: a few faint guide rings so the pool is visible when silent.
  tft.drawCircle(CENTRE_X, CENTRE_Y, 32, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 62, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 92, theme::VIZ_DEEP);

  for (size_t i = 0; i < RIPPLE_COUNT; ++i)
  {
    if (rippleStrength[i] == 0)
      continue;

    const int radius = rippleRadius[i];
    const uint8_t strength = rippleStrength[i];

    const uint16_t color = strength > 66 ? theme::VIZ_PEAK
                           : strength > 40 ? theme::VIZ_BRIGHT
                           : strength > 18 ? theme::VIZ_MID
                                           : theme::VIZ_GLOW;

    tft.drawCircle(CENTRE_X, CENTRE_Y, radius, color);
    if (strength > 40 && radius > 1)
      tft.drawCircle(CENTRE_X, CENTRE_Y, radius - 1, theme::VIZ_GLOW);

    // Expand and fade. Retiring the ring at the edge frees the slot.
    const int nextRadius = radius + 4;
    const int nextStrength = strength - 4;
    if (nextRadius > MAX_RADIUS || nextStrength <= 0)
    {
      rippleRadius[i] = 0;
      rippleStrength[i] = 0;
    }
    else
    {
      rippleRadius[i] = static_cast<uint8_t>(nextRadius);
      rippleStrength[i] = static_cast<uint8_t>(nextStrength);
    }
  }

  // The drop that makes the rings: sized by overall level.
  const int drop = map(overall, 0, 100, 3, 12);
  tft.fillCircle(CENTRE_X, CENTRE_Y, drop, theme::VIZ_CORE);
  tft.drawCircle(CENTRE_X, CENTRE_Y, drop, theme::VIZ_BRIGHT);

  endFrame(tft);
  present();
}

void VisualizerRenderer::drawPhaseScope(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels __attribute__((unused)) = frame.levels;
  const uint8_t *waveform __attribute__((unused)) = frame.waveform;
  const uint8_t overall __attribute__((unused)) = frame.overall;

  static constexpr int CENTRE_X = 160;
  static constexpr int CENTRE_Y = 139;
  static constexpr size_t DELAY = AudioVisualizer::WAVEFORM_COUNT / 4; // quarter turn
  static constexpr int X_GAIN = 24; // /10, so 2.4x
  static constexpr int Y_GAIN = 17; // /10, so 1.7x

  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, 40, 320, 200, TFT_BLACK);

  // Graticule.
  tft.drawFastHLine(48, CENTRE_Y, 224, theme::VIZ_DEEP);
  tft.drawFastVLine(CENTRE_X, 56, 166, theme::VIZ_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 76, theme::VIZ_DEEP);

  // Plotting the trace against a quarter-period-delayed copy of itself turns a
  // steady tone into a stable loop and noise into a scribble, which is exactly
  // the read a phase scope is for.
  int previousX = 0;
  int previousY = 0;

  for (size_t i = 0; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
  {
    const int sampleA = static_cast<int>(waveform[i]) - AudioSpectrum::WAVE_CENTRE;
    const int sampleB =
        static_cast<int>(waveform[(i + DELAY) % AudioVisualizer::WAVEFORM_COUNT]) -
        AudioSpectrum::WAVE_CENTRE;

    const int x = CENTRE_X + (sampleA * X_GAIN) / 10;
    const int y = CENTRE_Y + (sampleB * Y_GAIN) / 10;

    if (i > 0)
    {
      // Brighten along the trace so the direction of travel is readable.
      const uint16_t color = i > 46 ? theme::VIZ_PEAK
                             : i > 30 ? theme::VIZ_BRIGHT
                             : i > 14 ? theme::VIZ_MID
                                      : theme::VIZ_GLOW;
      tft.drawLine(previousX, previousY, x, y, color);
    }

    previousX = x;
    previousY = y;
  }

  // Close the loop so a steady tone reads as one continuous shape.
  const int firstSampleA = static_cast<int>(waveform[0]) - AudioSpectrum::WAVE_CENTRE;
  const int firstSampleB =
      static_cast<int>(waveform[DELAY]) - AudioSpectrum::WAVE_CENTRE;
  tft.drawLine(previousX, previousY,
               CENTRE_X + (firstSampleA * X_GAIN) / 10,
               CENTRE_Y + (firstSampleB * Y_GAIN) / 10, theme::VIZ_GLOW);

  tft.fillCircle(CENTRE_X, CENTRE_Y, 2, theme::VIZ_MID);

  endFrame(tft);
  present();
}
