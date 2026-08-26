#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Rollover-safe millis() helpers.
//
// A direct `millis() > deadline` comparison breaks permanently once the 32-bit
// tick counter wraps, which happens after ~49.7 days of continuous power. A car
// display that stays on a permanent 12 V feed reaches that. Subtracting first
// and comparing the signed difference keeps working across the wrap, as long as
// the interval being measured is shorter than ~24 days.
// ---------------------------------------------------------------------------

// True once `now` has reached or passed `deadline`.
inline bool timeReached(unsigned long deadline)
{
  return static_cast<long>(millis() - deadline) >= 0;
}

// True once at least `interval` ms have elapsed since `since`.
inline bool elapsed(unsigned long since, unsigned long interval)
{
  return static_cast<long>(millis() - since) >= static_cast<long>(interval);
}

// Build a deadline `milliseconds` into the future. Wrapping is intentional and
// handled correctly by timeReached().
inline unsigned long deadlineIn(unsigned long milliseconds)
{
  return millis() + milliseconds;
}
