#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Resistive touch input.
//
// A dedicated task samples the panel and latches one action per physical press.
// v0.3.15 published the result as eight parallel booleans that the caller had to
// clear and test in the right order; a single enum makes the contract obvious
// and impossible to read stale.
// ---------------------------------------------------------------------------

enum class TouchAction : int8_t
{
  None = 0,
  Previous,
  Next,
  PlayPause,
  ToggleClock,
  ToggleVisualizer,
  CycleBrightness,
  NextVisualizerStyle,
  PreviousVisualizerStyle,
  SkipWiFiWait,
  ToggleAnimationPlayback,
  OpenOfflineVisualizer
};

void touchSetup();

// Returns the pending action and consumes it. TouchAction::None when idle.
TouchAction takeTouchAction();

// Which hotspot map is active. Only one of these is ever true at a time.
void setTouchClockMode(bool active);
void setTouchVisualizerMode(bool active);
void setTouchSetupPortalMode(bool active);

// While the unit is still trying the saved network at boot, any touch anywhere
// means "stop waiting and show me the setup screen". It needs its own map
// because the player map leaves the middle of the screen inert, and the middle
// is exactly where someone jabs at a screen that is not doing anything.
void setTouchConnectingMode(bool active);

// True while the PIONEER mode is the one on screen. It puts a play/stop button
// in the middle of the overlay, which has to be tested before the left/right
// mode split or it would just be another "next mode" tap.
void setTouchAnimationMode(bool active);
