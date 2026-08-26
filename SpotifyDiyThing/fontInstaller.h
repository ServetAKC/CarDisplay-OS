#pragma once

#include <Arduino.h>
#include <SD.h>
#include "cydTheme.h"

extern const uint8_t hondaInstallerFontStart[]
    asm("_binary_firmware_assets_jp16_huf_start");
extern const uint8_t hondaInstallerFontEnd[]
    asm("_binary_firmware_assets_jp16_huf_end");

static constexpr const char *FONT_INSTALLER_DIR = "/hondathing";
static constexpr const char *HONDATHING_FONT_PATH = "/hondathing/jp16.huf";
static constexpr const char *HONDATHING_FONT_TEMP = "/hondathing/jp16.tmp";

inline void drawFontInstallerScreen(const char *title, const char *detail,
                                    uint16_t color)
{
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(3, 3, 314, 234, 0x0320);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawCentreString(title, 160, 68, 4);
  tft.setTextColor(0x7BEF, TFT_BLACK);
  tft.drawCentreString(detail, 160, 112, 2);
}

inline void stopInFontInstaller()
{
  while (true)
    delay(1000);
}

inline void runHondaJapaneseFontInstaller()
{
  tft.init();
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  drawFontInstallerScreen("FONT INSTALLER", "Preparing SD card...", TFT_GREEN);

  if (!SD.begin(SD_CS_PIN))
  {
    Serial.println("Font installer: SD mount failed");
    drawFontInstallerScreen("SD ERROR", "Insert FAT32 SD card", TFT_RED);
    stopInFontInstaller();
  }

  if (!SD.exists(HONDATHING_FONT_DIR) && !SD.mkdir(HONDATHING_FONT_DIR))
  {
    Serial.println("Font installer: could not create /hondathing");
    drawFontInstallerScreen("WRITE ERROR", "Cannot create folder", TFT_RED);
    stopInFontInstaller();
  }

  const size_t fontSize = static_cast<size_t>(hondaInstallerFontEnd -
                                               hondaInstallerFontStart);
  fs::File installed = SD.open(HONDATHING_FONT_PATH, FILE_READ);
  const bool alreadyInstalled = installed && installed.size() == fontSize;
  if (installed)
    installed.close();

  if (!alreadyInstalled)
  {
    if (SD.exists(HONDATHING_FONT_TEMP))
      SD.remove(HONDATHING_FONT_TEMP);

    fs::File output = SD.open(HONDATHING_FONT_TEMP, FILE_WRITE);
    if (!output)
    {
      Serial.println("Font installer: could not open temporary file");
      drawFontInstallerScreen("WRITE ERROR", "Cannot open font file", TFT_RED);
      stopInFontInstaller();
    }

    drawFontInstallerScreen("INSTALLING FONT", "Do not unplug power", TFT_GREEN);
    static constexpr size_t CHUNK_SIZE = 4096;
    size_t written = 0;
    while (written < fontSize)
    {
      const size_t chunk = min(CHUNK_SIZE, fontSize - written);
      if (output.write(hondaInstallerFontStart + written, chunk) != chunk)
        break;
      written += chunk;

      const int barWidth = static_cast<int>((written * 260ULL) / fontSize);
      tft.fillRect(30, 154, 260, 8, 0x0320);
      tft.fillRect(30, 154, barWidth, 8, TFT_GREEN);
      delay(1);
    }
    output.flush();
    output.close();

    fs::File verify = SD.open(HONDATHING_FONT_TEMP, FILE_READ);
    const bool valid = written == fontSize && verify && verify.size() == fontSize;
    if (verify)
      verify.close();

    if (!valid)
    {
      SD.remove(HONDATHING_FONT_TEMP);
      Serial.println("Font installer: write verification failed");
      drawFontInstallerScreen("WRITE ERROR", "Font copy failed", TFT_RED);
      stopInFontInstaller();
    }

    if (SD.exists(HONDATHING_FONT_PATH))
      SD.remove(HONDATHING_FONT_PATH);
    if (!SD.rename(HONDATHING_FONT_TEMP, HONDATHING_FONT_PATH))
    {
      Serial.println("Font installer: final rename failed");
      drawFontInstallerScreen("WRITE ERROR", "Final rename failed", TFT_RED);
      stopInFontInstaller();
    }
  }

  Serial.println("Japanese font installed on SD card");
  drawFontInstallerScreen("FONT READY", "Now upload: cyd2usb", TFT_GREEN);
  stopInFontInstaller();
}
