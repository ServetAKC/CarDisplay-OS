#include "deviceConfig.h"

#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>

namespace
{
constexpr const char *CONFIG_PATH = "/spotify_diy_config.json";

constexpr const char *KEY_REFRESH_TOKEN = "refreshToken";
constexpr const char *KEY_CLIENT_ID = "clientId";
constexpr const char *KEY_CLIENT_SECRET = "clientSecret";
constexpr const char *KEY_MARKET = "market";
constexpr const char *KEY_TIMEZONE = "timezone";
constexpr const char *KEY_LATITUDE = "latitude";
constexpr const char *KEY_LONGITUDE = "longitude";

// refreshToken alone is up to 399 characters, so the document has to comfortably
// exceed it once the remaining fields and JSON punctuation are added.
constexpr size_t CONFIG_DOC_SIZE = 1024;
} // namespace

DeviceConfig deviceConfig;

void DeviceConfig::setDefaults()
{
  refreshToken[0] = '\0';
  clientId[0] = '\0';
  clientSecret[0] = '\0';
  copyField(market, CARDISPLAY_DEFAULT_MARKET);
  copyField(timezone, CARDISPLAY_DEFAULT_TZ);
  latitude = CARDISPLAY_DEFAULT_LATITUDE;
  longitude = CARDISPLAY_DEFAULT_LONGITUDE;
}

bool DeviceConfig::hasSpotifyCredentials() const
{
  return clientId[0] != '\0' && clientSecret[0] != '\0';
}

bool loadDeviceConfig(DeviceConfig &config)
{
  config.setDefaults();

  if (!SPIFFS.exists(CONFIG_PATH))
  {
    Serial.println(F("Config file does not exist; using defaults"));
    return false;
  }

  fs::File configFile = SPIFFS.open(CONFIG_PATH, "r");
  if (!configFile)
  {
    Serial.println(F("Failed to open config file"));
    return false;
  }

  StaticJsonDocument<CONFIG_DOC_SIZE> json;
  const DeserializationError error = deserializeJson(json, configFile);
  configFile.close();

  if (error)
  {
    Serial.print(F("Failed to parse config: "));
    Serial.println(error.c_str());
    return false;
  }

  copyField(config.refreshToken, json[KEY_REFRESH_TOKEN] | "");
  copyField(config.clientId, json[KEY_CLIENT_ID] | "");
  copyField(config.clientSecret, json[KEY_CLIENT_SECRET] | "");

  // Regional settings are optional so configs written by v0.3.15 and earlier
  // still load; anything missing keeps its default.
  copyField(config.market, json[KEY_MARKET] | CARDISPLAY_DEFAULT_MARKET);
  copyField(config.timezone, json[KEY_TIMEZONE] | CARDISPLAY_DEFAULT_TZ);
  config.latitude = json[KEY_LATITUDE] | CARDISPLAY_DEFAULT_LATITUDE;
  config.longitude = json[KEY_LONGITUDE] | CARDISPLAY_DEFAULT_LONGITUDE;

  if (config.market[0] == '\0')
    copyField(config.market, CARDISPLAY_DEFAULT_MARKET);
  if (config.timezone[0] == '\0')
    copyField(config.timezone, CARDISPLAY_DEFAULT_TZ);

  if (!config.hasSpotifyCredentials())
  {
    Serial.println(F("Config has an empty client ID or secret"));
    return false;
  }

  Serial.print(F("Config loaded. Market="));
  Serial.print(config.market);
  Serial.print(F(" TZ="));
  Serial.println(config.timezone);
  return true;
}

bool saveDeviceConfig(const DeviceConfig &config)
{
  Serial.println(F("Saving config"));

  StaticJsonDocument<CONFIG_DOC_SIZE> json;
  json[KEY_REFRESH_TOKEN] = config.refreshToken;
  json[KEY_CLIENT_ID] = config.clientId;
  json[KEY_CLIENT_SECRET] = config.clientSecret;
  json[KEY_MARKET] = config.market;
  json[KEY_TIMEZONE] = config.timezone;
  json[KEY_LATITUDE] = config.latitude;
  json[KEY_LONGITUDE] = config.longitude;

  fs::File configFile = SPIFFS.open(CONFIG_PATH, "w");
  if (!configFile)
  {
    // v0.3.15 logged this and then serialised into the invalid handle anyway.
    Serial.println(F("Failed to open config file for writing"));
    return false;
  }

  const size_t written = serializeJson(json, configFile);
  configFile.close();

  if (written == 0)
  {
    Serial.println(F("Failed to write config file"));
    return false;
  }
  return true;
}
