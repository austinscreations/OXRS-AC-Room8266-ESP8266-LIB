/*
 * OXRS_Room8266.h
 */

#ifndef OXRS_ROOM8266_H
#define OXRS_ROOM8266_H

#include <OXRS_MQTT.h>                // For MQTT pub/sub
#include <OXRS_API.h>                 // For REST API

// Ethernet
#define       ETHERNET_CS_PIN           15
#define       WIZNET_RESET_PIN          2
#define       DHCP_TIMEOUT_MS           15000
#define       DHCP_RESPONSE_TIMEOUT_MS  4000

// I2C
#define       I2C_SDA                   4
#define       I2C_SCL                   5

// RGBW LED
#define       LED_PIN                   0
#define       LED_COUNT                 1
#define       LED_TIMEOUT_MS            50

// REST API
#define       REST_API_PORT             80

class OXRS_Room8266 : public Print
{
  public:
    void begin(jsonCallback config, jsonCallback command);
    void loop(void);

    // Firmware can define the config/commands it supports - for device discovery and adoption
    void setConfigSchema(JsonVariant json);
    void setCommandSchema(JsonVariant json);

    // Return a pointer to the MQTT library
    OXRS_MQTT * getMQTT(void);

    // Return a pointer to the API library
    OXRS_API * getAPI(void);

    // Helpers for publishing to stat/ and tele/ topics
    bool publishStatus(JsonVariant json);
    bool publishTelemetry(JsonVariant json);

    // Implement Print.h wrapper
    virtual size_t write(uint8_t);
    using Print::write;

  private:
    void _initialiseNetwork(byte * mac);
    void _initialiseMqtt(byte * mac);
    void _initialiseRestApi(void);
    
    void _initialiseLed(void);
    void _updateLed(void);

    bool _isNetworkConnected(void);
};

#endif
