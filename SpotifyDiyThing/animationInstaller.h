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
// Japanese font already uses: bake the files into the firmware, upload this
// environment once to write them onto the card, then upload the normal one.
//
// Upload cyd2usb_anim_installer, wait for ANIM READY, upload cyd2usb.
//
// The packs are the original Pioneer head-unit animations, decoded from the
// .lkd files by tools/lkd_to_anm.py. To change the set, edit the table below
// and the matching board_build.embed_files list in platformio.ini - the two
// have to agree, because the linker only emits symbols for files it embedded.
//
// AnimationPlayer::MAX_PACKS caps how many the player will pick up off the
// card, so keep this list at or under that number; extra packs would be
// installed and then silently never played.
// ---------------------------------------------------------------------------

// One pair of symbols per embedded file. The names come from the path, with
// every non-alphanumeric character replaced by an underscore.
#define CARDISPLAY_ANIM_SYMBOLS(name)                                          \
  extern const uint8_t cardisplayAnim_##name##_start[]                         \
      asm("_binary_firmware_assets_" #name "_anm_start");                      \
  extern const uint8_t cardisplayAnim_##name##_end[]                           \
      asm("_binary_firmware_assets_" #name "_anm_end");

CARDISPLAY_ANIM_SYMBOLS(dolphins)
CARDISPLAY_ANIM_SYMBOLS(carrozzeria)
CARDISPLAY_ANIM_SYMBOLS(reef)
CARDISPLAY_ANIM_SYMBOLS(flowers)
CARDISPLAY_ANIM_SYMBOLS(canyon)
CARDISPLAY_ANIM_SYMBOLS(city)
CARDISPLAY_ANIM_SYMBOLS(roadster)
CARDISPLAY_ANIM_SYMBOLS(mecha)

#define CARDISPLAY_ANIM_ENTRY(name)                                            \
  {                                                                            \
#name, cardisplayAnim_##name##_start, cardisplayAnim_##name##_end          \
  }

struct AnimInstallerPack
{
  const char *name;
  const uint8_t *start;
  const uint8_t *end;

  size_t size() const { return static_cast<size_t>(end - start); }
};

// The install order is the order the player walks them in, so the dolphins -
// the one everybody asks for - go first.
static const AnimInstallerPack ANIM_INSTALLER_PACKS[] = {
    CARDISPLAY_ANIM_ENTRY(dolphins),
    CARDISPLAY_ANIM_ENTRY(carrozzeria),
    CARDISPLAY_ANIM_ENTRY(reef),
    CARDISPLAY_ANIM_ENTRY(flowers),
    CARDISPLAY_ANIM_ENTRY(canyon),
    CARDISPLAY_ANIM_ENTRY(city),
    CARDISPLAY_ANIM_ENTRY(roadster),
    CARDISPLAY_ANIM_ENTRY(mecha),
};

static constexpr size_t ANIM_INSTALLER_COUNT =
    sizeof(ANIM_INSTALLER_PACKS) / sizeof(ANIM_INSTALLER_PACKS[0]);

static constexpr const char *ANIM_INSTALLER_DIR = "/anim";
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

// Copies one pack to the card. Returns false and leaves the screen showing the
// reason on any failure; the caller stops there rather than carrying on, so a
// card that filled up halfway does not look like a clean install.
inline bool installOneAnimPack(const AnimInstallerPack &pack, size_t position)
{
  char path[40];
  snprintf(path, sizeof(path), "%s/%s.anm", ANIM_INSTALLER_DIR, pack.name);

  const size_t packSize = pack.size();
  if (packSize < 16)
  {
    Serial.print(F("Animation installer: empty pack "));
    Serial.println(pack.name);
    drawAnimInstallerScreen("NO PACK", pack.name, TFT_RED);
    return false;
  }

  uint32_t magic = 0;
  memcpy(&magic, pack.start, sizeof(magic));
  if (magic != ANIM_INSTALLER_MAGIC)
  {
    Serial.print(F("Animation installer: not an ANM1 pack: "));
    Serial.println(pack.name);
    drawAnimInstallerScreen("BAD PACK", pack.name, TFT_RED);
    return false;
  }

  // Already there at the right size - nothing to do. Re-running the installer
  // after adding one pack should not rewrite the other seven.
  fs::File installed = SD.open(path, FILE_READ);
  const bool alreadyInstalled = installed && installed.size() == packSize;
  if (installed)
    installed.close();
  if (alreadyInstalled)
  {
    Serial.print(F("Animation already installed: "));
    Serial.println(path);
    return true;
  }

  if (SD.exists(ANIM_INSTALLER_TEMP))
    SD.remove(ANIM_INSTALLER_TEMP);

  fs::File output = SD.open(ANIM_INSTALLER_TEMP, FILE_WRITE);
  if (!output)
  {
    Serial.println(F("Animation installer: could not open temporary file"));
    drawAnimInstallerScreen("WRITE ERROR", "Cannot open output file", TFT_RED);
    return false;
  }

  char detail[32];
  snprintf(detail, sizeof(detail), "%u/%u  %s", static_cast<unsigned>(position + 1),
           static_cast<unsigned>(ANIM_INSTALLER_COUNT), pack.name);
  drawAnimInstallerScreen("INSTALLING", detail, theme::GREEN);
  tft.drawRect(29, 165, 262, 10, theme::DARK);

  static constexpr size_t CHUNK_SIZE = 4096;
  size_t written = 0;
  while (written < packSize)
  {
    const size_t chunk = min(CHUNK_SIZE, packSize - written);
    if (output.write(pack.start + written, chunk) != chunk)
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
    Serial.print(F("Animation installer: write verification failed for "));
    Serial.println(pack.name);
    drawAnimInstallerScreen("WRITE ERROR", "Copy failed, card full?", TFT_RED);
    return false;
  }

  if (SD.exists(path))
    SD.remove(path);
  if (!SD.rename(ANIM_INSTALLER_TEMP, path))
  {
    Serial.println(F("Animation installer: final rename failed"));
    drawAnimInstallerScreen("WRITE ERROR", "Final rename failed", TFT_RED);
    return false;
  }

  Serial.print(F("Animation installed: "));
  Serial.println(path);
  return true;
}

inline void runCarDisplayAnimationInstaller()
{
  tft.init();
  tft.setRotation(1);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  drawAnimInstallerScreen("PREPARING", "Mounting SD card...", theme::GREEN);

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

  for (size_t i = 0; i < ANIM_INSTALLER_COUNT; ++i)
  {
    if (!installOneAnimPack(ANIM_INSTALLER_PACKS[i], i))
      stopInAnimInstaller();
  }

  char summary[32];
  snprintf(summary, sizeof(summary), "%u packs in /anim",
           static_cast<unsigned>(ANIM_INSTALLER_COUNT));

  Serial.print(F("Animation install complete: "));
  Serial.println(summary);
  drawAnimInstallerScreen("ANIM READY", "Now upload: cyd2usb", theme::GREEN);

  tft.setTextColor(theme::DIM, TFT_BLACK);
  tft.drawCentreString(summary, layout::CENTRE_X, 166, 2);
  tft.drawCentreString("Pick PIONEER in the visualizer", layout::CENTRE_X, 196, 1);
  stopInAnimInstaller();
}
