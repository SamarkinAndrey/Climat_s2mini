#pragma once

#include "_common.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_mac.h>

#define MQTT_CONNECTION_CHECK_DELAY 30ul * 1000ul
#define MQTT_PUBLISH_DELAY          5ul * 1000ul

#define MQTT_TOPIC_SIZE 72
#define MQTT_PAYLOAD_SIZE 52

const dbParams mqttParams[] = {
    TemperatureControlEnabled,
    TemperatureAlarmThresholdLow,
    TemperatureAlarmThresholdHigh,
    TemperatureFanDuration,
    TemperatureFanDelay,
    TemperatureFanEffectiveThreshold,
    TemperatureFanNoEffectDelay,
    TemperatureHeatingDuration,
    TemperatureHeatingDelay,
    TemperatureHeatingEffectiveThreshold,
    TemperatureHeatingNoEffectDelay,
    TemperatureInStackSize,
    TemperatureOutStackSize,
    TemperatureOffset,
    TemperatureHysteresis,

    HumidityControlEnabled,
    HumidityAlarmThresholdLow,
    HumidityAlarmThresholdHigh,
    HumidityFanDuration,
    HumidityFanDelay,
    HumidityFanEffectiveThreshold,
    HumidityFanNoEffectDelay,
    HumidityWettingDuration,
    HumidityWettingDelay,
    HumidityWettingEffectiveThreshold,
    HumidityWettingNoEffectDelay,
    HumidityInStackSize,
    HumidityOutStackSize,
    HumidityOffset,
    HumidityHysteresis,

    CO2ControlEnabled,
    CO2AlarmThresholdHigh,
    CO2FanDuration,
    CO2FanDelay,
    CO2FanEffectiveThreshold,
    CO2FanNoEffectDelay,
    CO2InStackSize,
    CO2OutStackSize,
    CO2Offset,
    CO2Hysteresis,

    MqttPublishDelay};

class _mqttClient {
public:
  using _OnCallback   = std::function<void(char *topic, uint8_t *payload, uint length)>;                                                         // void MqttCallBack(char *topic, byte *payload, unsigned int length)
  using _OnCallbackEx = std::function<void(const char *topic, const char *value, const uint length, const dbParams dbParam, const bool IsWrited)>; // void MqttCallBack(char *topic, byte *payload, unsigned int length)

private:
  char _server[20]   = "";
  int  _port         = 0;
  char _user[20]     = "";
  char _password[20] = "";

  WiFiClient   _WiFiClient;
  PubSubClient _PubSubClient;

  _callback _on_publish_all   = nullptr;
  _callback _on_subscribe_all = nullptr;

  _OnCallbackEx _on_callback = nullptr;

  ulong _ConnectionCheckMillis = 0;
  ulong _PublishMillis         = 0;

  ulong _ConnectionCheckDelay = MQTT_CONNECTION_CHECK_DELAY;
  ulong _PublishDelay         = MQTT_PUBLISH_DELAY;

  void _Connect() {
    if (_PubSubClient.connected() && IsInit())
      return;

    _PubSubClient.setServer(_server, _port);

    debug.tprintln("Connecting to MQTT...");

    char hexID[24];
    uint8_t _mac[6];

    esp_read_mac(_mac, ESP_MAC_WIFI_STA);

    snprintf(hexID, sizeof(hexID), "gh_%02X%02X%02X", _mac[3], _mac[4], _mac[5]);

    if (_PubSubClient.connect(hexID, _user, _password)) {
      debug.tprintln("Connection established");

      SubscribeAll();
    } else {
      debug.tprint("Connection error, rc = ");
      debug.println(_PubSubClient.state());
    }
  }

  void _Connect(const char *server, const int port, const char *user, const char *password) {
    Init(server, port, user, password);

    if (_PubSubClient.connected())
      _PubSubClient.disconnect();

    _Connect();
  }

  void _PublishDB() {
    for (size_t i = 0; i < (sizeof(mqttParams) / sizeof(mqttParams[0])); i++) {
      dbParams param = mqttParams[i];

      char _getTopic[MQTT_TOPIC_SIZE];
      char _setTopic[MQTT_TOPIC_SIZE];

      getTopic(param, _getTopic, _setTopic);

      switch (db[param].type()) {
        case gdb::Type::Int:
        case gdb::Type::Uint:
          Publish(_getTopic, db[param].toInt());
          break;
        case gdb::Type::Float:
          Publish(_getTopic, db[param].toFloat());
          break;
      }
    }
  }

  void _SubscribeDB() {
    for (size_t i = 0; i < (sizeof(mqttParams) / sizeof(mqttParams[0])); i++) {
      dbParams param = mqttParams[i];

      char _getTopic[MQTT_TOPIC_SIZE];
      char _setTopic[MQTT_TOPIC_SIZE];

      getTopic(param, _getTopic, _setTopic);
      Subscribe(_setTopic);
    }
  }

  void _Callback(char *topic, uint8_t *payload, uint length) {
    if (length < 1 || length >= MQTT_PAYLOAD_SIZE)
      return;

    char _topic[MQTT_TOPIC_SIZE];
    cutTopic(_topic, sizeof(_topic), topic);

    char _payload[MQTT_PAYLOAD_SIZE];
    memcpy(_payload, payload, length);
    _payload[length] = '\0';

    dbParams _param = dbParams::Undefined;
    bool _writed = false;

    for (size_t i = 0; (i < (sizeof(mqttParams) / sizeof(mqttParams[0]))) && (_param == dbParams::Undefined); i++) {
      char _getTopic[MQTT_TOPIC_SIZE];
      char _setTopic[MQTT_TOPIC_SIZE];

      getTopic(mqttParams[i], _getTopic, _setTopic);

      if (strcmp(_topic, _setTopic) != 0)
        continue;

      _param = mqttParams[i];

      switch (db[_param].type()) {
        case gdb::Type::Int:
        case gdb::Type::Uint:
          db[_param] = atoi(_payload);
          _writed = true;
          break;
        case gdb::Type::Float:
          db[_param] = atof(_payload);
          _writed = true;
          break;
      }
    }

    if (_on_callback)
      _on_callback(_topic, _payload, length, _param, _writed);
  }

public:
  _mqttClient() : _PubSubClient(_WiFiClient) {
    _PubSubClient.setCallback([this](char *topic, uint8_t *payload, uint length) {
      this->_Callback(topic, payload, length);
    });
  }

  _mqttClient(const char *server, const int port, const char *user, const char *password) : _PubSubClient(_WiFiClient) {
    _mqttClient();

    Init(server, port, user, password);
  }

  void OnCallback(_OnCallbackEx cb) {
    if (cb)
      _on_callback = cb;
  }

  void Connect() {
    _Connect();
  }

  void Connect(const char *server, const int port, const char *user, const char *password) {
    _Connect(server, port, user, password);
  }

  void Reconnect() {
    if (_PubSubClient.connected())
      _PubSubClient.disconnect();

    _Connect();
  }

  void Disconnect() {
    if (_PubSubClient.connected())
      _PubSubClient.disconnect();
  }

  void Init(const char *server, const int port, const char *user, const char *password) {
    setServer(server);
    setPort(port);
    setUser(user);
    setPassword(password);
  }

  bool IsInit() {
    return ((strlen(_server) > 0) &&
            (_port > 0) &&
            (strlen(_user) > 0) &&
            (strlen(_password) > 0));
  }

  bool Connected() {
    return _PubSubClient.connected();
  }

  void makeTopic(char *buffer, const size_t size, const char *topic) {
    snprintf(buffer, size, "%s/%s", _user, topic);
  }

  void getTopic(dbParams param, char *getTopic, char *setTopic) {
    if (!getTopic && !setTopic)
      return;

    char _param[50];

    strncpy(_param, dbParamsName[param], sizeof(_param) - 1);
    _param[sizeof(_param) - 1] = '\0';

    const char *category = "";
    if (strncmp(_param, "Temperature", 11) == 0) {
      category = "Temperature";
    } else if (strncmp(_param, "Humidity", 8) == 0) {
      category = "Humidity";
    } else if (strncmp(_param, "CO2", 3) == 0) {
      category = "CO2";
    }

    const char *paramName = _param;
    if (category[0] != '\0') {
      paramName += strlen(category);

      if (*paramName == '_')
        paramName++;
    }

    if (category[0] != '\0') {
      if (getTopic)
        snprintf(getTopic, MQTT_TOPIC_SIZE, "%s/%s", category, paramName);
      if (setTopic)
        snprintf(setTopic, MQTT_TOPIC_SIZE, "%s/set%s", category, paramName);
    } else {
      if (getTopic)
        snprintf(getTopic, MQTT_TOPIC_SIZE, "%s", _param);
      if (setTopic)
        snprintf(setTopic, MQTT_TOPIC_SIZE, "set%s", _param);
    }
  }

  void cutTopic(char *buffer, const size_t size, const char *topic) {
    if (!buffer || !topic || size == 0)
      return;

    char _cut[sizeof(_user) + 2];

    snprintf(_cut, sizeof(_cut), "%s/", _user);

    const char *pos = (strncmp(topic, _cut, strlen(_cut)) == 0) ? topic + strlen(_cut) : topic;

    snprintf(buffer, size, "%s", pos);
  }

  void Publish(const char *topic, const char *value) {
    if (!_PubSubClient.connected())
      return;

    char _topic[MQTT_TOPIC_SIZE];

    makeTopic(_topic, sizeof(_topic), topic);

    // debug.tprintf("Publish(\"%s\", \"%s\")\n", _topic, value);

    _PubSubClient.publish(_topic, value);
  }

  void Publish(const char *topic, const double value) {
    char _value[10] = "";
    snprintf(_value, sizeof(_value), "%.1f", value);

    Publish(topic, _value);
  }

  void Publish(const char *topic, const float value) {
    char _value[10] = "";
    snprintf(_value, sizeof(_value), "%.1f", value);

    Publish(topic, _value);
  }

  void Publish(const char *topic, const int value) {
    char _value[10] = "";
    snprintf(_value, sizeof(_value), "%d", value);

    Publish(topic, _value);
  }

  void Publish(const char *topic, const ulong value) {
    char _value[10] = "";
    snprintf(_value, sizeof(_value), "%lu", value);

    Publish(topic, _value);
  }

  void Publish(const char *topic, const size_t value) {
    char _value[10] = "";
    snprintf(_value, sizeof(_value), "%d", value);

    Publish(topic, _value);
  }

  void Publish(const char *topic, const bool value) {
    char _value[10] = "";
    snprintf(_value, sizeof(_value), "%d", value ? 1 : 0);

    Publish(topic, _value);
  }

  void OnPublishAll(_callback cb) {
    if (cb)
      _on_publish_all = cb;
  }

  void PublishAll() {
    if (!_PubSubClient.connected())
      return;

    _PublishDB();

    if (_on_publish_all)
      _on_publish_all();
  }

  void setPublishDelay(ulong PublishDelay) {
    if (PublishDelay > 0)
      _PublishDelay = PublishDelay;
  }

  ulong PublishDelay() {
    return _PublishDelay;
  }

  void Subscribe(const char *topic) {
    if (!_PubSubClient.connected())
      return;

    char _topic[MQTT_TOPIC_SIZE] = "";
    snprintf(_topic, sizeof(_topic), "%s/%s", _user, topic);

    // debug.tprintf("Subscribe(\"%s\")\n", _topic);

    _PubSubClient.subscribe(_topic);
  }

  void OnSubscribeAll(_callback cb) {
    if (cb)
      _on_subscribe_all = cb;
  }

  void SubscribeAll() {
    if (!_PubSubClient.connected())
      return;

    _SubscribeDB();

    if (_on_subscribe_all)
      _on_subscribe_all();
  }

  void setServer(const char *server) {
    if (strlen(server) > 0)
      strncpy(_server, server, sizeof(_server) - 1);
  }

  void setPort(const int port) {
    _port = port;
  }

  void setUser(const char *user) {
    if (strlen(user) > 0)
      strncpy(_user, user, sizeof(_user) - 1);
  }

  void setPassword(const char *password) {
    if (strlen(password) > 0)
      strncpy(_password, password, sizeof(_password) - 1);
  }

  const char *Server() const {
    return _server;
  }

  const int Port() const {
    return _port;
  }

  const char *User() const {
    return _user;
  }

  const char *Password() const {
    return _password;
  }

  void Tick() {
    ulong _millis = millis();

    if (WiFi.isConnected() && !_PubSubClient.connected() && ((_ConnectionCheckMillis == 0) || (_millis - _ConnectionCheckMillis) > _ConnectionCheckDelay)) {
      _ConnectionCheckMillis = _millis;

      _Connect();
    }

    if (_PubSubClient.connected() && ((_PublishMillis == 0) || (_millis - _PublishMillis) > _PublishDelay)) {
      _PublishMillis = _millis;

      PublishAll();
    }

    _PubSubClient.loop();
  }
};
