#pragma once

#include <TFT_eSPI.h>

// ---------------------------------------------------------------------------
// Shared palette, screen layout and hardware pins for the Cheap Yellow Display.
//
// These used to be private constants inside one 3400-line class, which meant the
// album cache, the visualizer and the player screen could not be separated
// without duplicating them. Collecting them here is what makes the split
// possible - and it is now the one place to retune the look.
// ---------------------------------------------------------------------------

// The single TFT_eSPI instance. Defined in cheapYellowLCD.cpp; the display
// driver must only ever be touched from the Arduino loop task.
extern TFT_eSPI tft;

namespace theme
{
// Pack 8-bit RGB into the panel's 16-bit format. constexpr so every colour
// below is still a compile-time constant.
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Green "Honda" palette used by the player UI. Untouched since v0.3: the player
// screen, the header, the clock and the setup screens are meant to look exactly
// as they always have. Only the visualizer ramp below was retuned in v0.4.
constexpr uint16_t GREEN = 0x4EE6;
constexpr uint16_t BRIGHT = 0x9FF0;
constexpr uint16_t PRESS = 0xDFFB;          // pale mint highlight for prev/next taps
constexpr uint16_t DIM = 0x2363;            // secondary text
constexpr uint16_t DARK = 0x1A22;           // baselines and frames
constexpr uint16_t PROGRESS_TRACK = 0x2A65; // dark muted green, unplayed line

// ---------------------------------------------------------------------------
// Visualizer ramp, least to most energetic. This is the only part of the
// palette v0.4 changed, and it is used only by the microphone modes.
//
// v0.3.14 gave the retro modes their own Y2K palette (mint, lime), which read as
// a different product. v0.3.17 unified them onto one green - but did it by
// varying brightness, so every mode's low end landed on dark green.
//
// v0.4 varies HUE instead: energy travels cyan -> spring green -> neon green,
// and only the two structural steps are allowed to be dim. No mode ever draws a
// dark green, and depth still reads because the hue moves.
//
// The modes used to reach for DIM and DARK above for their own grid lines and
// baselines, which is how the player palette leaked into them. They use
// VIZ_GLOW and VIZ_DEEP now, so the two palettes are genuinely independent and
// retuning one cannot touch the other.
// ---------------------------------------------------------------------------
constexpr uint16_t VIZ_CORE = rgb(0, 26, 30);      // near-black core fill, teal cast
constexpr uint16_t VIZ_DEEP = rgb(0, 105, 115);    // faint grid lines, clearly cyan
constexpr uint16_t VIZ_GLOW = rgb(0, 168, 190);    // background structure, mid cyan
constexpr uint16_t VIZ_MID = rgb(38, 235, 175);    // the workhorse, spring green
constexpr uint16_t VIZ_BRIGHT = rgb(90, 255, 130); // active / loud, neon green
constexpr uint16_t VIZ_PEAK = rgb(190, 255, 240);  // peaks and highlights, mint

// Continuous green -> cyan ramp for the modes that colour by band index, depth
// or velocity rather than by discrete energy step. t = 0 is neon green, t = 255
// is pure cyan; every value in between keeps G at 235 or above, so no input can
// produce a dark colour. Modes that want depth vary `t`, not brightness.
inline uint16_t vizHue(uint8_t t)
{
  const int r = (60 * (255 - t)) / 255;
  const int g = 235 + (20 * t) / 255;
  const int b = 110 + (145 * t) / 255;
  return rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b));
}

// Same ramp, addressed by band index instead of a 0..255 position. Bass reads
// green, treble reads cyan, which gives the spectrum modes a second dimension
// without a second palette.
inline uint16_t vizBandHue(size_t band, size_t bandCount)
{
  if (bandCount < 2)
    return vizHue(0);
  return vizHue(static_cast<uint8_t>((band * 255U) / (bandCount - 1)));
}
} // namespace theme

namespace layout
{
constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;
constexpr int CENTRE_X = SCREEN_WIDTH / 2;

constexpr int HEADER_HEIGHT = 30;

constexpr int CONTENT_Y = 34;
constexpr int CONTENT_HEIGHT = 152;

constexpr int IMAGE_X = 8; // aligned with PROGRESS_X
constexpr int IMAGE_Y = 34;
constexpr int IMAGE_SIZE = 160;

constexpr int TEXT_X = 174;
constexpr int TEXT_WIDTH = 138;

constexpr int PROGRESS_X = 8;
constexpr int PROGRESS_Y = 197;
constexpr int PROGRESS_WIDTH = 304;
constexpr int PROGRESS_HEIGHT = 2;
constexpr int PROGRESS_TIME_X = 202;
constexpr int PROGRESS_TIME_Y = 178;
constexpr int PROGRESS_TIME_WIDTH = 110;
constexpr int PROGRESS_TIME_HEIGHT = 17;

constexpr int CONTROLS_Y = 218;
constexpr int CONTROLS_CLEAR_Y = 205;
constexpr int CONTROLS_CLEAR_HEIGHT = 28;

// Full-screen overlays (clock, visualizer) own everything below the chrome row.
constexpr int OVERLAY_TOP = 40;
constexpr int OVERLAY_HEIGHT = SCREEN_HEIGHT - OVERLAY_TOP;
} // namespace layout

// CYD microSD slot (ESP32-2432S028 / 2 USB) uses the VSPI pins; CS is GPIO5.
constexpr int SD_CS_PIN = 5;
