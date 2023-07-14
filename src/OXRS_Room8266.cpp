/*
 * OXRS_Room8266.cpp
 */

#include "Arduino.h"
#include "OXRS_Room8266.h"

#include <ESP8266WiFi.h>              // For networking
#include <Ethernet.h>                 // For networking
#include <Adafruit_NeoPixel.h>        // For RGBW LED
#include <LittleFS.h>                 // For file system access
#include <MqttLogger.h>               // For logging

#if defined(WIFI_MODE)
#include <WiFiManager.h>              // For WiFi AP config
#endif

// Macro for converting env vars to strings
#define STRINGIFY(s) STRINGIFY1(s)
#define STRINGIFY1(s) #s

// Network client (for MQTT)/server (for REST API)
#if defined(WIFI_MODE)
WiFiClient _client;
WiFiServer _server(REST_API_PORT);
#else
EthernetClient _client;
EthernetServer _server(REST_API_PORT);
#endif

// MQTT client
PubSubClient _mqttClient(_client);
OXRS_MQTT _mqtt(_mqttClient);

// REST API
OXRS_API _api(_mqtt);

// RGBW LED (actually GRBW)
Adafruit_NeoPixel _led(LED_COUNT, LED_PIN, NEO_GRBW);

// Logging (topic updated once MQTT connects successfully)
MqttLogger _logger(_mqttClient, "log", MqttLoggerMode::MqttAndSerial);

// Supported firmware config and command schemas
DynamicJsonDocument _fwConfigSchema(JSON_CONFIG_MAX_SIZE);
DynamicJsonDocument _fwCommandSchema(JSON_COMMAND_MAX_SIZE);

// MQTT callbacks wrapped by _mqttConfig/_mqttCommand
jsonCallback _onConfig;
jsonCallback _onCommand;

// Home Assistant self-discovery
bool g_hassDiscoveryEnabled = false;
char g_hassDiscoveryTopicPrefix[64] = "homeassistant";

// LED timer
uint32_t _ledOnMillis = 0L;

// stack size counter (for determine used heap size on ESP8266)
char * _stack_start;

/* Dirty hack to work out how much program space we are using */
uint32_t getStackSize()
{
  char stack;
  return (uint32_t)_stack_start - (uint32_t)&stack;  
}

/* JSON helpers */
void _mergeJson(JsonVariant dst, JsonVariantConst src)
{
  if (src.is<JsonObjectConst>())
  {
    for (JsonPairConst kvp : src.as<JsonObjectConst>())
    {
      if (dst[kvp.key()])
      {
        _mergeJson(dst[kvp.key()], kvp.value());
      }
      else
      {
        dst[kvp.key()] = kvp.value();
      }
    }
  }
  else
  {
    dst.set(src);
  }
}

/* LED helpers */
void _ledRGBW(uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
  _led.setPixelColor(0, r, g, b, w);
  _led.show();
}

void _ledRx(void)
{
  // yellow
  _ledRGBW(255, 255, 0, 0);
  _ledOnMillis = millis();
}

void _ledTx(void)
{
  // orange
  _ledRGBW(255, 100, 0, 0);
  _ledOnMillis = millis();
}

/* Adoption info builders */
void _getFirmwareJson(JsonVariant json)
{
  JsonObject firmware = json.createNestedObject("firmware");

  firmware["name"] = FW_NAME;
  firmware["shortName"] = FW_SHORT_NAME;
  firmware["maker"] = FW_MAKER;
  firmware["version"] = STRINGIFY(FW_VERSION);
  
#if defined(FW_GITHUB_URL)
  firmware["githubUrl"] = FW_GITHUB_URL;
#endif
}

void _getSystemJson(JsonVariant json)
{
  JsonObject system = json.createNestedObject("system");

  system["heapUsedBytes"] = getStackSize();
  system["heapFreeBytes"] = ESP.getFreeHeap();
  system["flashChipSizeBytes"] = ESP.getFlashChipSize();

  system["sketchSpaceUsedBytes"] = ESP.getSketchSize();
  system["sketchSpaceTotalBytes"] = ESP.getFreeSketchSpace();

  FSInfo fs_info;
  LittleFS.info(fs_info);
  system["fileSystemUsedBytes"] = fs_info.usedBytes;
  system["fileSystemTotalBytes"] = fs_info.totalBytes;
}

void _getNetworkJson(JsonVariant json)
{
  JsonObject network = json.createNestedObject("network");

  byte mac[6];

#if defined(WIFI_MODE)
  WiFi.macAddress(mac);

  network["mode"] = "wifi";
  network["ip"] = WiFi.localIP();
#else
  Ethernet.MACAddress(mac);

  network["mode"] = "ethernet";
  network["ip"] = Ethernet.localIP();
#endif

  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  network["mac"] = mac_display;
}

void _getConfigSchemaJson(JsonVariant json)
{
  JsonObject configSchema = json.createNestedObject("configSchema");
  
  // Config schema metadata
  configSchema["$schema"] = JSON_SCHEMA_VERSION;
  configSchema["title"] = FW_SHORT_NAME;
  configSchema["type"] = "object";

  JsonObject properties = configSchema.createNestedObject("properties");

  // Firmware config schema (if any)
  if (!_fwConfigSchema.isNull())
  {
    _mergeJson(properties, _fwConfigSchema.as<JsonVariant>());
  }

  // Home Assistant discovery config
  JsonObject hassDiscoveryEnabled = properties.createNestedObject("hassDiscoveryEnabled");
  hassDiscoveryEnabled["title"] = "Home Assistant Discovery";
  hassDiscoveryEnabled["description"] = "Publish Home Assistant discovery config (defaults to 'false`).";
  hassDiscoveryEnabled["type"] = "boolean";

  JsonObject hassDiscoveryTopicPrefix = properties.createNestedObject("hassDiscoveryTopicPrefix");
  hassDiscoveryTopicPrefix["title"] = "Home Assistant Discovery Topic Prefix";
  hassDiscoveryTopicPrefix["description"] = "Prefix for the Home Assistant discovery topic (defaults to 'homeassistant`).";
  hassDiscoveryTopicPrefix["type"] = "string";
}

void _getCommandSchemaJson(JsonVariant json)
{
  JsonObject commandSchema = json.createNestedObject("commandSchema");
  
  // Command schema metadata
  commandSchema["$schema"] = JSON_SCHEMA_VERSION;
  commandSchema["title"] = FW_SHORT_NAME;
  commandSchema["type"] = "object";

  JsonObject properties = commandSchema.createNestedObject("properties");

  // Firmware command schema (if any)
  if (!_fwCommandSchema.isNull())
  {
    _mergeJson(properties, _fwCommandSchema.as<JsonVariant>());
  }

  // Room8266 commands
  JsonObject restart = properties.createNestedObject("restart");
  restart["title"] = "Restart";
  restart["type"] = "boolean";
}

/* API callbacks */
void _apiAdopt(JsonVariant json)
{
  // Build device adoption info
  _getFirmwareJson(json);
  _getSystemJson(json);
  _getNetworkJson(json);
  _getConfigSchemaJson(json);
  _getCommandSchemaJson(json);
}

/* MQTT callbacks */
void _mqttConnected() 
{
  // MqttLogger doesn't copy the logging topic to an internal
  // buffer so we have to use a static array here
  static char logTopic[64];
  _logger.setTopic(_mqtt.getLogTopic(logTopic));

  // Publish device adoption info
  DynamicJsonDocument json(JSON_ADOPT_MAX_SIZE);
  _mqtt.publishAdopt(_api.getAdopt(json.as<JsonVariant>()));

  // Log the fact we are now connected
  _logger.println("[room] mqtt connected");
}

void _mqttDisconnected(int state) 
{
  // Log the disconnect reason
  // See https://github.com/knolleary/pubsubclient/blob/2d228f2f862a95846c65a8518c79f48dfc8f188c/src/PubSubClient.h#L44
  switch (state)
  {
    case MQTT_CONNECTION_TIMEOUT:
      _logger.println(F("[room] mqtt connection timeout"));
      break;
    case MQTT_CONNECTION_LOST:
      _logger.println(F("[room] mqtt connection lost"));
      break;
    case MQTT_CONNECT_FAILED:
      _logger.println(F("[room] mqtt connect failed"));
      break;
    case MQTT_DISCONNECTED:
      _logger.println(F("[room] mqtt disconnected"));
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      _logger.println(F("[room] mqtt bad protocol"));
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      _logger.println(F("[room] mqtt bad client id"));
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      _logger.println(F("[room] mqtt unavailable"));
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      _logger.println(F("[room] mqtt bad credentials"));
      break;      
    case MQTT_CONNECT_UNAUTHORIZED:
      _logger.println(F("[room] mqtt unauthorised"));
      break;      
  }
}

void _mqttConfig(JsonVariant json)
{
  // Home Assistant discovery config
  if (json.containsKey("hassDiscoveryEnabled"))
  {
    g_hassDiscoveryEnabled = json["hassDiscoveryEnabled"].as<bool>();
  }

  if (json.containsKey("hassDiscoveryTopicPrefix"))
  {
    strcpy(g_hassDiscoveryTopicPrefix, json["hassDiscoveryTopicPrefix"]);
  }

  // Pass on to the firmware callback
  if (_onConfig) { _onConfig(json); }
}

void _mqttCommand(JsonVariant json)
{
  // Check for Room8266 commands
  if (json.containsKey("restart") && json["restart"].as<bool>())
  {
    ESP.restart();
  }

  // Pass on to the firmware callback
  if (_onCommand) { _onCommand(json); }
}

void _mqttCallback(char * topic, byte * payload, int length) 
{
  // Update LED
  _ledRx();

  // Pass down to our MQTT handler and check it was processed ok
  int state = _mqtt.receive(topic, payload, length);
  switch (state)
  {
    case MQTT_RECEIVE_ZERO_LENGTH:
      _logger.println(F("[room] empty mqtt payload received"));
      break;
    case MQTT_RECEIVE_JSON_ERROR:
      _logger.println(F("[room] failed to deserialise mqtt json payload"));
      break;
    case MQTT_RECEIVE_NO_CONFIG_HANDLER:
      _logger.println(F("[room] no mqtt config handler"));
      break;
    case MQTT_RECEIVE_NO_COMMAND_HANDLER:
      _logger.println(F("[room] no mqtt command handler"));
      break;
  }
}

/* Main program */
void OXRS_Room8266::setMqttBroker(const char * broker, uint16_t port)
{
  _mqtt.setBroker(broker, port);
}

void OXRS_Room8266::setMqttClientId(const char * clientId)
{
  _mqtt.setClientId(clientId);
}

void OXRS_Room8266::setMqttAuth(const char * username, const char * password)
{
  _mqtt.setAuth(username, password);
}

void OXRS_Room8266::setMqttTopicPrefix(const char * prefix)
{
  _mqtt.setTopicPrefix(prefix);
}

void OXRS_Room8266::setMqttTopicSuffix(const char * suffix)
{
  _mqtt.setTopicSuffix(suffix);
}

void OXRS_Room8266::begin(jsonCallback config, jsonCallback command)
{
  // Store the address of the stack at startup so we can determine
  // the stack size at runtime (see getStackSize())
  char stack;
  _stack_start = &stack;

  // Get our firmware details
  DynamicJsonDocument json(512);
  _getFirmwareJson(json.as<JsonVariant>());

  // Log firmware details
  _logger.print(F("[room] "));
  serializeJson(json, _logger);
  _logger.println();

  // We wrap the callbacks so we can intercept messages intended for the Rack32
  _onConfig = config;
  _onCommand = command;
  
  // Set up the RGBW LED
  _initialiseLed();

  // Set up network and obtain an IP address
  byte mac[6];
  _initialiseNetwork(mac);

  // Set up MQTT (don't attempt to connect yet)
  _initialiseMqtt(mac);

  // Set up the REST API
  _initialiseRestApi();
}

void OXRS_Room8266::loop(void)
{
  // Check our network connection
  if (_isNetworkConnected())
  {
    // Maintain our DHCP lease
#if not defined(WIFI_MODE)
    Ethernet.maintain();
#endif
    
    // Handle any MQTT messages
    _mqtt.loop();
    
    // Handle any REST API requests
#if defined(WIFI_MODE)
    WiFiClient client = _server.available();
    _api.loop(&client);
#else
    EthernetClient client = _server.available();
    _api.loop(&client);
#endif
  }

  // Update the LED
  _updateLed();
}

void OXRS_Room8266::setConfigSchema(JsonVariant json)
{
  _fwConfigSchema.clear();
  _mergeJson(_fwConfigSchema.as<JsonVariant>(), json);
}

void OXRS_Room8266::setCommandSchema(JsonVariant json)
{
  _fwCommandSchema.clear();
  _mergeJson(_fwCommandSchema.as<JsonVariant>(), json);
}

void OXRS_Room8266::apiGet(const char * path, Router::Middleware * middleware)
{
  _api.get(path, middleware);
}

void OXRS_Room8266::apiPost(const char * path, Router::Middleware * middleware)
{
  _api.post(path, middleware);
}

bool OXRS_Room8266::publishStatus(JsonVariant json)
{
  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }

  bool success = _mqtt.publishStatus(json);
  if (success) { _ledTx(); }
  return success;
}

bool OXRS_Room8266::publishTelemetry(JsonVariant json)
{
  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }

  bool success = _mqtt.publishTelemetry(json);
  if (success) { _ledTx(); }
  return success;
}

bool OXRS_Room8266::isHassDiscoveryEnabled()
{
  return g_hassDiscoveryEnabled;
}

void OXRS_Room8266::getHassDiscoveryJson(JsonVariant json, char * id, char * name, bool isTelemetry)
{
  char uniqueId[64];
  sprintf_P(uniqueId, PSTR("%s_%s"), _mqtt.getClientId(), id);
  json["uniq_id"] = uniqueId;
  json["obj_id"] = uniqueId;
  json["name"] = name;

  char topic[64];
  json["stat_t"] = isTelemetry ? _mqtt.getTelemetryTopic(topic) : _mqtt.getStatusTopic(topic);
  json["avty_t"] = _mqtt.getLwtTopic(topic);
  json["avty_tpl"] = "{% if value_json.online == true %}online{% else %}offline{% endif %}";

  JsonObject dev = json.createNestedObject("dev");
  dev["name"] = _mqtt.getClientId();
  dev["mf"] = FW_MAKER;
  dev["mdl"] = FW_NAME;
  dev["sw"] = STRINGIFY(FW_VERSION);

  JsonArray ids = dev.createNestedArray("ids");
  ids.add(_mqtt.getClientId());
}

bool OXRS_Room8266::publishHassDiscovery(JsonVariant json, char * component, char * id)
{
  // Exit early if Home Assistant discovery has been disabled
  if (!g_hassDiscoveryEnabled) { return false; }

  // Exit early if no network connection
  if (!_isNetworkConnected()) { return false; }

  // Build the discovery topic
  char topic[64];
  sprintf_P(topic, PSTR("%s/%s/%s/%s/config"), g_hassDiscoveryTopicPrefix, component, _mqtt.getClientId(), id);

  // Check for a null payload and ensure we send an empty JSON object
  // to clear any existing Home Assistant config
  if (json.isNull())
  {
    json = json.to<JsonObject>();
  }

  bool success = _mqtt.publish(json, topic, true);
  if (success) { _ledTx(); }
  return success;
}

size_t OXRS_Room8266::write(uint8_t character)
{
  // Pass to logger - allows firmware to use `rack32.println("Log this!")`
  return _logger.write(character);
}

void OXRS_Room8266::_initialiseNetwork(byte * mac)
{
  // Get WiFi base MAC address
  WiFi.macAddress(mac);

#if not defined(WIFI_MODE)
  // Ethernet MAC address is base MAC + 3
  // See https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/system.html#mac-address
  mac[5] += 3;
#endif

  // Format the MAC address for logging
  char mac_display[18];
  sprintf_P(mac_display, PSTR("%02X:%02X:%02X:%02X:%02X:%02X"), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Attempt to connect
  bool success = false;
  
#if defined(WIFI_MODE)
  _logger.print(F("[room] wifi mac address: "));
  _logger.println(mac_display);

  // Ensure we are in the correct WiFi mode
  WiFi.mode(WIFI_STA);

  // Connect using saved creds, or start captive portal if none found
  // NOTE: Blocks until connected or the portal is closed
  WiFiManager wm;
  success = wm.autoConnect("OXRS_WiFi", "superhouse");

  _logger.print(F("[room] ip address: "));
  _logger.println(success ? WiFi.localIP() : IPAddress(0, 0, 0, 0));
#else
  _logger.print(F("[room] ethernet mac address: "));
  _logger.println(mac_display);

  // Initialise ethernet library
  Ethernet.init(ETHERNET_CS_PIN);

  // Reset Wiznet W5500
  pinMode(WIZNET_RESET_PIN, OUTPUT);
  digitalWrite(WIZNET_RESET_PIN, HIGH);
  delay(250);
  digitalWrite(WIZNET_RESET_PIN, LOW);
  delay(50);
  digitalWrite(WIZNET_RESET_PIN, HIGH);
  delay(350);

  // Connect ethernet and get an IP address via DHCP
  success = Ethernet.begin(mac, DHCP_TIMEOUT_MS, DHCP_RESPONSE_TIMEOUT_MS);
  
  _logger.print(F("[room] ip address: "));
  _logger.println(success ? Ethernet.localIP() : IPAddress(0, 0, 0, 0));
#endif
}

void OXRS_Room8266::_initialiseMqtt(byte * mac)
{
  // NOTE: this must be called *before* initialising the REST API since
  //       that will load MQTT config from file, which has precendence

  // Set the default client ID to last 3 bytes of the MAC address
  char clientId[32];
  sprintf_P(clientId, PSTR("%02x%02x%02x"), mac[3], mac[4], mac[5]);  
  _mqtt.setClientId(clientId);
  
  // Register our callbacks
  _mqtt.onConnected(_mqttConnected);
  _mqtt.onDisconnected(_mqttDisconnected);
  _mqtt.onConfig(_mqttConfig);
  _mqtt.onCommand(_mqttCommand);
  
  // Start listening for MQTT messages
  _mqttClient.setCallback(_mqttCallback);
}

void OXRS_Room8266::_initialiseRestApi(void)
{
  // NOTE: this must be called *after* initialising MQTT since that sets
  //       the default client id, which has lower precendence than MQTT
  //       settings stored in file and loaded by the API

  // Set up the REST API
  _api.begin();
  
  // Register our callbacks
  _api.onAdopt(_apiAdopt);

  // Start listening
  _server.begin();
}

void OXRS_Room8266::_initialiseLed(void)
{
  // Start the LED driver
  _led.begin();

  // Flash the LED to indicate we are booting
  _ledRGBW(255, 0, 0, 0);
  delay(500);
  _ledRGBW(0, 255, 0, 0);
  delay(500);
  _ledRGBW(0, 0, 255, 0);
  delay(500);
  _ledRGBW(0, 0, 0, 255);
  delay(500);
  _ledRGBW(0, 0, 0, 0);
}

void OXRS_Room8266::_updateLed(void)
{
  // Don't bother with network checks if we have an activity LED showing
  if (_ledOnMillis)
  {
    // Turn off LED if timed out
    if ((millis() - _ledOnMillis) > LED_TIMEOUT_MS)
    {
      _ledRGBW(0, 0, 0, 0);
      _ledOnMillis = 0L;
    }
  }
  else
  {
    // Check network connection state
    if (!_isNetworkConnected())
    {
      // RED if no network at all
      _ledRGBW(50, 0, 0, 0);
    }
    else
    {
      if (!_mqtt.connected())
      {
        // BLUE if network, but no MQTT connection
        _ledRGBW(0, 0, 50, 0);        
      }
      else
      {
        // GREEN if everything ok
        _ledRGBW(0, 50, 0, 0);        
      }
    }
  }
}

bool OXRS_Room8266::_isNetworkConnected(void)
{
#if defined(WIFI_MODE)
  return WiFi.status() == WL_CONNECTED;
#else
  return Ethernet.linkStatus() == LinkON;
#endif
}