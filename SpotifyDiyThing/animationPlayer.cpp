#include "animationPlayer.h"

#include <SD.h>
#include <string.h>

#include "cydTheme.h"
#include "timing.h"

namespace
{
constexpr const char *ANIMATION_DIR = "/anim";

// Only the leaf name, without the directory or the extension, for the chrome
// row. "/anim/dolphin.anm" becomes "DOLPHIN".
void shortName(const char *path, char *out, size_t outSize)
{
  const char *slash = strrchr(path, '/');
  const char *start = slash != nullptr ? slash + 1 : path;

  size_t written = 0;
  while (start[written] != '\0' && start[written] != '.' && written + 1 < outSize)
  {
    const char c = start[written];
    out[written] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    ++written;
  }
  out[written] = '\0';
}
} // namespace

size_t AnimationPlayer::begin()
{
  packTotal = 0;

  // The album cache mounts the card first; calling begin() again when it is
  // already mounted is harmless and covers the case where the cache is absent.
  if (!SD.begin(SD_CS_PIN))
  {
    Serial.println(F("Animation: no SD card, /anim unavailable"));
    return 0;
  }

  fs::File dir = SD.open(ANIMATION_DIR);
  if (!dir || !dir.isDirectory())
  {
    Serial.println(F("Animation: /anim does not exist on the card"));
    if (dir)
      dir.close();
    return 0;
  }

  while (packTotal < MAX_PACKS)
  {
    fs::File entry = dir.openNextFile();
    if (!entry)
      break;

    const char *name = entry.name();
    const size_t length = strlen(name);
    const bool isPack = !entry.isDirectory() && length > 4 &&
                        strcmp(name + length - 4, ".anm") == 0;
    if (isPack)
    {
      // entry.name() is the full path on some core versions and the leaf on
      // others. Rebuild it either way rather than guessing.
      if (name[0] == '/')
        snprintf(packPath[packTotal], sizeof(packPath[0]), "%s", name);
      else
        snprintf(packPath[packTotal], sizeof(packPath[0]), "%s/%s", ANIMATION_DIR, name);

      shortName(packPath[packTotal], packName[packTotal], sizeof(packName[0]));
      Serial.print(F("Animation pack: "));
      Serial.println(packPath[packTotal]);
      ++packTotal;
    }
    entry.close();
  }
  dir.close();

  if (packTotal == 0)
    Serial.println(F("Animation: /anim exists but holds no .anm packs"));
  return packTotal;
}

const char *AnimationPlayer::currentPackName() const
{
  if (!packOpen || packIndex >= packTotal)
    return "";
  return packName[packIndex];
}

bool AnimationPlayer::open()
{
  if (packTotal == 0)
    return false;

  if (frameBuffer == nullptr)
  {
    frameBuffer = static_cast<uint8_t *>(malloc(MAX_FRAME_BYTES));
    if (frameBuffer == nullptr)
    {
      Serial.println(F("Animation: frame buffer allocation failed"));
      return false;
    }
  }

  packIndex = 0;
  return openPack(packIndex);
}

void AnimationPlayer::close()
{
  closePack();
  free(frameBuffer);
  frameBuffer = nullptr;
}

bool AnimationPlayer::openPack(size_t index)
{
  closePack();

  if (index >= packTotal || frameBuffer == nullptr)
    return false;

  fs::File file = SD.open(packPath[index], FILE_READ);
  if (!file)
  {
    Serial.print(F("Animation: cannot open "));
    Serial.println(packPath[index]);
    return false;
  }

  const size_t read = file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header));
  file.close();

  if (read != sizeof(header) || header.magic != ANM_MAGIC)
  {
    Serial.print(F("Animation: bad header in "));
    Serial.println(packPath[index]);
    return false;
  }

  if (header.width == 0 || header.height == 0 || header.frameCount == 0 ||
      header.width > MAX_WIDTH || header.height > MAX_HEIGHT)
  {
    Serial.print(F("Animation: unusable dimensions in "));
    Serial.println(packPath[index]);
    return false;
  }

  rowBytes = (header.width + 7) / 8;
  // Set before readFrame(), which addresses the file through packIndex. Doing
  // it in the caller instead reads frame zero out of the pack being replaced.
  packIndex = index;
  packOpen = true;
  frameIndex = 0;
  nextFrameTime = 0;
  return readFrame(0);
}

void AnimationPlayer::closePack()
{
  packOpen = false;
  frameIndex = 0;
  rowBytes = 0;
}

bool AnimationPlayer::readFrame(uint16_t index)
{
  if (!packOpen || frameBuffer == nullptr || index >= header.frameCount)
    return false;

  const size_t frameBytes = rowBytes * header.height;
  if (frameBytes > MAX_FRAME_BYTES)
    return false;

  // Reopened per frame rather than held open for the life of the mode. An SD
  // handle kept across the seconds-long TLS stalls the Spotify clients used to
  // cause was the source of a whole class of card errors in v0.3; the reopen
  // costs about a millisecond and removes the problem.
  fs::File file = SD.open(packPath[packIndex], FILE_READ);
  if (!file)
    return false;

  const size_t offset = sizeof(Header) + static_cast<size_t>(index) * frameBytes;
  bool ok = file.seek(offset) && file.read(frameBuffer, frameBytes) == frameBytes;
  file.close();
  return ok;
}

void AnimationPlayer::drawCurrent(TFT_eSPI &target, int originX, int originY) const
{
  drawFrame(target, originX, originY);
}

void AnimationPlayer::drawFrame(TFT_eSPI &target, int originX, int originY) const
{
  if (!packOpen || frameBuffer == nullptr)
    return;

  // Run-length drawing rather than per-pixel. A monochrome frame is mostly long
  // horizontal runs, so this turns 64000 drawPixel calls into a few thousand
  // drawFastHLine calls - the difference between roughly two frames a second
  // and the full rate.
  for (int y = 0; y < header.height; ++y)
  {
    const uint8_t *row = frameBuffer + static_cast<size_t>(y) * rowBytes;
    int x = 0;
    while (x < header.width)
    {
      // Skip a run of clear pixels. The background is already black, so there
      // is nothing to draw for them.
      while (x < header.width && ((row[x >> 3] >> (7 - (x & 7))) & 1) == 0)
        ++x;
      if (x >= header.width)
        break;

      const int runStart = x;
      while (x < header.width && ((row[x >> 3] >> (7 - (x & 7))) & 1) != 0)
        ++x;

      target.drawFastHLine(originX + runStart, originY + y, x - runStart,
                           theme::VIZ_BRIGHT);
    }
  }
}

bool AnimationPlayer::service(TFT_eSPI &target, int originX, int originY)
{
  if (!packOpen)
    return false;

  // Frame pacing is the pack's own, not the renderer's. A 12 FPS animation
  // played at the renderer's 24 would run at double speed, and these sequences
  // were authored at a particular rate.
  const uint8_t rate = header.frameRate == 0 ? 12 : header.frameRate;
  const unsigned long interval = 1000UL / rate;

  if (nextFrameTime == 0 || timeReached(nextFrameTime))
  {
    nextFrameTime = deadlineIn(interval);

    uint16_t next = frameIndex + 1;
    if (next >= header.frameCount)
    {
      // End of the pack. With more than one on the card they play as one show;
      // with one it simply loops.
      if (packTotal > 1)
      {
        const size_t current = packIndex;
        const size_t following = (packIndex + 1) % packTotal;
        if (openPack(following) || openPack(current))
        {
          drawFrame(target, originX, originY);
          return true;
        }
        // Both failed: the card has gone away mid-show. Report it so the caller
        // can put something on screen instead of leaving the last frame frozen.
        return false;
      }
      next = 0;
    }

    if (readFrame(next))
      frameIndex = next;
  }

  drawFrame(target, originX, originY);
  return true;
}
