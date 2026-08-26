#include "powerCycleDetector.h"

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <esp_system.h>

#include "timing.h"

namespace
{
constexpr const char *POWER_CYCLE_FLAG = "/power_cycle.flag";
constexpr unsigned long POWER_CYCLE_WINDOW_MS = 8000UL;

bool armed = false;
unsigned long disarmDeadline = 0;
} // namespace

void clearPowerCycleMarker()
{
  armed = false;
  if (SPIFFS.exists(POWER_CYCLE_FLAG))
    SPIFFS.remove(POWER_CYCLE_FLAG);
}

bool detectQuickPowerCycle()
{
  if (SPIFFS.exists(POWER_CYCLE_FLAG))
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool physicalPowerCycle = reason == ESP_RST_POWERON || reason == ESP_RST_EXT;
    clearPowerCycleMarker();
    if (physicalPowerCycle)
      return true;
    // Software, watchdog and panic resets are not user power gestures.
  }

  fs::File marker = SPIFFS.open(POWER_CYCLE_FLAG, "w");
  if (marker)
  {
    marker.print("1");
    marker.close();
    armed = true;
    disarmDeadline = deadlineIn(POWER_CYCLE_WINDOW_MS);
  }
  return false;
}

void servicePowerCycleDetector()
{
  if (armed && timeReached(disarmDeadline))
    clearPowerCycleMarker();
}
