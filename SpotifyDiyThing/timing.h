#pragma once

#include <Arduino.h>

#include "timeMath.h"

// Arduino-side wrappers over the rollover-safe helpers in timeMath.h.

// True once the current tick has reached or passed `deadline`.
inline bool timeReached(unsigned long deadline)
{
  return hasReached(millis(), deadline);
}

// True once at least `interval` ms have elapsed since `since`.
inline bool elapsed(unsigned long since, unsigned long interval)
{
  return hasElapsed(millis(), since, interval);
}

// Build a deadline `milliseconds` into the future. Wrapping past 2^32 is
// intentional and handled correctly by timeReached().
inline unsigned long deadlineIn(unsigned long milliseconds)
{
  return millis() + milliseconds;
}
