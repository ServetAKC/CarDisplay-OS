#include "visualizerRenderer.h"

#include <string.h>

#include "spotifyLogic.h"
#include "timing.h"

// The one list of visualizer modes. Order here is the order the on-screen
// "n/16" counter and the tap-to-cycle sequence follow.
const VisualizerRenderer::ModeDef VisualizerRenderer::MODES[VisualizerRenderer::MODE_COUNT] = {
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
};

namespace
{
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
  for (size_t i = 0; i < MODE_COUNT; ++i)
  {
    if (strcmp(MODES[i].name, DEFAULT_STYLE_NAME) == 0)
    {
      styleIndex = static_cast<uint8_t>(i);
      break;
    }
  }

  micOk = audioVisualizer.begin();
  if (!micOk)
    Serial.println(F("Visualizer will show MIC ERROR until I2S setup is fixed"));
  return micOk;
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
}

void VisualizerRenderer::nextStyle()
{
  if (!openFlag)
    return;

  styleIndex = static_cast<uint8_t>((styleIndex + 1U) % MODE_COUNT);
  resetDrawingState();
  lastDrawTime = 0;
  drawFrame(true);
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
  if (!openFlag || !elapsed(lastDrawTime, FRAME_INTERVAL_MS))
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

  // 320 x 200 at 8 bpp is 64 KB, which is a large slice of the ESP32 heap once
  // three TLS sessions are up. The queue-prefetch client is idle for as long as
  // the overlay is open, so hand its session back first; that is often the
  // difference between a buffered frame and falling back to direct draw.
  spotifyReleaseQueueConnection();

  sprite.setColorDepth(8);
  if (sprite.createSprite(layout::SCREEN_WIDTH, layout::OVERLAY_HEIGHT) == nullptr)
  {
    Serial.println(F("Visualizer frame buffer allocation failed; using direct draw"));
    return false;
  }

  // Draw functions keep normal screen coordinates; content starts at y = 40.
  sprite.setOrigin(0, -layout::OVERLAY_TOP);
  sprite.setTextWrap(false);
  spriteReady = true;
  return true;
}

void VisualizerRenderer::releaseSprite()
{
  if (!spriteReady)
    return;
  sprite.deleteSprite();
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
  char label[32];
  snprintf(label, sizeof(label), "%u/%u  %s",
           static_cast<unsigned>(styleIndex) + 1U,
           static_cast<unsigned>(MODE_COUNT),
           styleName());
  tft.fillRect(0, 0, 278, layout::OVERLAY_TOP, TFT_BLACK);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawString(label, 8, 12, 2);
  drawOverlayCloseButton();
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
    tft.drawFastHLine(LEFT, BASELINE, 304, theme::DARK);

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
    tft.drawFastHLine(LEFT, CENTRE_Y, 304, theme::DIM);

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

  tft.drawFastHLine(LEFT, CENTRE_Y, RIGHT - LEFT + 1, theme::DARK);
  for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
  {
    const int x0 = LEFT + static_cast<int>((i - 1) * (RIGHT - LEFT) /
                                           (AudioVisualizer::WAVEFORM_COUNT - 1));
    const int x1 = LEFT + static_cast<int>(i * (RIGHT - LEFT) /
                                           (AudioVisualizer::WAVEFORM_COUNT - 1));
    tft.drawLine(x0, nextY[i - 1], x1, nextY[i], theme::GREEN);
    tft.drawLine(x0, nextY[i - 1] + 1, x1, nextY[i] + 1, theme::DIM);
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
    tft.drawRect(BAR_X - 2, BAR_Y - 2, BAR_WIDTH + 4, BAR_HEIGHT + 4, theme::DIM);
    for (int i = 0; i <= 10; ++i)
    {
      const int x = BAR_X + (BAR_WIDTH * i) / 10;
      tft.drawFastVLine(x, BAR_Y + BAR_HEIGHT + 8, (i % 5 == 0) ? 9 : 5, theme::DIM);
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
  tft.fillCircle(CENTRE_X, CENTRE_Y, coreRadius, theme::DARK);
  tft.drawCircle(CENTRE_X, CENTRE_Y, coreRadius, theme::GREEN);
  tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS, theme::DIM);

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

    uint16_t color = theme::Y2K_NEON;
    uint16_t glowColor = theme::Y2K_DEEP;
    const uint8_t colorPhase = i % 12U;
    if (colorPhase >= 4U && colorPhase < 9U)
    {
      color = theme::Y2K_LIME;
      glowColor = theme::Y2K_GLOW;
    }
    else if (colorPhase >= 9U)
    {
      color = theme::Y2K_MINT;
      glowColor = theme::Y2K_GLOW;
    }
    if (level > 78)
      color = theme::Y2K_MINT;

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
  tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS, theme::Y2K_GLOW);
  tft.drawCircle(CENTRE_X, CENTRE_Y, INNER_RADIUS - 2, theme::Y2K_DEEP);
  tft.fillCircle(CENTRE_X, CENTRE_Y, coreRadius - 7, theme::CORE_DARK);
  tft.drawCircle(CENTRE_X, CENTRE_Y, coreRadius, theme::Y2K_NEON);
  tft.drawLine(CENTRE_X, CENTRE_Y - coreRadius,
               CENTRE_X + coreRadius, CENTRE_Y, theme::Y2K_MINT);
  tft.drawLine(CENTRE_X + coreRadius, CENTRE_Y,
               CENTRE_X, CENTRE_Y + coreRadius, theme::Y2K_LIME);
  tft.drawLine(CENTRE_X, CENTRE_Y + coreRadius,
               CENTRE_X - coreRadius, CENTRE_Y, theme::Y2K_NEON);
  tft.drawLine(CENTRE_X - coreRadius, CENTRE_Y,
               CENTRE_X, CENTRE_Y - coreRadius, theme::Y2K_MINT);
  const int innerDiamond = max(8, coreRadius - 11);
  tft.drawLine(CENTRE_X, CENTRE_Y - innerDiamond,
               CENTRE_X + innerDiamond, CENTRE_Y, theme::Y2K_GLOW);
  tft.drawLine(CENTRE_X + innerDiamond, CENTRE_Y,
               CENTRE_X, CENTRE_Y + innerDiamond, theme::Y2K_DEEP);
  tft.drawLine(CENTRE_X, CENTRE_Y + innerDiamond,
               CENTRE_X - innerDiamond, CENTRE_Y, theme::Y2K_DEEP);
  tft.drawLine(CENTRE_X - innerDiamond, CENTRE_Y,
               CENTRE_X, CENTRE_Y - innerDiamond, theme::Y2K_GLOW);
  tft.fillCircle(CENTRE_X, CENTRE_Y, 3, theme::Y2K_MINT);

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
  tft.drawCircle(160, 67, sunRadius, theme::Y2K_GLOW);
  tft.drawCircle(160, 67, max(4, sunRadius - 4), theme::Y2K_DEEP);

  // Audio skyline at the vanishing line.
  for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
  {
    const int x = 8 + static_cast<int>(i) * 19;
    const int height = map(levels[i], 0, 100, 2, 43);
    const uint16_t color = levels[i] > 70 ? theme::Y2K_LIME : theme::Y2K_GLOW;
    tft.fillRect(x, HORIZON_Y - height, 12, height, theme::Y2K_DEEP);
    tft.drawFastHLine(x, HORIZON_Y - height, 12, color);
  }

  tft.drawFastHLine(0, HORIZON_Y, 320, theme::Y2K_NEON);
  for (int ray = 0; ray <= 10; ++ray)
  {
    const int bottomX = 8 + ray * 30;
    const uint8_t level = levels[(ray * 3) % AudioVisualizer::BAR_COUNT];
    tft.drawLine(160, HORIZON_Y, bottomX, BOTTOM_Y,
                 level > 55 ? theme::Y2K_GLOW : theme::Y2K_DEEP);
  }

  const int phase = retroVisualizerFrame % 16U;
  for (int row = 0; row < 10; ++row)
  {
    const int distance = (row * 16 + phase) % 143;
    const int y = HORIZON_Y + (distance * distance) / 143;
    if (y <= BOTTOM_Y)
      tft.drawFastHLine(7, y, 306, y > 190 ? theme::Y2K_GLOW : theme::Y2K_DEEP);
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
    const uint16_t color = layer % 3 == 0 ? theme::Y2K_MINT
                             : layer % 3 == 1 ? theme::Y2K_NEON
                                              : theme::Y2K_GLOW;
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
  tft.fillCircle(CENTRE_X, CENTRE_Y, core, theme::Y2K_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, core, theme::Y2K_LIME);
  tft.drawFastHLine(CENTRE_X - core, CENTRE_Y, core * 2 + 1, theme::Y2K_NEON);
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
      const uint16_t color = trail == 0 ? theme::Y2K_MINT
                               : trail == 1 ? theme::Y2K_NEON
                               : trail < 4 ? theme::Y2K_GLOW
                                           : theme::Y2K_DEEP;
      const int width = trail == 0 ? 8 : 6;
      tft.fillRect(x + (8 - width) / 2, y, width, 3, color);
    }

    if (levels[column] > 72)
      tft.drawPixel(x + 4, headY - 2, theme::Y2K_LIME);
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
    tft.drawFastHLine(0, y, 320, theme::Y2K_DEEP);
  tft.drawFastHLine(0, CENTRE_Y, 320, theme::Y2K_GLOW);

  int previousX = LEFT;
  int previousY = map(waveform[0], 0, 100, 210, 68);
  int previousEchoY = CENTRE_Y * 2 - previousY;
  for (size_t i = 1; i < AudioVisualizer::WAVEFORM_COUNT; ++i)
  {
    const int x = LEFT + static_cast<int>(i) * (RIGHT - LEFT) /
                             (AudioVisualizer::WAVEFORM_COUNT - 1);
    const int y = map(waveform[i], 0, 100, 210, 68);
    const int echoY = CENTRE_Y * 2 - y;
    tft.drawLine(previousX, previousEchoY, x, echoY, theme::Y2K_DEEP);
    tft.drawLine(previousX, previousY + 2, x, y + 2, theme::Y2K_GLOW);
    tft.drawLine(previousX, previousY, x, y,
                 overall > 70 ? theme::Y2K_LIME : theme::Y2K_MINT);
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
  tft.drawCircle(CENTRE_X, CENTRE_Y, 34, theme::Y2K_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 62, theme::Y2K_GLOW);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 88, theme::Y2K_DEEP);

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
    tft.drawLine(nodeX[i], nodeY[i], nodeX[next], nodeY[next], theme::Y2K_GLOW);
    const uint16_t color = levels[i] > 68 ? theme::Y2K_LIME : theme::Y2K_NEON;
    tft.fillCircle(nodeX[i], nodeY[i], levels[i] > 68 ? 3 : 2, color);
  }

  const int core = map(overall, 0, 100, 8, 19);
  tft.fillCircle(CENTRE_X, CENTRE_Y, core, theme::Y2K_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, core, theme::Y2K_MINT);
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
  tft.drawFastHLine(0, BASELINE, 320, theme::Y2K_NEON);

  for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
  {
    const int x = 4 + static_cast<int>(i) * 20;
    const int height = 14 + map(levels[i], 0, 100, 0, 143);
    const int top = BASELINE - height;
    tft.fillRect(x, top, 15, height, theme::Y2K_DEEP);
    tft.drawFastVLine(x, top, height, theme::Y2K_GLOW);
    tft.drawFastVLine(x + 14, top, height, theme::Y2K_GLOW);
    tft.drawFastHLine(x, top, 15, levels[i] > 70 ? theme::Y2K_LIME : theme::Y2K_NEON);

    int windowRow = 0;
    for (int y = BASELINE - 9; y > top + 4; y -= 14, ++windowRow)
    {
      const bool lit = (retroVisualizerFrame + i + windowRow) % 4U != 0U;
      const uint16_t windowColor = lit ? theme::Y2K_GLOW : TFT_BLACK;
      tft.fillRect(x + 3, y, 3, 3, windowColor);
      tft.fillRect(x + 9, y, 3, 3, windowColor);
    }
    if (levels[i] > 82)
      tft.drawFastVLine(x + 7, top - 8, 8, theme::Y2K_MINT);
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
  tft.drawCircle(CENTRE_X, CENTRE_Y, 28, theme::Y2K_DEEP);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 57, theme::Y2K_GLOW);
  tft.drawCircle(CENTRE_X, CENTRE_Y, 88, theme::Y2K_NEON);
  tft.drawFastHLine(CENTRE_X - 88, CENTRE_Y, 177, theme::Y2K_DEEP);
  tft.drawFastVLine(CENTRE_X, CENTRE_Y - 88, 177, theme::Y2K_DEEP);

  const int sweep = retroVisualizerFrame % 32U;
  for (int tail = 2; tail >= 0; --tail)
  {
    const int direction = (sweep + 32 - tail) % 32;
    const int endX = CENTRE_X + (DX[direction] * 87) / 1000;
    const int endY = CENTRE_Y + (DY[direction] * 87) / 1000;
    const uint16_t color = tail == 0 ? theme::Y2K_MINT : tail == 1 ? theme::Y2K_GLOW : theme::Y2K_DEEP;
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
                   levels[i] > 70 ? theme::Y2K_LIME : theme::Y2K_NEON);
  }
  tft.fillCircle(CENTRE_X, CENTRE_Y, map(overall, 0, 100, 2, 6), theme::Y2K_MINT);
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
    tft.drawCircle(CENTRE_X[deck], CENTRE_Y, 35, theme::Y2K_DEEP);
    tft.drawCircle(CENTRE_X[deck], CENTRE_Y, 52, theme::Y2K_GLOW);
    for (size_t i = 0; i < AudioVisualizer::BAR_COUNT; ++i)
    {
      const size_t source = deck == 0 ? i : AudioVisualizer::BAR_COUNT - 1U - i;
      const int direction = (static_cast<int>(i) + (deck == 0 ? rotation : 16 - rotation)) % 16;
      const int radius = 40 + map(levels[source], 0, 100, 0, 19);
      const int startX = CENTRE_X[deck] + (DX[direction] * 36) / 1000;
      const int startY = CENTRE_Y + (DY[direction] * 36) / 1000;
      const int endX = CENTRE_X[deck] + (DX[direction] * radius) / 1000;
      const int endY = CENTRE_Y + (DY[direction] * radius) / 1000;
      const uint16_t color = levels[source] > 68 ? theme::Y2K_LIME : theme::Y2K_NEON;
      tft.drawLine(startX, startY, endX, endY, color);
      if (levels[source] > 58)
        tft.fillCircle(endX, endY, 2, theme::Y2K_MINT);
    }
    const int hub = map(overall, 0, 100, 7, 14);
    tft.fillCircle(CENTRE_X[deck], CENTRE_Y, hub, theme::Y2K_DEEP);
    tft.drawCircle(CENTRE_X[deck], CENTRE_Y, hub, theme::Y2K_MINT);
  }
  tft.drawFastHLine(139, 216, 43, theme::Y2K_DEEP);
  const int sliderX = 139 + map(levels[7], 0, 100, 0, 42);
  tft.fillCircle(sliderX, 216, 3, theme::Y2K_NEON);
  endFrame(tft);
  present();
}

uint16_t VisualizerRenderer::waterfallColor(uint8_t level) const
{
  if (level < 12)
    return TFT_BLACK;
  if (level < 32)
    return theme::DARK;
  if (level < 55)
    return theme::DIM;
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
    tft.setTextColor(theme::DIM, TFT_BLACK);
    tft.drawString("BASS", LEFT, 45, 2);
    tft.drawRightString("TREBLE", 312, 45, 2);
    for (int band = 1; band < BAND_COUNT; ++band)
      tft.drawFastVLine(LEFT + band * BAND_WIDTH, TOP, BOTTOM - TOP, theme::DARK);
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
      tft.drawFastVLine(LEFT + band * BAND_WIDTH, TOP, BOTTOM - TOP, theme::DARK);
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
                    BAND_COUNT * BAND_WIDTH, theme::DIM);
  endFrame(tft);
  present();

  waterfallWriteY += ROW_STEP;
}
