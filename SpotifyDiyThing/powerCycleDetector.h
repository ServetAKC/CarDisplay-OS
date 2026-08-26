#pragma once

// Detects the "power the unit off and on twice quickly" gesture that forces the
// Wi-Fi setup portal open. Implemented in powerCycleDetector.cpp so the armed
// flag is a single object across the whole firmware; when this lived in a header
// as a file-static, every translation unit that included it got its own copy.

void clearPowerCycleMarker();
bool detectQuickPowerCycle();
void servicePowerCycleDetector();
