#pragma once

#include <Arduino.h>
#include <SD.h>

#include "cydTheme.h"

// ---------------------------------------------------------------------------
// One-time animation installer.
//
// The PIONEER mode plays .anm packs from /anim on the SD card, which normally
// means putting the card in a reader and copying a file. Not everybody has a
// reader, and the card lives inside the unit - so this is the same trick the
// Japanese font already uses: bake the file into the firmware, upload this
// environment once to write it onto the card, then upload the normal one.
//
// Upload cyd2usb_anim_installer, wait for ANIM READY, upload cyd2usb.
//
// The pack is firmware_assets/anim.anm, built by tools/make_dolphin_pack.py or
// tools/make_animation.py. To install a different animation, replace that file
// and upload this environment again. CARDISPLAY_ANIM_NAME sets the name it
// lands under, so several packs can be installed one after another without
// overwriting each other.
// ---------------------------------------------------------------------------

extern const uint8_t cardisplayInstallerAnimStart[]
    asm("_binary_firmware_assets_anim_anm_start");
extern const uint8_t cardisplayInstallerAnimEnd[]
    asm("_binary_firmware_assets_anim_anm_end");

#ifndef CARDISPLAY_ANIM_NAME
#define CARDISPLAY_ANIM_NAME "dolphin"
#endif

static constexpr const char *ANIM_INSTALLER_DIR = "/anim";
static constexpr const char *ANIM_INSTALLER_PATH = "/anim/" CARDISPLAY_ANIM_NAME ".anm";
static constexpr const char *ANIM_INSTALLER_TEMP = "/anim/install.tmp";

// "ANM1". Checked before anything is written, so a firmware built with a
// truncated or wrong file says so rather than installing rubbish that the
// player then has to reject on the card.
static constexpr uint32_t ANIM_INSTALLER_MAGIC = 0x314D4E41UL;

inline void drawAnimInstallerScreen(const char *title, const char *detail,
                                    uint16_t color)
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString("ANIMATION INSTALLER", 12, 8, 2);
  tft.drawFastHLine(0, 30, layout::SCREEN_WIDTH, color);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawCentreString(title, layout::CENTRE_X, 84, 4);
  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString(detail, layout::CENTRE_X, 126, 2);
}

inline void stopInAnimInstaller()
{
  while (true)
    delay(1000);
}

inline void runCarDisplayAnimationInstaller()
{
  tft.init();
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  drawAnimInstallerScreen("PREPARING", "Mounting SD card...", theme::GREEN);

  const size_t packSize = static_cast<size_t>(cardisplayInstallerAnimEnd -
                                              cardisplayInstallerAnimStart);

  // Validate what was embedded before touching the card.
  if (packSize < 16)
  {
    Serial.println(F("Animation installer: embedded pack is empty"));
    drawAnimInstallerScreen("NO PACK", "firmware_assets/anim.anm missing", TFT_RED);
    stopInAnimInstaller();
  }

  uint32_t magic = 0;
  memcpy(&magic, cardisplayInstallerAnimStart, sizeof(magic));
  if (magic != ANIM_INSTALLER_MAGIC)
  {
    Serial.println(F("Animation installer: embedded file is not a .anm pack"));
    drawAnimInstallerScreen("BAD PACK", "Not an ANM1 file", TFT_RED);
    stopInAnimInstaller();
  }

  if (!SD.begin(SD_CS_PIN))
  {
    Serial.println(F("Animation installer: SD mount failed"));
    drawAnimInstallerScreen("SD ERROR", "Insert a FAT32 SD card", TFT_RED);
    stopInAnimInstaller();
  }

  if (!SD.exists(ANIM_INSTALLER_DIR) && !SD.mkdir(ANIM_INSTALLER_DIR))
  {
    Serial.println(F("Animation installer: could not create /anim"));
    drawAnimInstallerScreen("WRITE ERROR", "Cannot create /anim", TFT_RED);
    stopInAnimInstaller();
  }

  fs::File installed = SD.open(ANIM_INSTALLER_PATH, FILE_READ);
  const bool alreadyInstalled = installed && installed.size() == packSize;
  if (installed)
    installed.close();

  if (!alreadyInstalled)
  {
    if (SD.exists(ANIM_INSTALLER_TEMP))
      SD.remove(ANIM_INSTALLER_TEMP);

    fs::File output = SD.open(ANIM_INSTALLER_TEMP, FILE_WRITE);
    if (!output)
    {
      Serial.println(F("Animation installer: could not open temporary file"));
      drawAnimInstallerScreen("WRITE ERROR", "Cannot open output file", TFT_RED);
      stopInAnimInstaller();
    }

    drawAnimInstallerScreen("INSTALLING", "Do not unplug power", theme::GREEN);
    tft.drawRect(29, 165, 262, 10, theme::DARK);

    static constexpr size_t CHUNK_SIZE = 4096;
    size_t written = 0;
    while (written < packSize)
    {
      const size_t chunk = min(CHUNK_SIZE, packSize - written);
      if (output.write(cardisplayInstallerAnimStart + written, chunk) != chunk)
        break;
      written += chunk;

      const int barWidth = static_cast<int>((written * 260ULL) / packSize);
      tft.fillRect(30, 166, barWidth, 8, theme::GREEN);
      delay(1);
    }
    output.flush();
    output.close();

    // Written and read back at the expected size before anything is renamed, so
    // a card that quietly ran out of room cannot leave a half pack behind under
    // the real name for the player to trip over.
    fs::File verify = SD.open(ANIM_INSTALLER_TEMP, FILE_READ);
    const bool valid = written == packSize && verify && verify.size() == packSize;
    if (verify)
      verify.close();

    if (!valid)
    {
      SD.remove(ANIM_INSTALLER_TEMP);
      Serial.println(F("Animation installer: write verification failed"));
      drawAnimInstallerScreen("WRITE ERROR", "Copy failed, card full?", TFT_RED);
      stopInAnimInstaller();
    }

    if (SD.exists(ANIM_INSTALLER_PATH))
      SD.remove(ANIM_INSTALLER_PATH);
    if (!SD.rename(ANIM_INSTALLER_TEMP, ANIM_INSTALLER_PATH))
    {
      Serial.println(F("Animation installer: final rename failed"));
      drawAnimInstallerScreen("WRITE ERROR", "Final rename failed", TFT_RED);
      stopInAnimInstaller();
    }
  }

  Serial.print(F("Animation installed: "));
  Serial.println(ANIM_INSTALLER_PATH);
  drawAnimInstallerScreen("ANIM READY", "Now upload: cyd2usb", theme::GREEN);

  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString(ANIM_INSTALLER_PATH, layout::CENTRE_X, 166, 2);
  tft.drawCentreString("Pick PIONEER in the visualizer", layout::CENTRE_X, 196, 1);
  stopInAnimInstaller();
}
