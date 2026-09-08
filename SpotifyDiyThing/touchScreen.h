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
  OpenOfflineVisualizer,

  // The animation selector. Opening and closing it are two distinct actions
  // rather than one toggle, because the renderer also closes the menu by
  // itself once a row is picked and a toggle would then reopen it.
  OpenAnimationMenu,
  CloseAnimationMenu,
  PickAnimation
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

// True while the animation selector is covering the scene. It takes the whole
// overlay: every row is a pick and the left/right mode split is off, or
// choosing an animation on the left would also step back a mode.
void setTouchAnimationMenu(bool active);

// Y of the press that produced the action just taken. Only meaningful for
// PickAnimation, where the row is the payload the enum cannot carry. Read it
// straight after takeTouchAction(); the next press overwrites it.
int16_t lastTouchY();
