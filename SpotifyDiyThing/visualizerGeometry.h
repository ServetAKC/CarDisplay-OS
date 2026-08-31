#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Integer geometry shared by the visualizer modes.
//
// These tables lived in an anonymous namespace inside visualizerRenderer.cpp
// until v0.4 split the newer modes into visualizerModes.cpp. They are here so
// both files see the same numbers instead of each keeping a copy that could
// drift.
//
// Everything is fixed point on purpose. The FFT already spends the float unit
// once per frame; a mode that also called sinf() per point would push the
// buffered modes below the 24 FPS the overlay is built around.
// ---------------------------------------------------------------------------

namespace vizgeom
{
// 32 unit vectors scaled by 1000, one every 11.25 degrees. Index 0 points
// straight up and the index advances clockwise.
constexpr int16_t UNIT_DX[32] = {
    0, 195, 383, 556, 707, 831, 924, 981,
    1000, 981, 924, 831, 707, 556, 383, 195,
    0, -195, -383, -556, -707, -831, -924, -981,
    -1000, -981, -924, -831, -707, -556, -383, -195};
constexpr int16_t UNIT_DY[32] = {
    -1000, -981, -924, -831, -707, -556, -383, -195,
    0, 195, 383, 556, 707, 831, 924, 981,
    1000, 981, 924, 831, 707, 556, 383, 195,
    0, -195, -383, -556, -707, -831, -924, -981};

// 256-step sine and cosine, scaled by 1000. Coarser than calling sinf() and
// that is the point: it is a table lookup in the inner loop of the ribbon and
// grid modes.
//
// The tables above are indexed from straight up, so UNIT_DX is already the sine
// of the angle and UNIT_DY is its negated cosine. Reading them in that order is
// the whole implementation.
inline int sin1000(uint8_t angle)
{
  // Fold the 256-step circle onto the 32 directions above.
  return UNIT_DX[(angle >> 3) & 31];
}

inline int cos1000(uint8_t angle)
{
  return -UNIT_DY[(angle >> 3) & 31];
}
} // namespace vizgeom
