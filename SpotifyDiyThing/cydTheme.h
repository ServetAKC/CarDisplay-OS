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
// Green "Honda" palette used by the player UI.
constexpr uint16_t GREEN = 0x4EE6;
constexpr uint16_t BRIGHT = 0x9FF0;
constexpr uint16_t PRESS = 0xDFFB; // pale mint highlight for previous/next taps
constexpr uint16_t DIM = 0x2363;
constexpr uint16_t DARK = 0x1A22;
constexpr uint16_t PROGRESS_TRACK = 0x2A65; // dark muted green, unplayed line

// Visualizer brightness ramp, darkest to brightest.
//
// The retro modes used to run on their own Y2K palette, whose mint (teal) and
// lime (yellow-green) steps read as a different product from the player screen.
// The ramp below stays inside the same green family as the header, the close
// button and the transport icons: the modes that used hue for variety now use
// brightness, which is what the old palette was really being used for anyway.
constexpr uint16_t VIZ_CORE = 0x0881;   // near-black core fill
constexpr uint16_t VIZ_DEEP = 0x0A61;   // barely-there grid lines and fills
constexpr uint16_t VIZ_GLOW = DIM;      // background structure
constexpr uint16_t VIZ_MID = GREEN;     // the workhorse, same as the player UI
constexpr uint16_t VIZ_BRIGHT = BRIGHT; // active / loud
constexpr uint16_t VIZ_PEAK = PRESS;    // peaks and highlights
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
