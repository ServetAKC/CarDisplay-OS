#pragma once

#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_system.h>

#define HONDATHING_POWER_CYCLE_FLAG "/power_cycle.flag"
static constexpr unsigned long HONDATHING_POWER_CYCLE_WINDOW_MS = 8000UL;
static bool hondaThingPowerCycleArmed = false;

inline void clearPowerCycleMarker()
{
  hondaThingPowerCycleArmed = false;
  if (SPIFFS.exists(HONDATHING_POWER_CYCLE_FLAG))
    SPIFFS.remove(HONDATHING_POWER_CYCLE_FLAG);
}

inline bool detectQuickPowerCycle()
{
  if (SPIFFS.exists(HONDATHING_POWER_CYCLE_FLAG))
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool physicalPowerCycle = reason == ESP_RST_POWERON || reason == ESP_RST_EXT;
    clearPowerCycleMarker();
    if (physicalPowerCycle)
      return true;
    // Software, watchdog and panic resets are not user power gestures.
  }

  File marker = SPIFFS.open(HONDATHING_POWER_CYCLE_FLAG, "w");
  if (marker)
  {
    marker.print("1");
    marker.close();
    hondaThingPowerCycleArmed = true;
  }
  return false;
}

inline void servicePowerCycleDetector()
{
  if (hondaThingPowerCycleArmed && millis() > HONDATHING_POWER_CYCLE_WINDOW_MS)
    clearPowerCycleMarker();
}
