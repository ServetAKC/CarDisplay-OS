#pragma once

// ---------------------------------------------------------------------------
// Rollover-safe tick arithmetic.
//
// A direct `now > deadline` comparison breaks permanently once the 32-bit
// millisecond counter wraps, which happens after ~49.7 days of continuous
// power - reachable in a car left on a permanent 12 V feed. v0.3.15 compared
// absolute values in the Spotify poller, the progress bar, the touch cooldown
// and the visualizer peak decay, so all four would have wedged at that point.
//
// Subtracting first and testing the signed difference keeps working across the
// wrap, provided the interval being measured is shorter than ~24 days.
//
// Kept free of Arduino so the native tests can drive it across the wrap point
// directly, which is not something you want to wait 49 days to discover.
// ---------------------------------------------------------------------------

inline bool hasReached(unsigned long now, unsigned long deadline)
{
  return static_cast<long>(now - deadline) >= 0;
}

inline bool hasElapsed(unsigned long now, unsigned long since, unsigned long interval)
{
  return static_cast<long>(now - since) >= static_cast<long>(interval);
}
