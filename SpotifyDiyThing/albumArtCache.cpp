#include "albumArtCache.h"

#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <SD.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "cydTheme.h"
#include "timing.h"

namespace
{
constexpr const char *ALBUM_ART = "/album.jpg";
constexpr const char *ALBUM_ART_TEMP = "/album.tmp";
constexpr const char *ALBUM_CACHE_DIR = "/hondathing_cache";

constexpr uint32_t RAW_CACHE_MAGIC = 0x48544331UL; // "HTC1"

struct RawCacheHeader
{
  uint32_t magic;
  uint16_t width;
  uint16_t height;
};

constexpr size_t RAW_CACHE_BYTES =
    sizeof(RawCacheHeader) +
    static_cast<size_t>(AlbumArtCache::CACHED_ART_SIZE) *
        AlbumArtCache::CACHED_ART_SIZE * sizeof(uint16_t);

// -------------------------------------------------------------------------
// JPEGDEC plumbing.
//
// JPEGDEC uses C callbacks, so the file being read has to reach them somehow.
// Rather than two near-identical sets of globals (which is what v0.3.15 had),
// the source is carried in JPEGFILE::fHandle and the draw destination in
// JPEGDRAW::pUser. Only the open callback needs a per-decoder variant, because
// its signature has no user pointer - everything below it is shared.
// -------------------------------------------------------------------------
struct JpegSource
{
  fs::FS *fs = nullptr;
  fs::File file;
};

JpegSource displaySource; // Arduino loop task: decodes straight to the panel
JpegSource cacheSource;   // background task: decodes into the raw SD cache

void *openSource(JpegSource &source, const char *filename, int32_t *size)
{
  if (source.fs == nullptr)
  {
    *size = 0;
    return nullptr;
  }
  source.file = source.fs->open(filename, FILE_READ);
  *size = source.file ? source.file.size() : 0;
  return source.file ? &source : nullptr;
}

void *openDisplaySource(const char *filename, int32_t *size)
{
  return openSource(displaySource, filename, size);
}

void *openCacheSource(const char *filename, int32_t *size)
{
  return openSource(cacheSource, filename, size);
}

void closeSource(void *handle)
{
  JpegSource *source = static_cast<JpegSource *>(handle);
  if (source != nullptr && source->file)
    source->file.close();
}

int32_t readSource(JPEGFILE *handle, uint8_t *buffer, int32_t length)
{
  JpegSource *source = static_cast<JpegSource *>(handle->fHandle);
  if (source == nullptr || !source->file)
    return 0;
  return source->file.read(buffer, length);
}

int32_t seekSource(JPEGFILE *handle, int32_t position)
{
  JpegSource *source = static_cast<JpegSource *>(handle->fHandle);
  if (source == nullptr || !source->file)
    return 0;
  return source->file.seek(position);
}

int drawToPanel(JPEGDRAW *pDraw)
{
  if (pDraw->y >= tft.height())
    return 0;
  tft.pushImage(pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight, pDraw->pPixels);
  return 1;
}

// Writes one decoded MCU block into the pre-sized raw cache file carried in
// JPEGDRAW::pUser.
int drawToRawCache(JPEGDRAW *pDraw)
{
  fs::File *output = static_cast<fs::File *>(pDraw->pUser);
  constexpr int SIZE = AlbumArtCache::CACHED_ART_SIZE;

  if (output == nullptr || !*output || pDraw->x >= SIZE || pDraw->y >= SIZE)
    return 0;

  const int clippedWidth = min(pDraw->iWidth, SIZE - pDraw->x);
  const int clippedHeight = min(pDraw->iHeight, SIZE - pDraw->y);
  const size_t rowBytes = clippedWidth * sizeof(uint16_t);

  for (int row = 0; row < clippedHeight; ++row)
  {
    const size_t pixelOffset =
        (static_cast<size_t>(pDraw->y + row) * SIZE + pDraw->x) * sizeof(uint16_t);
    if (!output->seek(sizeof(RawCacheHeader) + pixelOffset))
      return 0;

    const uint8_t *source =
        reinterpret_cast<const uint8_t *>(pDraw->pPixels + (row * pDraw->iWidth));
    if (output->write(source, rowBytes) != rowBytes)
      return 0;
  }
  return 1;
}

JPEGDEC displayJpeg;
JPEGDEC cacheJpeg;

// FNV-1a: small, deterministic and plenty for cache filenames.
uint32_t urlHash(const char *text)
{
  uint32_t hash = 2166136261UL;
  while (text != nullptr && *text)
  {
    hash ^= static_cast<uint8_t>(*text++);
    hash *= 16777619UL;
  }
  return hash;
}

// Streams `source` into `destination` in 4 KB chunks.
bool copyStream(fs::File &source, fs::File &destination)
{
  uint8_t buffer[4096];
  size_t total = 0;

  while (source.available())
  {
    const size_t bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead == 0)
      break;
    if (destination.write(buffer, bytesRead) != bytesRead)
      return false;
    total += bytesRead;
    taskYIELD();
  }

  destination.flush();
  return total > 0;
}
} // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AlbumArtCache::begin()
{
  // The CYD microSD slot uses the ESP32 VSPI pins (SCK 18, MISO 19, MOSI 23,
  // CS 5). The cache is optional: the player still works with no card inserted.
  sdCacheReady = SD.begin(SD_CS_PIN);
  if (sdCacheReady)
  {
    if (!SD.exists(ALBUM_CACHE_DIR))
      SD.mkdir(ALBUM_CACHE_DIR);
    Serial.print(F("SD album cache ready. Card MB: "));
    Serial.println(static_cast<uint32_t>(SD.cardSize() / (1024ULL * 1024ULL)));
  }
  else
  {
    Serial.println(F("SD album cache unavailable; using network only"));
  }

  requestQueue = xQueueCreate(1, sizeof(Request));
  if (requestQueue == nullptr)
  {
    Serial.println(F("Failed to create album-art queue"));
    return false;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      taskTrampoline, "albumDownload", 8192, this, 1, &taskHandle, 0);

  if (result != pdPASS)
  {
    Serial.println(F("Failed to start album-art task"));
    taskHandle = nullptr;
    return false;
  }
  return true;
}

void AlbumArtCache::setPaused(bool paused)
{
  pausedFlag = paused;
}

void AlbumArtCache::allowNetworkStart()
{
  networkStartAllowed = true;
}

bool AlbumArtCache::retryDue() const
{
  return timeReached(nextRetryTime);
}

void AlbumArtCache::request(const char *albumArtUrl)
{
  if (requestQueue == nullptr || taskHandle == nullptr || albumArtUrl == nullptr)
    return;

  Request request = {};
  copyField(request.url, albumArtUrl);
  request.generation = ++requestGeneration;

  downloadActive = true;
  networkStartAllowed = false;
  nextRetryTime = deadlineIn(RETRY_INTERVAL_MS);

  xQueueOverwrite(requestQueue, &request);
}

// ---------------------------------------------------------------------------
// Hand-off, run on the Arduino loop task
// ---------------------------------------------------------------------------

AlbumArtCache::Handoff AlbumArtCache::serviceHandoff()
{
  if (!handoffReady)
  {
    // processImageInfo() runs inside the Spotify response callback. Let that
    // HTTPS request finish before the second TLS connection starts.
    if (downloadActive)
      networkStartAllowed = true;
    return Handoff::Nothing;
  }

  const uint32_t readyGeneration = handoffGeneration;
  const Source readySource = handoffSource;
  char readyPath[sizeof(handoffPath)] = {};
  copyField(readyPath, handoffPath);

  if (readyGeneration != requestGeneration)
  {
    if (readySource == Source::SpiffsTemp && SPIFFS.exists(ALBUM_ART_TEMP))
      SPIFFS.remove(ALBUM_ART_TEMP);
    handoffReady = false;
    downloadActive = false;
    return Handoff::Discarded;
  }

  bool fileReady = false;
  switch (readySource)
  {
  case Source::SpiffsTemp:
    if (SPIFFS.exists(ALBUM_ART))
      SPIFFS.remove(ALBUM_ART);
    fileReady = SPIFFS.exists(ALBUM_ART_TEMP) &&
                SPIFFS.rename(ALBUM_ART_TEMP, ALBUM_ART);
    if (fileReady)
    {
      currentSource = Source::SpiffsCurrent;
      copyField(currentPath, ALBUM_ART);
    }
    break;

  case Source::SdRaw:
  case Source::SdJpeg:
    fileReady = sdCacheReady && SD.exists(readyPath);
    if (fileReady)
    {
      currentSource = readySource;
      copyField(currentPath, readyPath);
    }
    break;

  default:
    break;
  }

  handoffSource = Source::None;
  handoffPath[0] = '\0';
  handoffReady = false;
  downloadActive = false;

  if (!fileReady)
  {
    nextRetryTime = deadlineIn(RETRY_INTERVAL_MS);
    Serial.println(F("Album-art cache/temp file was not ready"));
    return Handoff::Failed;
  }
  return Handoff::Ready;
}

// ---------------------------------------------------------------------------
// Drawing, run on the Arduino loop task
// ---------------------------------------------------------------------------

bool AlbumArtCache::drawCurrent()
{
  bool drawn = false;

  if (currentPath[0] != '\0')
  {
    switch (currentSource)
    {
    case Source::SdRaw:
      drawn = drawRawCacheFile(currentPath);
      break;
    case Source::SdJpeg:
      drawn = drawJpegFile(SD, currentPath);
      break;
    case Source::SpiffsCurrent:
      drawn = drawJpegFile(SPIFFS, currentPath);
      break;
    default:
      break;
    }
  }

  // Backward-compatible startup fallback for the last downloaded cover.
  if (!drawn && currentSource == Source::None && SPIFFS.exists(ALBUM_ART))
  {
    currentSource = Source::SpiffsCurrent;
    copyField(currentPath, ALBUM_ART);
    drawn = drawJpegFile(SPIFFS, ALBUM_ART);
  }

  return drawn;
}

bool AlbumArtCache::drawJpegFile(fs::FS &sourceFs, const char *path)
{
  const unsigned long started = millis();
  displaySource.fs = &sourceFs;

  if (displayJpeg.open(path, openDisplaySource, closeSource,
                       readSource, seekSource, drawToPanel) != 1)
  {
    Serial.println(F("JPEG open failed"));
    return false;
  }

  displayJpeg.setPixelType(1);
  const int decodeStatus = displayJpeg.decode(layout::IMAGE_X, layout::IMAGE_Y,
                                              JPEG_SCALE_QUARTER);
  displayJpeg.close();

  tft.drawRect(layout::IMAGE_X - 1, layout::IMAGE_Y - 1,
               layout::IMAGE_SIZE + 2, layout::IMAGE_SIZE + 2, theme::GREEN);

  Serial.print(F("JPEG decode+draw (ms): "));
  Serial.println(millis() - started);
  return decodeStatus == 1;
}

bool AlbumArtCache::drawRawCacheFile(const char *rawPath)
{
  if (!isValidRawCache(rawPath))
    return false;

  const unsigned long started = millis();
  fs::File file = SD.open(rawPath, FILE_READ);
  if (!file)
    return false;

  RawCacheHeader header = {};
  if (file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header))
  {
    file.close();
    return false;
  }

  // Four rows at a time: 1280 bytes of static buffer, and few enough SD reads
  // that this stays well under the JPEG path.
  static uint16_t pixelRows[CACHED_ART_SIZE * 4];
  for (int y = 0; y < CACHED_ART_SIZE;)
  {
    const int rows = min(4, CACHED_ART_SIZE - y);
    const size_t bytesNeeded = static_cast<size_t>(CACHED_ART_SIZE) * rows * sizeof(uint16_t);
    if (file.read(reinterpret_cast<uint8_t *>(pixelRows), bytesNeeded) != bytesNeeded)
    {
      file.close();
      return false;
    }
    tft.pushImage(layout::IMAGE_X, layout::IMAGE_Y + y, CACHED_ART_SIZE, rows, pixelRows);
    y += rows;
  }

  file.close();
  tft.drawRect(layout::IMAGE_X - 1, layout::IMAGE_Y - 1,
               layout::IMAGE_SIZE + 2, layout::IMAGE_SIZE + 2, theme::GREEN);
  Serial.print(F("RAW cache draw (ms): "));
  Serial.println(millis() - started);
  return true;
}

// ---------------------------------------------------------------------------
// Background task
// ---------------------------------------------------------------------------

void AlbumArtCache::taskTrampoline(void *parameter)
{
  static_cast<AlbumArtCache *>(parameter)->taskLoop();
}

void AlbumArtCache::taskLoop()
{
  Request request = {};

  while (true)
  {
    while (handoffReady || pausedFlag)
      vTaskDelay(pdMS_TO_TICKS(10));

    if (xQueueReceive(requestQueue, &request, portMAX_DELAY) != pdTRUE)
      continue;

    while (pausedFlag || !networkStartAllowed || spotifyControlCommandPending)
      vTaskDelay(pdMS_TO_TICKS(5));
    networkStartAllowed = false;

    char jpegCachePath[64] = {};
    char rawCachePath[64] = {};
    buildJpegCachePath(request.url, jpegCachePath, sizeof(jpegCachePath));
    buildRawCachePath(request.url, rawCachePath, sizeof(rawCachePath));

    bool ready = false;
    Source readySource = Source::None;
    const char *readyPath = "";

    if (sdCacheReady && isValidRawCache(rawCachePath))
    {
      // Fastest path: pre-decoded RGB565, no JPEG work and no SD->SPIFFS copy.
      Serial.print(F("Raw album cache hit: "));
      Serial.println(rawCachePath);
      ready = true;
      readySource = Source::SdRaw;
      readyPath = rawCachePath;
    }
    else if (sdCacheReady && SD.exists(jpegCachePath))
    {
      // Older JPEG-only caches stay compatible; they are promoted to .rgb below.
      Serial.print(F("JPEG album cache hit: "));
      Serial.println(jpegCachePath);
      ready = true;
      readySource = Source::SdJpeg;
      readyPath = jpegCachePath;
    }
    else
    {
      Serial.print(F("Album cache miss; downloading: "));
      Serial.println(request.url);
      ready = downloadToSpiffsTemp(request.url, request.generation);
      readySource = ready ? Source::SpiffsTemp : Source::None;

      if (ready && sdCacheReady && copyTempToSdCache(jpegCachePath))
      {
        Serial.print(F("Saved album JPEG to SD cache: "));
        Serial.println(jpegCachePath);
      }
    }

    if (request.generation != requestGeneration)
    {
      if (readySource == Source::SpiffsTemp && SPIFFS.exists(ALBUM_ART_TEMP))
        SPIFFS.remove(ALBUM_ART_TEMP);
      continue;
    }

    if (!ready)
    {
      downloadActive = false;
      nextRetryTime = deadlineIn(RETRY_INTERVAL_MS);
      Serial.println(F("Background album-art download failed"));
      continue;
    }

    copyField(handoffPath, readyPath);
    handoffSource = readySource;
    handoffGeneration = request.generation;
    handoffReady = true;

    // Wait only until the main loop has consumed this cover. The UI stays
    // responsive because everything below runs on the background core.
    while (handoffReady && request.generation == requestGeneration)
      vTaskDelay(pdMS_TO_TICKS(10));

    if (request.generation != requestGeneration || !sdCacheReady)
      continue;

    while (pausedFlag && request.generation == requestGeneration)
      vTaskDelay(pdMS_TO_TICKS(15));

    if (request.generation != requestGeneration)
      continue;

    // Promote the JPEG to a screen-native blob. This is what makes later cache
    // hits genuinely fast rather than merely offline.
    if (!isValidRawCache(rawCachePath) && SD.exists(jpegCachePath))
    {
      Serial.print(F("Building fast raw cache: "));
      Serial.println(rawCachePath);
      createRawCacheFromJpeg(jpegCachePath, rawCachePath);
    }

    prefetchQueuedCovers(request.url, request.generation);
  }
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

bool AlbumArtCache::cancelled(const TransferOptions &options) const
{
  if (pausedFlag || options.generation != requestGeneration)
    return true;
  return options.abortOnControlCommand && spotifyControlCommandPending;
}

// One implementation for both the on-screen cover and the prefetch window;
// v0.3.15 carried two ~90-line copies of this loop that had already started to
// drift apart in their timeout and cancellation handling.
bool AlbumArtCache::streamToFile(const char *url, fs::File &destination,
                                 const TransferOptions &options)
{
  if (WiFi.status() != WL_CONNECTED || url == nullptr)
    return false;

  WiFiClientSecure imageClient;
  // Album art comes from Spotify's CDN, which rotates hosts and certificates.
  // The payload is only ever fed to the JPEG decoder and is size-capped below,
  // so it is not worth pinning a CA here - but the cap is not optional.
  imageClient.setInsecure();
  imageClient.setTimeout(options.timeoutMs);

  HTTPClient http;
  http.setTimeout(options.timeoutMs);

  bool success = false;

  if (http.begin(imageClient, url))
  {
    const int code = http.GET();
    if (code == HTTP_CODE_OK)
    {
      const int declaredSize = http.getSize();
      if (declaredSize > static_cast<int>(MAX_COVER_BYTES))
      {
        Serial.print(F("Cover rejected, too large: "));
        Serial.println(declaredSize);
        http.end();
        imageClient.stop();
        return false;
      }

      WiFiClient *stream = http.getStreamPtr();
      int remaining = declaredSize;
      uint8_t buffer[2048];
      size_t total = 0;
      unsigned long lastDataAt = millis();
      bool aborted = false;

      while (http.connected() && (remaining > 0 || remaining == -1))
      {
        if (cancelled(options))
        {
          aborted = true;
          break;
        }

        const size_t available = stream->available();
        if (available > 0)
        {
          const size_t toRead = min(available, sizeof(buffer));
          const int bytesRead = stream->readBytes(buffer, toRead);
          if (bytesRead <= 0)
            break;
          if (destination.write(buffer, bytesRead) != static_cast<size_t>(bytesRead))
            break;

          total += bytesRead;
          if (total > MAX_COVER_BYTES)
          {
            // Chunked responses report remaining == -1, so the cap has to be
            // enforced on the running total as well as the declared length.
            Serial.println(F("Cover exceeded size cap; aborting"));
            aborted = true;
            break;
          }
          if (remaining > 0)
            remaining -= bytesRead;
          lastDataAt = millis();
          taskYIELD();
        }
        else
        {
          if (elapsed(lastDataAt, options.timeoutMs))
            break;
          vTaskDelay(pdMS_TO_TICKS(5));
        }
      }

      destination.flush();
      success = !aborted && total > 0 && !cancelled(options) &&
                (remaining <= 0 || !http.connected());

      Serial.print(F("Album-art bytes: "));
      Serial.println(total);
    }
    else
    {
      Serial.print(F("Album-art HTTP error: "));
      Serial.println(code);
    }
    http.end();
  }
  else
  {
    Serial.println(F("Could not begin album-art HTTPS request"));
  }

  imageClient.stop();
  return success;
}

bool AlbumArtCache::downloadToSpiffsTemp(const char *url, uint32_t generation)
{
  if (SPIFFS.exists(ALBUM_ART_TEMP))
    SPIFFS.remove(ALBUM_ART_TEMP);

  fs::File file = SPIFFS.open(ALBUM_ART_TEMP, "w");
  if (!file)
  {
    Serial.println(F("Could not open album temp file"));
    return false;
  }

  const TransferOptions options = {generation, false, 12000};
  const bool success = streamToFile(url, file, options) && file.size() > 0;
  file.close();

  if (!success && SPIFFS.exists(ALBUM_ART_TEMP))
    SPIFFS.remove(ALBUM_ART_TEMP);
  return success;
}

bool AlbumArtCache::downloadToSdCache(const char *url, const char *cachePath,
                                      uint32_t generation)
{
  if (!sdCacheReady || cachePath == nullptr)
    return false;

  char tempPath[72];
  snprintf(tempPath, sizeof(tempPath), "%s.tmp", cachePath);
  if (SD.exists(tempPath))
    SD.remove(tempPath);

  fs::File file = SD.open(tempPath, FILE_WRITE);
  if (!file)
    return false;

  const TransferOptions options = {generation, true, 8000};
  const bool success = streamToFile(url, file, options);
  file.close();

  if (!success)
  {
    if (SD.exists(tempPath))
      SD.remove(tempPath);
    return false;
  }

  if (SD.exists(cachePath))
    SD.remove(cachePath);
  if (!SD.rename(tempPath, cachePath))
  {
    SD.remove(tempPath);
    return false;
  }
  return true;
}

bool AlbumArtCache::copyTempToSdCache(const char *cachePath)
{
  if (!sdCacheReady || !SPIFFS.exists(ALBUM_ART_TEMP))
    return false;

  if (SD.exists(cachePath))
    return true;

  fs::File source = SPIFFS.open(ALBUM_ART_TEMP, "r");
  if (!source)
    return false;

  fs::File destination = SD.open(cachePath, FILE_WRITE);
  if (!destination)
  {
    source.close();
    return false;
  }

  const bool success = copyStream(source, destination);
  source.close();
  destination.close();

  if (!success && SD.exists(cachePath))
    SD.remove(cachePath);
  return success;
}

// ---------------------------------------------------------------------------
// Raw cache
// ---------------------------------------------------------------------------

void AlbumArtCache::buildJpegCachePath(const char *url, char *out, size_t outSize) const
{
  snprintf(out, outSize, "%s/%08lx.jpg", ALBUM_CACHE_DIR,
           static_cast<unsigned long>(urlHash(url)));
}

void AlbumArtCache::buildRawCachePath(const char *url, char *out, size_t outSize) const
{
  snprintf(out, outSize, "%s/%08lx.rgb", ALBUM_CACHE_DIR,
           static_cast<unsigned long>(urlHash(url)));
}

bool AlbumArtCache::isValidRawCache(const char *rawPath) const
{
  if (!sdCacheReady || rawPath == nullptr || !SD.exists(rawPath))
    return false;

  fs::File file = SD.open(rawPath, FILE_READ);
  if (!file)
    return false;

  RawCacheHeader header = {};
  const size_t read = file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header));
  const bool valid = read == sizeof(header) &&
                     header.magic == RAW_CACHE_MAGIC &&
                     header.width == CACHED_ART_SIZE &&
                     header.height == CACHED_ART_SIZE &&
                     file.size() == RAW_CACHE_BYTES;
  file.close();
  return valid;
}

bool AlbumArtCache::createRawCacheFromJpeg(const char *jpegPath, const char *rawPath)
{
  if (!sdCacheReady || jpegPath == nullptr || rawPath == nullptr || !SD.exists(jpegPath))
    return false;

  char tempPath[72];
  snprintf(tempPath, sizeof(tempPath), "%s.tmp", rawPath);
  if (SD.exists(tempPath))
    SD.remove(tempPath);

  fs::File output = SD.open(tempPath, FILE_WRITE);
  if (!output)
    return false;

  const RawCacheHeader header = {RAW_CACHE_MAGIC, CACHED_ART_SIZE, CACHED_ART_SIZE};
  bool prepared =
      output.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) == sizeof(header);

  // Pre-size the file so the seeking draw callback never leaves a hole at the
  // end when the last MCU row lands short.
  if (prepared)
    prepared = output.seek(RAW_CACHE_BYTES - 1) && output.write(static_cast<uint8_t>(0)) == 1;

  if (!prepared)
  {
    output.close();
    SD.remove(tempPath);
    return false;
  }
  output.flush();

  cacheSource.fs = &SD;
  cacheJpeg.setUserPointer(&output);
  if (cacheJpeg.open(jpegPath, openCacheSource, closeSource,
                     readSource, seekSource, drawToRawCache) != 1)
  {
    output.close();
    SD.remove(tempPath);
    return false;
  }

  cacheJpeg.setPixelType(1);
  const int decodeStatus = cacheJpeg.decode(0, 0, JPEG_SCALE_QUARTER);
  cacheJpeg.close();
  output.flush();
  output.close();

  bool valid = decodeStatus == 1;
  if (valid)
  {
    fs::File verify = SD.open(tempPath, FILE_READ);
    valid = verify && verify.size() == RAW_CACHE_BYTES;
    if (verify)
      verify.close();
  }

  if (!valid)
  {
    SD.remove(tempPath);
    return false;
  }

  if (SD.exists(rawPath))
    SD.remove(rawPath);
  if (!SD.rename(tempPath, rawPath))
  {
    SD.remove(tempPath);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Rolling prefetch window
// ---------------------------------------------------------------------------

void AlbumArtCache::prefetchQueuedCovers(const char *currentUrl, uint32_t generation)
{
  const TransferOptions gate = {generation, true, 8000};
  if (!sdCacheReady || cancelled(gate))
    return;

  const size_t queuedCount = spotifyGetQueuedAlbumArtUrls(queuedUrls, QUEUE_PREFETCH_LIMIT);
  if (queuedCount == 0)
    return;

  Serial.print(F("Queue covers found: "));
  Serial.println(queuedCount);

  for (size_t index = 0; index < queuedCount; index++)
  {
    if (cancelled(gate))
    {
      Serial.println(F("Queue-cover prefetch stopped for overlay/track/control change"));
      return;
    }

    const char *nextUrl = queuedUrls[index];
    if (nextUrl[0] == '\0' || strcmp(nextUrl, currentUrl) == 0)
      continue;

    char nextJpegPath[64] = {};
    char nextRawPath[64] = {};
    buildJpegCachePath(nextUrl, nextJpegPath, sizeof(nextJpegPath));
    buildRawCachePath(nextUrl, nextRawPath, sizeof(nextRawPath));

    // A cache hit must not stop the chain; skip it and keep filling ahead.
    if (isValidRawCache(nextRawPath))
      continue;

    if (!SD.exists(nextJpegPath))
    {
      Serial.print(F("Prefetching queued cover "));
      Serial.print(index + 1);
      Serial.print('/');
      Serial.println(queuedCount);

      if (!downloadToSdCache(nextUrl, nextJpegPath, generation))
      {
        Serial.println(F("Queued-cover prefetch cancelled/failed"));
        return;
      }
    }

    if (cancelled(gate))
      return;

    if (SD.exists(nextJpegPath) && !isValidRawCache(nextRawPath))
    {
      if (createRawCacheFromJpeg(nextJpegPath, nextRawPath))
        Serial.println(F("Queued cover ready in fast cache"));
    }

    // Be polite to the UI and SD task scheduling between covers.
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  Serial.println(F("Rolling queue cover cache is filled"));
}
