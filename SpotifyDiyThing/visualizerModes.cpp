// ---------------------------------------------------------------------------
// The twenty visualizer modes added in v0.4.
//
// They live in their own translation unit because visualizerRenderer.cpp was
// already 1487 lines with twenty modes in it, and doubling that would have
// undone the v0.3.16 split. The lifecycle, the frame buffer and the dispatch
// table all still live next door; this file only contains draw functions.
//
// House rules, same as the v0.3 modes:
//   - canvas() is the target, never the global `tft`. It is the off-screen
//     sprite when the allocation succeeded and the panel when it did not.
//   - beginFrame()/endFrame() bracket the drawing, present() pushes the sprite.
//   - Colour comes from theme::VIZ_* or theme::vizHue(). Never a literal.
//   - Integer maths only. The FFT already owns the float unit for this frame.
// ---------------------------------------------------------------------------

#include "visualizerRenderer.h"

#include <string.h>

#include "timing.h"
#include "visualizerGeometry.h"

using vizgeom::cos1000;
using vizgeom::sin1000;
using vizgeom::UNIT_DX;
using vizgeom::UNIT_DY;

namespace
{
constexpr int SCENE_TOP = layout::OVERLAY_TOP;         // 40
constexpr int SCENE_BOTTOM = layout::SCREEN_HEIGHT - 1; // 239
constexpr int SCENE_HEIGHT = layout::OVERLAY_HEIGHT;    // 200
constexpr int SCENE_CENTRE_Y = SCENE_TOP + SCENE_HEIGHT / 2;
constexpr int SCENE_CENTRE_X = layout::CENTRE_X;

constexpr size_t BANDS = AudioVisualizer::BAR_COUNT;      // 16
constexpr size_t WAVE_POINTS = AudioVisualizer::WAVEFORM_COUNT; // 64

// A leaping dolphin, facing right, drawn as art rather than geometry.
//
// Pioneer head units have had one arcing over the spectrum analyser since the
// 90s and that is what was asked for. No arrangement of circles and triangles
// reads as a dolphin at this size, so it is a sprite. Rows may be any length -
// drawing stops at the terminator - so trailing transparent columns are simply
// left off, which is also why the tail rows are short.
const char *const DOLPHIN_ART[] = {
    ".................####",
    "................######",
    "...............########",
    "..............##########.....######",
    "............##############.#########",
    "..........############################",
    ".........##############################",
    "##.......###############################",
    "###.....################################",
    "####...#################################",
    "#####..#################################",
    "######..###############################",
    "#####..##############################",
    "####...###########################",
    "###....######################",
    "##.....################...####",
    ".......############.....#####",
    "........#########........####",
    "..........#####...........##",
};
constexpr int DOLPHIN_ROWS = static_cast<int>(sizeof(DOLPHIN_ART) / sizeof(DOLPHIN_ART[0]));
constexpr int DOLPHIN_WIDTH = 40;

// Where the eye sits in the art above, measured from the nose end so that the
// horizontal flip moves it with the head instead of leaving it behind.
constexpr int DOLPHIN_EYE_FROM_NOSE = 7;
constexpr int DOLPHIN_EYE_ROW = 5;

// Average of a slice of the spectrum, 0..100. Several modes want "how loud is
// the bass" without caring which exact bin carried it.
uint8_t bandAverage(const uint8_t *levels, size_t first, size_t last)
{
  if (first > last || last >= BANDS)
    return 0;
  unsigned total = 0;
  for (size_t i = first; i <= last; ++i)
    total += levels[i];
  return static_cast<uint8_t>(total / (last - first + 1));
}

// Clamp helper. The modes below compute coordinates from audio, and audio can
// briefly saturate; clipping here is cheaper than letting TFT_eSPI reject a
// whole primitive that ran one pixel past the edge.
int clampInt(int value, int low, int high)
{
  return value < low ? low : (value > high ? high : value);
}
} // namespace

// ---------------------------------------------------------------------------
// DOLPHIN
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawDolphin(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;
  const uint8_t overall = frame.overall;

  // The sea line. Bars grow down from it so the spectrum reads as water rather
  // than as a chart the dolphin happens to be standing on.
  constexpr int WATER_Y = 196;
  constexpr int TRAVEL_LEFT = 6;
  constexpr int TRAVEL_SPAN = 268;

  // Loudness sets how high the next leap goes. Smoothed hard on the way down so
  // the dolphin does not belly-flop between two quiet frames.
  const int targetArc = 26 + (overall * 92) / 100;
  if (targetArc > dolphinArc)
    dolphinArc = static_cast<uint8_t>(dolphinArc + (targetArc - dolphinArc) / 2 + 1);
  else if (dolphinArc > 26)
    dolphinArc = static_cast<uint8_t>(dolphinArc - 1);

  // Advance along the leap. The floor keeps the animation alive in a silent
  // cabin; without it the dolphin freezes mid-air and reads as a crash.
  const int speed = 3 + overall / 18;
  int phase = dolphinPhase + dolphinDirection * speed;
  if (phase >= 255)
  {
    phase = 255;
    dolphinDirection = -1;
    dolphinSplash = 10;
  }
  else if (phase <= 0)
  {
    phase = 0;
    dolphinDirection = 1;
    dolphinSplash = 10;
  }
  dolphinPhase = static_cast<uint8_t>(phase);

  // Parabolic leap: zero at both ends, dolphinArc at the midpoint.
  const int height = (dolphinArc * phase * (255 - phase)) / 16256;

  const int noseX = TRAVEL_LEFT + (phase * TRAVEL_SPAN) / 255;
  const int bodyBottom = WATER_Y + 14 - height;
  const int bodyTop = bodyBottom - DOLPHIN_ROWS;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  // Sky: a faint horizon so the leap has something to be above.
  tft.drawFastHLine(0, WATER_Y, layout::SCREEN_WIDTH, theme::VIZ_GLOW);

  // The dolphin, flipped to face the way it is travelling.
  const bool facingRight = dolphinDirection > 0;
  for (int row = 0; row < DOLPHIN_ROWS; ++row)
  {
    const int y = bodyTop + row;
    if (y < SCENE_TOP || y > SCENE_BOTTOM)
      continue;

    const char *art = DOLPHIN_ART[row];
    for (int col = 0; art[col] != '\0'; ++col)
    {
      if (art[col] != '#')
        continue;

      // The art is drawn nose-right, so `col` counts back from the nose. Facing
      // left mirrors that about noseX, which keeps the nose leading in both
      // directions instead of the dolphin swimming backwards on the return leg.
      const int fromNose = DOLPHIN_WIDTH - 1 - col;
      const int x = facingRight ? (noseX - fromNose) : (noseX + fromNose);
      if (x < 0 || x >= layout::SCREEN_WIDTH)
        continue;

      // Above the water the dolphin is lit; the part still submerged is drawn
      // in the water colour so it reads as being under the surface instead of
      // being clipped off.
      tft.drawPixel(x, y, y < WATER_Y ? theme::VIZ_BRIGHT : theme::VIZ_GLOW);
    }
  }

  // Eye, punched out after the body so it is not overwritten by it.
  const int eyeRowY = bodyTop + DOLPHIN_EYE_ROW;
  const int eyeX = noseX + (facingRight ? -DOLPHIN_EYE_FROM_NOSE : DOLPHIN_EYE_FROM_NOSE);
  if (eyeRowY > SCENE_TOP && eyeRowY < WATER_Y && eyeX > 0 && eyeX < layout::SCREEN_WIDTH - 1)
  {
    tft.fillRect(eyeX, eyeRowY, 2, 2, TFT_BLACK);
    tft.drawPixel(eyeX, eyeRowY, theme::VIZ_PEAK);
  }

  // Splash rings where the dolphin broke the surface.
  if (dolphinSplash > 0)
  {
    const int splashX = dolphinDirection > 0 ? TRAVEL_LEFT : TRAVEL_LEFT + TRAVEL_SPAN;
    const int radius = (10 - dolphinSplash) * 4 + 3;
    tft.drawFastHLine(clampInt(splashX - radius, 0, 319), WATER_Y - 1,
                      clampInt(radius * 2, 1, 320), theme::VIZ_PEAK);
    tft.drawFastHLine(clampInt(splashX - radius / 2, 0, 319), WATER_Y - 3,
                      clampInt(radius, 1, 320), theme::VIZ_BRIGHT);
    --dolphinSplash;
  }

  // The sea itself: the spectrum, hanging down from the water line.
  for (size_t i = 0; i < BANDS; ++i)
  {
    const int depth = 6 + (levels[i] * 40) / 100;
    const int x = 8 + static_cast<int>(i) * 19;
    tft.fillRect(x, WATER_Y + 1, 16, depth, theme::vizBandHue(i, BANDS));
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// SPECTRUM ARC
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawSpectrumArc(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // Bars stand on a semicircle instead of a line, so the low and high ends of
  // the spectrum are equally far from the eye. The centre sits below the scene
  // so the arc fills the width without clipping the tall middle bars.
  constexpr int ORIGIN_Y = 232;
  constexpr int INNER_RADIUS = 52;
  constexpr int MAX_LENGTH = 96;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i < BANDS; ++i)
  {
    // Index 24 points left, 0 points up, 8 points right, so counting UP from 24
    // and wrapping walks the upper half of the circle left to right. Counting
    // down from 24 walks the lower half, which would put every bar under the
    // bottom edge.
    const int index = (24 + static_cast<int>((i * 16) / BANDS)) & 31;
    const int length = INNER_RADIUS + (levels[i] * MAX_LENGTH) / 100;

    const int x0 = SCENE_CENTRE_X + (UNIT_DX[index] * INNER_RADIUS) / 1000;
    const int y0 = ORIGIN_Y + (UNIT_DY[index] * INNER_RADIUS) / 1000;
    const int x1 = SCENE_CENTRE_X + (UNIT_DX[index] * length) / 1000;
    const int y1 = ORIGIN_Y + (UNIT_DY[index] * length) / 1000;

    const uint16_t colour = theme::vizBandHue(i, BANDS);
    tft.drawLine(x0, y0, x1, y1, colour);
    tft.drawLine(x0 + 1, y0, x1 + 1, y1, colour);
    tft.drawLine(x0 - 1, y0, x1 - 1, y1, colour);
    tft.fillCircle(x1, y1, 2, theme::VIZ_PEAK);
  }

  tft.drawCircle(SCENE_CENTRE_X, ORIGIN_Y, INNER_RADIUS - 2, theme::VIZ_DEEP);
  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// TWIN TOWERS
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawTwinTowers(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;
  const uint8_t *waveform = frame.waveform;

  // Two stacked-segment columns, bass on the left and treble on the right, with
  // the raw waveform threaded between them. It is the layout of a rack-mount
  // level meter, which is where the segment gap comes from.
  constexpr int SEGMENTS = 18;
  constexpr int SEGMENT_HEIGHT = 9;
  constexpr int SEGMENT_GAP = 2;
  constexpr int TOWER_WIDTH = 40;
  constexpr int LEFT_X = 12;
  constexpr int RIGHT_X = layout::SCREEN_WIDTH - 12 - TOWER_WIDTH;
  constexpr int BASE_Y = 232;

  const int bass = bandAverage(levels, 0, BANDS / 2 - 1);
  const int treble = bandAverage(levels, BANDS / 2, BANDS - 1);

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (int side = 0; side < 2; ++side)
  {
    const int x = side == 0 ? LEFT_X : RIGHT_X;
    const int level = side == 0 ? bass : treble;
    const int lit = (level * SEGMENTS) / 100;

    for (int segment = 0; segment < SEGMENTS; ++segment)
    {
      const int y = BASE_Y - (segment + 1) * (SEGMENT_HEIGHT + SEGMENT_GAP);
      // Unlit segments stay visible as structure. They are the one place a dim
      // tone is wanted, and it is a cyan one.
      const uint16_t colour =
          segment < lit ? theme::vizHue(static_cast<uint8_t>((segment * 255) / SEGMENTS))
                        : theme::VIZ_DEEP;
      tft.fillRect(x, y, TOWER_WIDTH, SEGMENT_HEIGHT, colour);
    }
  }

  // Waveform down the middle, running vertically so it does not fight the
  // towers for the horizontal axis.
  constexpr int TRACE_X = SCENE_CENTRE_X;
  int previousX = TRACE_X;
  int previousY = SCENE_TOP + 4;
  for (size_t i = 0; i < WAVE_POINTS; ++i)
  {
    const int y = SCENE_TOP + 4 + static_cast<int>((i * (SCENE_HEIGHT - 8)) / WAVE_POINTS);
    const int x = TRACE_X + ((static_cast<int>(waveform[i]) - 50) * 68) / 50;
    tft.drawLine(previousX, previousY, x, y, theme::VIZ_BRIGHT);
    previousX = x;
    previousY = y;
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// WAVE TUNNEL
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawWaveTunnel(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // Concentric rings whose radii come from the spectrum, redrawn every frame so
  // the whole tunnel breathes at once. Rotating the ring assignment by the frame
  // counter is what makes it read as travel rather than as a pulsing target.
  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t ring = 0; ring < BANDS; ++ring)
  {
    const size_t band = (ring + retroVisualizerFrame / 3) % BANDS;
    const int base = 8 + static_cast<int>(ring) * 6;
    const int radius = base + (levels[band] * 22) / 100;
    if (radius >= 100)
      continue;

    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((ring * 255) / BANDS));
    // Squashed vertically: the scene is 320 x 200, so true circles would waste
    // the width and clip the height.
    tft.drawEllipse(SCENE_CENTRE_X, SCENE_CENTRE_Y, radius + radius / 2, radius, colour);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// MIRROR X
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawMirrorX(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // The mirrored spectrum turned on its side: bands run top to bottom and grow
  // out from the centre line. Reads very differently from MIRRORED even though
  // the data is identical, which is the cheapest kind of new mode there is.
  constexpr int ROW_HEIGHT = 10;
  constexpr int ROW_GAP = 2;
  constexpr int MAX_HALF = 148;
  const int top = SCENE_TOP + (SCENE_HEIGHT - static_cast<int>(BANDS) * (ROW_HEIGHT + ROW_GAP)) / 2;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);
  tft.drawFastVLine(SCENE_CENTRE_X, SCENE_TOP + 2, SCENE_HEIGHT - 4, theme::VIZ_DEEP);

  for (size_t i = 0; i < BANDS; ++i)
  {
    const int y = top + static_cast<int>(i) * (ROW_HEIGHT + ROW_GAP);
    const int half = (levels[i] * MAX_HALF) / 100;
    if (half <= 0)
      continue;

    const uint16_t colour = theme::vizBandHue(i, BANDS);
    tft.fillRect(SCENE_CENTRE_X - half, y, half, ROW_HEIGHT, colour);
    tft.fillRect(SCENE_CENTRE_X + 1, y, half, ROW_HEIGHT, colour);
    tft.fillRect(SCENE_CENTRE_X - half - 2, y, 2, ROW_HEIGHT, theme::VIZ_PEAK);
    tft.fillRect(SCENE_CENTRE_X + half + 1, y, 2, ROW_HEIGHT, theme::VIZ_PEAK);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// COMET TRAIL
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawCometTrail(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t overall = frame.overall;

  // One bright head dragging a tail of fading dots around an ellipse. Loudness
  // drives both the speed and how long the tail is, so a quiet passage leaves a
  // short comet drifting rather than a still dot.
  constexpr int RADIUS_X = 132;
  constexpr int RADIUS_Y = 78;
  constexpr int MAX_TAIL = 22;

  retroVisualizerFrame = static_cast<uint16_t>(retroVisualizerFrame + 3 + overall / 16);
  const int tail = 6 + (overall * (MAX_TAIL - 6)) / 100;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  tft.drawEllipse(SCENE_CENTRE_X, SCENE_CENTRE_Y, RADIUS_X, RADIUS_Y, theme::VIZ_DEEP);

  for (int step = tail; step >= 0; --step)
  {
    const uint8_t angle = static_cast<uint8_t>(retroVisualizerFrame - step * 4);
    const int x = SCENE_CENTRE_X + (cos1000(angle) * RADIUS_X) / 1000;
    const int y = SCENE_CENTRE_Y + (sin1000(angle) * RADIUS_Y) / 1000;

    // Position in the tail picks the hue, so the comet fades green to cyan
    // rather than green to black.
    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((step * 255) / (tail + 1)));
    const int size = step == 0 ? 5 : (step < 4 ? 3 : 2);
    tft.fillCircle(x, y, size, step == 0 ? theme::VIZ_PEAK : colour);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// EQ STAIRS
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawEqStairs(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // The spectrum drawn as a solid staircase silhouette instead of separate
  // bars, with a bright tread on each step. Fills the frame, so it stays
  // readable across a car cabin in a way sixteen thin bars do not.
  constexpr int BASELINE = 234;
  constexpr int MAX_HEIGHT = 180;
  constexpr int STEP_WIDTH = layout::SCREEN_WIDTH / static_cast<int>(BANDS);

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i < BANDS; ++i)
  {
    const int height = (levels[i] * MAX_HEIGHT) / 100;
    const int x = static_cast<int>(i) * STEP_WIDTH;
    if (height > 0)
      tft.fillRect(x, BASELINE - height, STEP_WIDTH, height, theme::vizBandHue(i, BANDS));
    // The tread. Drawn even at zero height so the staircase never disappears.
    tft.fillRect(x, BASELINE - height - 3, STEP_WIDTH, 3, theme::VIZ_PEAK);
  }

  tft.drawFastHLine(0, BASELINE + 1, layout::SCREEN_WIDTH, theme::VIZ_GLOW);
  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// SONAR SWEEP
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawSonarSweep(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;
  const uint8_t overall = frame.overall;

  // A rotating sweep line over range rings, with contacts painted where the
  // spectrum is loud. RADAR 2000 sweeps a wedge; this one is the thin-line
  // version with rings that expand on the beat, so the two do not collide.
  constexpr int MAX_RADIUS = 94;
  retroVisualizerFrame = static_cast<uint16_t>(retroVisualizerFrame + 4);

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (int ring = 1; ring <= 4; ++ring)
  {
    const int radius = (MAX_RADIUS * ring) / 4;
    tft.drawEllipse(SCENE_CENTRE_X, SCENE_CENTRE_Y, radius + radius / 2, radius,
                    ring == 4 ? theme::VIZ_GLOW : theme::VIZ_DEEP);
  }
  tft.drawFastHLine(SCENE_CENTRE_X - MAX_RADIUS * 3 / 2, SCENE_CENTRE_Y,
                    MAX_RADIUS * 3, theme::VIZ_DEEP);
  tft.drawFastVLine(SCENE_CENTRE_X, SCENE_CENTRE_Y - MAX_RADIUS,
                    MAX_RADIUS * 2, theme::VIZ_DEEP);

  // Contacts: one per band, placed at its own bearing, distance from the level.
  for (size_t i = 0; i < BANDS; ++i)
  {
    if (levels[i] < 12)
      continue;
    const int index = static_cast<int>((i * 32) / BANDS) & 31;
    const int radius = 14 + (levels[i] * (MAX_RADIUS - 18)) / 100;
    const int x = SCENE_CENTRE_X + (UNIT_DX[index] * radius * 3) / 2000;
    const int y = SCENE_CENTRE_Y + (UNIT_DY[index] * radius) / 1000;
    tft.fillCircle(x, y, 3, theme::vizBandHue(i, BANDS));
  }

  // The sweep line last, so it passes over the contacts.
  const uint8_t angle = static_cast<uint8_t>(retroVisualizerFrame);
  const int reach = MAX_RADIUS + (overall * 24) / 100;
  const int endX = SCENE_CENTRE_X + (cos1000(angle) * reach * 3) / 2000;
  const int endY = SCENE_CENTRE_Y + (sin1000(angle) * reach) / 1000;
  tft.drawLine(SCENE_CENTRE_X, SCENE_CENTRE_Y, endX, endY, theme::VIZ_PEAK);
  tft.fillCircle(SCENE_CENTRE_X, SCENE_CENTRE_Y, 3, theme::VIZ_BRIGHT);

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// FREQ RIBBON
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawFreqRibbon(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // The spectrum as one continuous filled ribbon rather than sixteen islands.
  // Each band contributes a trapezium between its own height and its
  // neighbour's, which is what removes the staircase edge EQ STAIRS keeps.
  constexpr int CENTRE = SCENE_CENTRE_Y;
  constexpr int MAX_HALF = 86;
  constexpr int SEGMENT = layout::SCREEN_WIDTH / static_cast<int>(BANDS - 1);

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i + 1 < BANDS; ++i)
  {
    const int leftHalf = (levels[i] * MAX_HALF) / 100;
    const int rightHalf = (levels[i + 1] * MAX_HALF) / 100;
    const int x0 = static_cast<int>(i) * SEGMENT;
    const int x1 = x0 + SEGMENT;
    const uint16_t colour = theme::vizBandHue(i, BANDS);

    // Two triangles per segment make the quad. fillTriangle is the only filled
    // primitive TFT_eSPI offers that is not axis aligned.
    tft.fillTriangle(x0, CENTRE - leftHalf, x1, CENTRE - rightHalf, x0, CENTRE + leftHalf, colour);
    tft.fillTriangle(x1, CENTRE - rightHalf, x1, CENTRE + rightHalf, x0, CENTRE + leftHalf, colour);
    tft.drawLine(x0, CENTRE - leftHalf, x1, CENTRE - rightHalf, theme::VIZ_PEAK);
    tft.drawLine(x0, CENTRE + leftHalf, x1, CENTRE + rightHalf, theme::VIZ_PEAK);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// VOICE PRINT
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawVoicePrint(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // A spectrogram written one column at a time with a wrapping write head, the
  // same trick WATERFALL uses for rows. Scrolling the whole buffer every frame
  // would cost a full-screen copy at 24 FPS; moving the write head costs one
  // column.
  // 256 columns, not 312: the write head is a uint8_t, so a wider span would
  // wrap at 256 anyway and leave a stripe down the right side that nothing ever
  // wrote to. Centring an honest 256 is better than clipping a dishonest 312.
  constexpr int LEFT = 32;
  constexpr int WIDTH = 256;
  constexpr int TOP = 46;
  constexpr int ROW_HEIGHT = 11;

  beginFrame(tft);
  if (force)
  {
    tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);
    scrollColumn = 0;
  }

  const int x = LEFT + scrollColumn;
  for (size_t i = 0; i < BANDS; ++i)
  {
    // Low bands at the bottom, the way a spectrogram is normally read.
    const int y = TOP + static_cast<int>(BANDS - 1 - i) * ROW_HEIGHT;
    const uint16_t colour = levels[i] < 8
                                ? TFT_BLACK
                                : theme::vizHue(static_cast<uint8_t>((levels[i] * 255) / 100));
    tft.fillRect(x, y, 2, ROW_HEIGHT - 1, colour);
  }

  // The write head, one column ahead, so the newest data has a visible edge.
  const int headX = LEFT + ((scrollColumn + 2) % WIDTH);
  tft.drawFastVLine(headX, TOP, static_cast<int>(BANDS) * ROW_HEIGHT, theme::VIZ_PEAK);

  scrollColumn = static_cast<uint8_t>((scrollColumn + 2) % WIDTH);
  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// HEX PULSE
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawHexPulse(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // Nested hexagons, one per band, each sized by its own level. Drawn as six
  // line segments from the direction table rather than as a polygon, because
  // TFT_eSPI has no polygon primitive and six drawLine calls are cheaper than
  // building one.
  constexpr int RINGS = 8;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (int ring = RINGS - 1; ring >= 0; --ring)
  {
    const size_t band = static_cast<size_t>(ring) * BANDS / RINGS;
    const int radius = 12 + ring * 11 + (levels[band] * 16) / 100;
    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((ring * 255) / RINGS));

    int previousX = 0;
    int previousY = 0;
    for (int corner = 0; corner <= 6; ++corner)
    {
      // Six corners means every sixth of the circle; the 32-step table has no
      // exact sixth, so step by 5.33 and round - the error is under a pixel at
      // this radius.
      const int index = ((corner * 16) / 3) & 31;
      const int x = SCENE_CENTRE_X + (UNIT_DX[index] * radius * 3) / 2000;
      const int y = SCENE_CENTRE_Y + (UNIT_DY[index] * radius) / 1000;
      if (corner > 0)
        tft.drawLine(previousX, previousY, x, y, colour);
      previousX = x;
      previousY = y;
    }
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// LED MATRIX
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawLedMatrix(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // A dot-matrix panel: sixteen columns of twelve LEDs, lit from the bottom.
  // Unlit dots stay on screen as dim cyan so the panel reads as hardware that
  // is switched on rather than as a spectrum floating in the dark.
  constexpr int ROWS = 12;
  constexpr int DOT = 5;
  constexpr int COL_STEP = 19;
  constexpr int ROW_STEP = 15;
  constexpr int LEFT = 14;
  constexpr int TOP = 52;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t column = 0; column < BANDS; ++column)
  {
    const int lit = (levels[column] * ROWS) / 100;
    const int x = LEFT + static_cast<int>(column) * COL_STEP;

    for (int row = 0; row < ROWS; ++row)
    {
      const int y = TOP + (ROWS - 1 - row) * ROW_STEP;
      const bool on = row < lit;
      const uint16_t colour =
          on ? theme::vizHue(static_cast<uint8_t>((row * 255) / ROWS)) : theme::VIZ_CORE;
      tft.fillCircle(x, y, on ? DOT : 2, colour);
    }
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// SPIRAL ARM
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawSpiralArm(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;
  const uint8_t overall = frame.overall;

  // An Archimedean spiral of dots whose radius is pushed outward by whichever
  // band that turn of the spiral belongs to. Rotating the whole thing on the
  // frame counter turns a static plot into an arm sweeping past.
  constexpr int POINTS = 96;
  retroVisualizerFrame = static_cast<uint16_t>(retroVisualizerFrame + 2 + overall / 24);

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (int point = 0; point < POINTS; ++point)
  {
    const uint8_t angle = static_cast<uint8_t>(retroVisualizerFrame + point * 9);
    const size_t band = static_cast<size_t>(point) * BANDS / POINTS;
    const int radius = 6 + point + (levels[band] * 30) / 100;
    if (radius > 96)
      continue;

    const int x = SCENE_CENTRE_X + (cos1000(angle) * radius * 3) / 2000;
    const int y = SCENE_CENTRE_Y + (sin1000(angle) * radius) / 1000;
    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((point * 255) / POINTS));
    tft.fillCircle(x, y, point > POINTS - 12 ? 3 : 2, colour);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// BOUNCE BALLS
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawBounceBalls(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // One ball per band under constant gravity, kicked upward when its band goes
  // loud. The physics is what makes it different from a bar chart: the ball
  // keeps rising after the transient has passed, so a snare reads as a throw
  // rather than as a spike.
  constexpr int FLOOR = 228;
  constexpr int CEILING = 52;
  constexpr int GRAVITY = 2;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);
  tft.drawFastHLine(0, FLOOR + 7, layout::SCREEN_WIDTH, theme::VIZ_GLOW);

  for (size_t i = 0; i < BANDS; ++i)
  {
    int height = bounceHeight[i];
    int velocity = bounceVelocity[i];

    // A loud band sets a floor under the launch velocity rather than adding to
    // it, so a sustained note does not accelerate the ball off the top.
    const int kick = (levels[i] * 17) / 100;
    if (kick > velocity && height < 24)
      velocity = kick;

    height += velocity;
    velocity -= GRAVITY;

    if (height <= 0)
    {
      height = 0;
      velocity = 0;
    }
    else if (height > FLOOR - CEILING)
    {
      height = FLOOR - CEILING;
      velocity = 0;
    }

    bounceHeight[i] = static_cast<uint8_t>(height);
    bounceVelocity[i] = static_cast<int8_t>(clampInt(velocity, -127, 127));

    const int x = 18 + static_cast<int>(i) * 19;
    const int y = FLOOR - height;
    tft.fillCircle(x, y, 6, theme::vizBandHue(i, BANDS));
    tft.drawCircle(x, y, 6, theme::VIZ_PEAK);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// SCAN LINES
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawScanLines(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *waveform = frame.waveform;

  // Horizontal lines whose width is taken from the waveform, stacked up the
  // screen. Reads like a CRT test pattern reacting to the room, and it is the
  // only mode that shows all sixty-four waveform points as separate rows.
  constexpr int ROW_STEP = 3;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i < WAVE_POINTS; ++i)
  {
    const int y = SCENE_TOP + 4 + static_cast<int>(i) * ROW_STEP;
    if (y > SCENE_BOTTOM - 2)
      break;

    // Distance from the centre line is the amplitude, so silence collapses to a
    // narrow stripe instead of to nothing.
    const int amplitude = abs(static_cast<int>(waveform[i]) - 50);
    const int half = 8 + (amplitude * 148) / 50;
    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((i * 255) / WAVE_POINTS));
    tft.drawFastHLine(clampInt(SCENE_CENTRE_X - half, 0, 319), y,
                      clampInt(half * 2, 1, 320), colour);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// KALEIDO
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawKaleido(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // One quadrant of spectrum spokes, mirrored into all four. Four-way symmetry
  // is what turns sixteen bars into a pattern; it is also four times the draw
  // calls, which is why the spokes are lines and not filled wedges.
  constexpr int MAX_LENGTH = 96;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i < BANDS; ++i)
  {
    const int index = static_cast<int>((i * 8) / BANDS) & 31;
    const int length = 10 + (levels[i] * MAX_LENGTH) / 100;
    const int dx = (UNIT_DX[index] * length * 3) / 2000;
    const int dy = (UNIT_DY[index] * length) / 1000;
    const uint16_t colour = theme::vizBandHue(i, BANDS);

    // The four reflections of one spoke.
    tft.drawLine(SCENE_CENTRE_X, SCENE_CENTRE_Y, SCENE_CENTRE_X + dx, SCENE_CENTRE_Y + dy, colour);
    tft.drawLine(SCENE_CENTRE_X, SCENE_CENTRE_Y, SCENE_CENTRE_X - dx, SCENE_CENTRE_Y + dy, colour);
    tft.drawLine(SCENE_CENTRE_X, SCENE_CENTRE_Y, SCENE_CENTRE_X + dx, SCENE_CENTRE_Y - dy, colour);
    tft.drawLine(SCENE_CENTRE_X, SCENE_CENTRE_Y, SCENE_CENTRE_X - dx, SCENE_CENTRE_Y - dy, colour);

    tft.fillCircle(SCENE_CENTRE_X + dx, SCENE_CENTRE_Y + dy, 2, theme::VIZ_PEAK);
    tft.fillCircle(SCENE_CENTRE_X - dx, SCENE_CENTRE_Y + dy, 2, theme::VIZ_PEAK);
    tft.fillCircle(SCENE_CENTRE_X + dx, SCENE_CENTRE_Y - dy, 2, theme::VIZ_PEAK);
    tft.fillCircle(SCENE_CENTRE_X - dx, SCENE_CENTRE_Y - dy, 2, theme::VIZ_PEAK);
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// NEEDLE GAUGE
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawNeedleGauge(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t overall = frame.overall;

  // An analogue VU movement: a needle with real ballistics and a peak needle
  // that falls back slowly. VU METER is the bar version; this is the dial, and
  // the slow return is the entire character of it.
  constexpr int PIVOT_Y = 218;
  constexpr int NEEDLE_LENGTH = 132;
  constexpr int SWEEP = 200; // deflection units, 0 = rest, SWEEP = full scale

  // In the 256-step circle, 160 points up-left, 192 straight up and 224
  // up-right. Mapping the deflection onto 160..224 keeps the needle in the
  // upper half where a meter movement lives; the obvious 192 + x runs it
  // straight through the pivot and out the bottom of the case.
  constexpr int ANGLE_REST = 160;
  constexpr int ANGLE_SPAN = 64;
  const unsigned long now = millis();

  // Map loudness onto the dial, then approach it. Rising fast, falling slow, in
  // the proportion a real VU movement uses.
  const int target = (overall * SWEEP) / 100;
  if (target > gaugeAngle)
    gaugeAngle += (target - gaugeAngle + 1) / 2;
  else
    gaugeAngle -= (gaugeAngle - target + 5) / 6;
  gaugeAngle = clampInt(gaugeAngle, 0, SWEEP);

  if (gaugeAngle >= gaugePeakAngle)
  {
    gaugePeakAngle = gaugeAngle;
    gaugePeakHoldUntil = now + 700;
  }
  else if (timeReached(gaugePeakHoldUntil) && gaugePeakAngle > 0)
  {
    --gaugePeakAngle;
  }

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  // Scale: eleven ticks across the sweep, the top three in the peak colour the
  // way the red zone is marked on a real meter.
  for (int tick = 0; tick <= 10; ++tick)
  {
    const int position = (tick * SWEEP) / 10;
    const uint8_t angle = static_cast<uint8_t>(ANGLE_REST + (position * ANGLE_SPAN) / SWEEP);
    const int outerX = SCENE_CENTRE_X + (cos1000(angle) * NEEDLE_LENGTH) / 1000;
    const int outerY = PIVOT_Y + (sin1000(angle) * NEEDLE_LENGTH) / 1000;
    const int innerX = SCENE_CENTRE_X + (cos1000(angle) * (NEEDLE_LENGTH - 14)) / 1000;
    const int innerY = PIVOT_Y + (sin1000(angle) * (NEEDLE_LENGTH - 14)) / 1000;
    tft.drawLine(innerX, innerY, outerX, outerY,
                 tick >= 8 ? theme::VIZ_PEAK : theme::VIZ_GLOW);
  }

  const uint8_t needleAngle = static_cast<uint8_t>(ANGLE_REST + (gaugeAngle * ANGLE_SPAN) / SWEEP);
  const int tipX = SCENE_CENTRE_X + (cos1000(needleAngle) * (NEEDLE_LENGTH - 6)) / 1000;
  const int tipY = PIVOT_Y + (sin1000(needleAngle) * (NEEDLE_LENGTH - 6)) / 1000;

  const uint8_t peakAngle = static_cast<uint8_t>(ANGLE_REST + (gaugePeakAngle * ANGLE_SPAN) / SWEEP);
  const int peakX = SCENE_CENTRE_X + (cos1000(peakAngle) * (NEEDLE_LENGTH - 6)) / 1000;
  const int peakY = PIVOT_Y + (sin1000(peakAngle) * (NEEDLE_LENGTH - 6)) / 1000;
  tft.drawLine(SCENE_CENTRE_X, PIVOT_Y, peakX, peakY, theme::VIZ_DEEP);

  // Three parallel lines give the needle weight without a filled polygon.
  tft.drawLine(SCENE_CENTRE_X, PIVOT_Y, tipX, tipY, theme::VIZ_BRIGHT);
  tft.drawLine(SCENE_CENTRE_X - 1, PIVOT_Y, tipX, tipY, theme::VIZ_BRIGHT);
  tft.drawLine(SCENE_CENTRE_X + 1, PIVOT_Y, tipX, tipY, theme::VIZ_BRIGHT);
  tft.fillCircle(SCENE_CENTRE_X, PIVOT_Y, 7, theme::VIZ_MID);
  tft.fillCircle(SCENE_CENTRE_X, PIVOT_Y, 3, theme::VIZ_PEAK);

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// PARTICLE JET
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawParticleJet(const Frame &frame, bool force)
{
  TFT_eSPI &tft = canvas();
  const uint8_t overall = frame.overall;

  // Particles thrown up from the bottom edge, faster when the room is louder.
  // Seeded deterministically on force so the field is already spread on the
  // first frame instead of erupting from one point.
  if (force)
  {
    for (size_t i = 0; i < JET_COUNT; ++i)
    {
      jetX[i] = static_cast<uint8_t>((i * 53U) % 160U);
      jetY[i] = static_cast<uint8_t>((i * 97U) % 200U);
      jetSpeed[i] = static_cast<uint8_t>(2 + (i % 5));
    }
  }

  const int push = 1 + overall / 20;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i < JET_COUNT; ++i)
  {
    int y = jetY[i] + jetSpeed[i] + push;
    if (y >= SCENE_HEIGHT)
    {
      // Respawn at the nozzle on a new column.
      y = 0;
      jetX[i] = static_cast<uint8_t>((jetX[i] + 37U + (retroVisualizerFrame & 15U)) % 160U);
      jetSpeed[i] = static_cast<uint8_t>(2 + ((i + retroVisualizerFrame) % 5));
    }
    jetY[i] = static_cast<uint8_t>(y);

    // y counts up from the nozzle; the screen counts down to it.
    const int screenY = SCENE_BOTTOM - y;
    const int screenX = 0 + jetX[i] * 2;
    // Height above the nozzle picks the hue, so a particle cools green to cyan
    // as it rises without ever going dark.
    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((y * 255) / SCENE_HEIGHT));
    tft.fillCircle(screenX, screenY, y > 150 ? 1 : 2, colour);
    tft.drawFastVLine(screenX, screenY, 3, colour);
  }

  ++retroVisualizerFrame;
  tft.drawFastHLine(0, SCENE_BOTTOM - 1, layout::SCREEN_WIDTH, theme::VIZ_GLOW);
  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// WAVE GRID
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawWaveGrid(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *waveform = frame.waveform;

  // A perspective grid whose rows are displaced by the waveform. CYBER GRID
  // scrolls a flat floor; this one deforms it, so the surface reads as a sheet
  // the sound is passing through.
  constexpr int ROWS = 9;
  constexpr int COLUMNS = 12;
  constexpr int HORIZON_Y = 70;

  ++retroVisualizerFrame;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (int row = 0; row < ROWS; ++row)
  {
    // Rows bunch up toward the horizon. Squaring the row index is the whole
    // perspective model and it is close enough at this size.
    const int depth = (row + 1) * (row + 1);
    const int baseY = HORIZON_Y + (depth * 158) / (ROWS * ROWS);
    const int spread = 20 + (depth * 300) / (ROWS * ROWS);
    const uint16_t colour = theme::vizHue(static_cast<uint8_t>((row * 255) / ROWS));

    int previousX = 0;
    int previousY = 0;
    for (int column = 0; column <= COLUMNS; ++column)
    {
      const size_t sample = (static_cast<size_t>(column * 5 + row * 7 + retroVisualizerFrame)) % WAVE_POINTS;
      const int displacement = ((static_cast<int>(waveform[sample]) - 50) * (row + 2)) / 14;
      const int x = SCENE_CENTRE_X + ((column - COLUMNS / 2) * spread) / (COLUMNS / 2);
      const int y = clampInt(baseY + displacement, SCENE_TOP + 1, SCENE_BOTTOM - 1);

      if (column > 0)
        tft.drawLine(previousX, previousY, x, y, colour);
      previousX = x;
      previousY = y;
    }
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// CHROMA RINGS
// ---------------------------------------------------------------------------

void VisualizerRenderer::drawChromaRings(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  // One ring per band, thickness from the level. WAVE TUNNEL moves its rings;
  // these stay put and change weight instead, which makes the band-to-hue
  // mapping legible - it is the mode to look at to understand the palette.
  constexpr int INNER = 10;
  constexpr int STEP = 6;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  for (size_t i = 0; i < BANDS; ++i)
  {
    const int radius = INNER + static_cast<int>(i) * STEP;
    const int thickness = 1 + (levels[i] * 4) / 100;
    const uint16_t colour = theme::vizBandHue(i, BANDS);

    for (int pass = 0; pass < thickness; ++pass)
    {
      const int r = radius + pass;
      tft.drawEllipse(SCENE_CENTRE_X, SCENE_CENTRE_Y, r + r / 2, r, colour);
    }
  }

  endFrame(tft);
  present();
}

// ---------------------------------------------------------------------------
// PIONEER
//
// The one mode that is not procedural. Everything above reads the FFT and draws
// geometry; this plays a pre-drawn frame sequence off the SD card, which is how
// the Pioneer head units did their dolphins and race cars and is the only way
// to get that look. AnimationPlayer owns the format and the pacing.
// ---------------------------------------------------------------------------

// The play / stop button, and the only control in the middle of any overlay.
// It has to be drawn where touchScreen.cpp's ANIMATION_PLAY_ZONE tests, so both
// numbers are written once here and once there and must agree.
void VisualizerRenderer::drawAnimationPlayButton(TFT_eSPI &target)
{
  constexpr int X = 110;
  constexpr int Y = 138;
  constexpr int W = 100;
  constexpr int H = 64;
  const int cx = X + W / 2;
  const int cy = Y + H / 2;

  if (animationPlaying)
  {
    // While it plays the frame is the point, so the control shrinks to a small
    // stop square out of the way in the corner. Tapping the middle still stops
    // it, the way a video player behaves.
    target.drawRect(8, SCENE_BOTTOM - 18, 14, 14, theme::VIZ_DEEP);
    target.fillRect(11, SCENE_BOTTOM - 15, 8, 8, theme::VIZ_GLOW);
    return;
  }

  target.fillRect(X, Y, W, H, TFT_BLACK);
  target.drawRoundRect(X, Y, W, H, 8, theme::VIZ_MID);
  target.drawRoundRect(X + 1, Y + 1, W - 2, H - 2, 7, theme::VIZ_DEEP);

  // A filled play triangle. fillTriangle is the only non-axis-aligned filled
  // primitive TFT_eSPI has, which is exactly one more than this needs.
  target.fillTriangle(cx - 10, cy - 14, cx - 10, cy + 14, cx + 16, cy,
                      theme::VIZ_BRIGHT);
}

void VisualizerRenderer::drawPioneer(const Frame &frame, bool force)
{
  (void)force;
  TFT_eSPI &tft = canvas();
  const uint8_t *levels = frame.levels;

  beginFrame(tft);
  tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);

  if (!animationOpen)
  {
    // Say what is missing rather than showing a black rectangle. Getting a pack
    // onto the card is a five step job and any one of them can be the one that
    // was skipped, so name the path and let the card be checked.
    tft.setTextColor(theme::VIZ_BRIGHT, TFT_BLACK);
    tft.drawCentreString("NO ANIMATION", SCENE_CENTRE_X, 96, 4);
    tft.setTextColor(theme::VIZ_GLOW, TFT_BLACK);
    tft.drawCentreString("Put a .anm pack in /anim", SCENE_CENTRE_X, 132, 2);
    tft.drawCentreString("on the SD card", SCENE_CENTRE_X, 152, 2);
    tft.setTextColor(theme::VIZ_DEEP, TFT_BLACK);
    tft.drawCentreString("tools/make_animation.py builds one", SCENE_CENTRE_X, 180, 1);
    endFrame(tft);
    present();
    return;
  }

  // Centred horizontally, sitting at the top of the scene. A pack shorter than
  // the scene leaves room along the bottom, which is where the spectrum goes -
  // the arrangement the originals used.
  const int width = animation.frameWidth();
  const int height = animation.frameHeight();
  const int originX = (layout::SCREEN_WIDTH - width) / 2;
  const int spare = SCENE_HEIGHT - height;

  bool alive = true;
  if (animationPlaying)
  {
    alive = animation.service(tft, originX, SCENE_TOP);
  }
  else
  {
    // Paused: the loaded frame stands as a poster behind the play button, and
    // the microphone is still running, so the spectrum below it still moves.
    animation.drawCurrent(tft, originX, SCENE_TOP);
  }

  if (!alive)
  {
    tft.fillRect(0, SCENE_TOP, layout::SCREEN_WIDTH, SCENE_HEIGHT, TFT_BLACK);
    tft.setTextColor(theme::VIZ_BRIGHT, TFT_BLACK);
    tft.drawCentreString("SD CARD LOST", SCENE_CENTRE_X, 120, 4);
    endFrame(tft);
    present();
    return;
  }

  // The spectrum only while paused. During playback the microphone is off, so
  // the levels are a frozen frame of silence and drawing them would be a lie
  // painted over somebody's artwork.
  if (!animationPlaying && spare >= 28)
  {
    const int baseline = SCENE_BOTTOM - 2;
    const int maxHeight = spare - 8;
    for (size_t i = 0; i < BANDS; ++i)
    {
      const int barHeight = (levels[i] * maxHeight) / 100;
      if (barHeight <= 0)
        continue;
      tft.fillRect(8 + static_cast<int>(i) * 19, baseline - barHeight, 16,
                   barHeight, theme::vizBandHue(i, BANDS));
    }
  }

  drawAnimationPlayButton(tft);
  endFrame(tft);
  present();
}
