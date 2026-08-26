#pragma once

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <TFT_eSPI.h>

// Compact 16x16 monochrome Unicode font reader. The complete glyph bitmap stays
// on the SD card; only one 32-byte glyph and a 512-byte TFT pixel buffer are held
// in RAM while text is drawn.
class SdJapaneseFont
{
public:
  bool begin(fs::FS &filesystem = SD, const char *path = "/hondathing/jp16.huf")
  {
    close();
    fontFile = filesystem.open(path, FILE_READ);
    if (!fontFile)
      return false;

    FontHeader header;
    if (fontFile.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header) ||
        memcmp(header.magic, "HJF1", 4) != 0 ||
        header.width != GLYPH_WIDTH || header.height != GLYPH_HEIGHT ||
        header.glyphCount != GLYPH_COUNT || header.bytesPerGlyph != BYTES_PER_GLYPH)
    {
      close();
      return false;
    }

    const size_t expectedSize = sizeof(FontHeader) +
                                static_cast<size_t>(GLYPH_COUNT) * BYTES_PER_GLYPH;
    ready = fontFile.size() >= expectedSize;
    if (!ready)
      close();
    return ready;
  }

  void close()
  {
    if (fontFile)
      fontFile.close();
    ready = false;
  }

  bool isReady() const { return ready; }

  static bool containsNonAscii(const char *text)
  {
    if (text == nullptr)
      return false;
    while (*text)
    {
      if (static_cast<uint8_t>(*text++) >= 0x80)
        return true;
    }
    return false;
  }

  int textWidth(const char *text) const
  {
    if (text == nullptr)
      return 0;
    int width = 0;
    const char *cursor = text;
    while (*cursor)
      width += glyphAdvance(nextCodepoint(cursor));
    return width;
  }

  void drawText(TFT_eSPI &display, const char *text, int x, int y,
                int maxWidth, uint16_t color, uint16_t background)
  {
    if (!ready || text == nullptr || maxWidth <= 0)
      return;

    const bool truncated = textWidth(text) > maxWidth;
    const int contentLimit = truncated ? max(0, maxWidth - ELLIPSIS_WIDTH) : maxWidth;
    int used = 0;
    const char *cursor = text;

    while (*cursor)
    {
      const char *before = cursor;
      const uint32_t codepoint = nextCodepoint(cursor);
      const int advance = glyphAdvance(codepoint);
      if (used + advance > contentLimit)
      {
        cursor = before;
        break;
      }
      drawGlyph(display, codepoint, x + used, y, color, background);
      used += advance;
    }

    if (truncated)
    {
      for (uint8_t i = 0; i < 3 && used + ASCII_ADVANCE <= maxWidth; ++i)
      {
        drawGlyph(display, '.', x + used, y, color, background);
        used += ASCII_ADVANCE;
      }
    }
  }

private:
  struct FontHeader
  {
    char magic[4];
    uint16_t width;
    uint16_t height;
    uint32_t glyphCount;
    uint32_t bytesPerGlyph;
  };

  static_assert(sizeof(FontHeader) == 16, "Unexpected Japanese font header size");

  static constexpr uint16_t GLYPH_WIDTH = 16;
  static constexpr uint16_t GLYPH_HEIGHT = 16;
  static constexpr uint32_t BYTES_PER_GLYPH = 32;
  static constexpr uint32_t GLYPH_COUNT = 29503;
  static constexpr int ASCII_ADVANCE = 8;
  static constexpr int WIDE_ADVANCE = 16;
  static constexpr int ELLIPSIS_WIDTH = ASCII_ADVANCE * 3;

  fs::File fontFile;
  bool ready = false;
  uint8_t bitmap[BYTES_PER_GLYPH] = {};
  uint16_t pixels[GLYPH_WIDTH * GLYPH_HEIGHT] = {};

  static uint32_t nextCodepoint(const char *&cursor)
  {
    const uint8_t first = static_cast<uint8_t>(*cursor++);
    if (first < 0x80)
      return first;

    if ((first & 0xE0) == 0xC0)
    {
      const uint8_t second = static_cast<uint8_t>(*cursor);
      if ((second & 0xC0) == 0x80)
      {
        cursor++;
        return ((first & 0x1F) << 6) | (second & 0x3F);
      }
    }
    else if ((first & 0xF0) == 0xE0)
    {
      if (!cursor[0] || !cursor[1])
        return '?';
      const uint8_t second = static_cast<uint8_t>(cursor[0]);
      const uint8_t third = static_cast<uint8_t>(cursor[1]);
      if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80)
      {
        cursor += 2;
        return ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
      }
    }
    else if ((first & 0xF8) == 0xF0)
    {
      if (!cursor[0] || !cursor[1] || !cursor[2])
        return '?';
      const uint8_t second = static_cast<uint8_t>(cursor[0]);
      const uint8_t third = static_cast<uint8_t>(cursor[1]);
      const uint8_t fourth = static_cast<uint8_t>(cursor[2]);
      if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80)
      {
        cursor += 3;
        return ((first & 0x07) << 18) | ((second & 0x3F) << 12) |
               ((third & 0x3F) << 6) | (fourth & 0x3F);
      }
    }

    return '?';
  }

  static int glyphAdvance(uint32_t codepoint)
  {
    if ((codepoint >= 0x20 && codepoint <= 0xFF) ||
        (codepoint >= 0xFF61 && codepoint <= 0xFF9F))
      return ASCII_ADVANCE;
    return glyphIndex(codepoint) >= 0 ? WIDE_ADVANCE : ASCII_ADVANCE;
  }

  static int32_t glyphIndex(uint32_t codepoint)
  {
    if (codepoint >= 0x20 && codepoint <= 0x7E)
      return codepoint - 0x20;
    if (codepoint >= 0xA0 && codepoint <= 0xFF)
      return 95 + codepoint - 0xA0;
    if (codepoint >= 0x2000 && codepoint <= 0x206F)
      return 191 + codepoint - 0x2000;
    if (codepoint >= 0x2100 && codepoint <= 0x214F)
      return 303 + codepoint - 0x2100;
    if (codepoint >= 0x3000 && codepoint <= 0x30FF)
      return 383 + codepoint - 0x3000;
    if (codepoint >= 0x31F0 && codepoint <= 0x33FF)
      return 639 + codepoint - 0x31F0;
    if (codepoint >= 0x3400 && codepoint <= 0x4DBF)
      return 1167 + codepoint - 0x3400;
    if (codepoint >= 0x4E00 && codepoint <= 0x9FFF)
      return 7759 + codepoint - 0x4E00;
    if (codepoint >= 0xF900 && codepoint <= 0xFAFF)
      return 28751 + codepoint - 0xF900;
    if (codepoint >= 0xFF00 && codepoint <= 0xFFEF)
      return 29263 + codepoint - 0xFF00;
    return -1;
  }

  void drawGlyph(TFT_eSPI &display, uint32_t codepoint, int x, int y,
                 uint16_t color, uint16_t background)
  {
    int32_t index = glyphIndex(codepoint);
    if (index < 0)
      index = glyphIndex('?');

    const size_t offset = sizeof(FontHeader) +
                          static_cast<size_t>(index) * BYTES_PER_GLYPH;
    if (!fontFile.seek(offset) || fontFile.read(bitmap, sizeof(bitmap)) != sizeof(bitmap))
      return;

    const int drawWidth = glyphAdvance(codepoint);
    for (int row = 0; row < GLYPH_HEIGHT; ++row)
    {
      const uint16_t bits = (static_cast<uint16_t>(bitmap[row * 2]) << 8) |
                            bitmap[row * 2 + 1];
      for (int column = 0; column < drawWidth; ++column)
        pixels[row * drawWidth + column] =
            (bits & (0x8000U >> column)) ? color : background;
    }
    display.pushImage(x, y, drawWidth, GLYPH_HEIGHT, pixels);
  }
};
