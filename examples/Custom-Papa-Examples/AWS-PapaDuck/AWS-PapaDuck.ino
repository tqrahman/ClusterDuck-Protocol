/**
 * @file PapaDuck.ino
 * @author Timo Wielink
 * @brief Uses built-in PapaDuck from the SDK to create a WiFi enabled Papa Duck
 *
 * This example will configure and run a Papa Duck that connects to AWS cloud
 * and forwards all messages (except pings) to the cloud. When disconnected
 * it will add received packets to a queue. When it reconnects to MQTT it will
 * try to publish all messages in the queue. You can change the size of the queue
 * by changing `QUEUE_SIZE_MAX`.
 *
 * @date 05-07-2025
 */
#include <CDP.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <queue>
#include "secrets.h"

#define CA_CERT
#ifdef CA_CERT
#endif

// --- WiFi Configuration ---
#define WIFI_SSID "Sabic_2U"                    // Your WiFi SSID (Needs to be 2.4 Ghz network)
#define WIFI_PASSWORD "maca2635"                // Your WiFi Password

// --- Command Definitions ---
#define CMD_STATE_WIFI "/wifi/"
#define CMD_STATE_HEALTH "/health/"
#define CMD_STATE_CHANNEL "/channel/"

// --- Telemetry - XPowersLib for AXP2101 ---
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
XPowersPMU PMU;

// --- Global Objects ---
PapaDuck duck(THINGNAME);
int QUEUE_SIZE_MAX = 5;
auto timer = timer_create_default();
bool retry = true;
const char commandTopic[] = "iot-2/cmd/+/fmt/+";
int messagesSeen = 0;
int publishFailed = 0;
int disconnectTime = 0;
bool disconnect = false;
int INTERVAL_HEALTH = 1000 * 60 * 10 + 3711;  // ~10 minutes

// --- WiFi downtime tracking ---
bool wifiPreviouslyConnected = true;    // assume connected after setup()
unsigned long wifiOfflineStart = 0;     // millis() when WiFi dropped; 0 = online
unsigned long pendingWifiDowntime = 0;  // seconds offline, published on next MQTT connect
int wifiOfflineMissed = 0;              // messages received but not published during outage

// --- Function Declarations ---
std::queue<std::vector<byte>> packetQueue;
std::string toTopicString(byte topic);
int quackJson(CdpPacket packet);
void handleDuckData(CdpPacket receivedPacket);
void dmsCmdReceived(char* topic, byte* payload, unsigned int payloadLength);
void mqttConnect();
void subscribeTo(const char* topic);
bool enableRetry(void*);
void publishQueue();
bool runHealthCheck(void*);
JsonDocument getBatteryData();
JsonDocument createJsonDoc(JsonDocument& payload);
int publishJson(byte eventTopic, JsonDocument& doc);
void publishWifiDowntime(unsigned long seconds);

// --- WIFI Setup Function ---
WiFiClientSecure wifiClient;
PubSubClient client(AWS_IOT_ENDPOINT, 8883, dmsCmdReceived, wifiClient);

/**
 * @brief Converts a CDP topic byte value into a string.
 */
std::string toTopicString(byte topic) {
  switch (topic) {
    case topics::status:  return "status";
    case topics::cpm:     return "portal";
    case topics::sensor:  return "sensor";
    case topics::alert:   return "alert";
    case topics::gps:     return "gps";
    case topics::health:  return "health";
    case topics::bmp180:  return "bmp180";
    case topics::pir:     return "pir";
    case topics::dht11:   return "dht";
    case topics::bmp280:  return "bmp280";
    case topics::mq7:     return "mq7";
    case topics::gp2y:    return "gp2y";
    case reservedTopic::ack: return "ack";
    default:              return "status";
  }
}

/**
 * @brief Converts a received CDP packet into JSON format and publishes it to an MQTT topic.
 *
 * @param packet A CDP packet received from the mesh network
 * @return int Returns 0 if published successfully; -1 on failure
 */
int quackJson(CdpPacket packet) {

  JsonDocument doc;

  std::string payload(packet.data.begin(), packet.data.end());
  std::string sduid(packet.sduid.begin(), packet.sduid.end());
  std::string dduid(packet.dduid.begin(), packet.dduid.end());
  std::string muid(packet.muid.begin(), packet.muid.end());

  Serial.println("[PAPA] Packet Received:");
  Serial.printf("[PAPA] sduid:   %s\n", sduid.c_str());
  Serial.printf("[PAPA] dduid:   %s\n", dduid.c_str());
  Serial.printf("[PAPA] muid:    %s\n", muid.c_str());
  Serial.printf("[PAPA] data:    %s\n", payload.c_str());
  Serial.printf("[PAPA] hops:    %s\n", std::to_string(packet.hopCount).c_str());
  Serial.printf("[PAPA] duck:    %s\n", std::to_string(packet.duckType).c_str());

  doc["DeviceID"] = sduid;
  doc["MessageID"] = muid;

  JsonDocument inner;
  DeserializationError err = deserializeJson(inner, payload);
  if (!err) {
    doc["Payload"] = inner.as<JsonVariant>();
  } else {
    doc["Payload"] = payload;
  }

  doc["hops"].set(packet.hopCount);
  doc["duckType"].set(packet.duckType);

  return publishJson(packet.topic, doc);
}

/**
 * @brief Callback function to handle incoming data from the Papa Duck.
 */
void handleDuckData(CdpPacket receivedPacket) {
  Serial.printf("[PAPA] got packet\n");

  if (receivedPacket.topic != reservedTopic::ack &&
      receivedPacket.topic != reservedTopic::rrep &&
      receivedPacket.topic != reservedTopic::rreq) {
    if (quackJson(receivedPacket) == -1) {
      if (!duck.isWifiConnected()) {
        wifiOfflineMissed++;
      }
      if (packetQueue.size() > QUEUE_SIZE_MAX) {
        packetQueue.pop();
      }
      packetQueue.push(receivedPacket.data);
      Serial.print("[PAPA] New size of queue: ");
      Serial.println(packetQueue.size());
    }
  }

  subscribeTo(commandTopic);
}

/**
 * @brief Initializes the PapaDuck node.
 */
void setup() {
  duck.setupWithDefaults();
  duck.joinWifiNetwork(WIFI_SSID, WIFI_PASSWORD);

  duck.onReceiveDuckData(handleDuckData);

  client.setBufferSize(1024);

  #ifdef CA_CERT
  Serial.println("[PAPA] Using root CA cert");
  wifiClient.setCACert(AWS_CERT_CA);
  wifiClient.setCertificate(AWS_CERT_CRT);
  wifiClient.setPrivateKey(AWS_CERT_PRIVATE);
  #else
  Serial.println("[PAPA] Using insecure TLS");
  wifiClient.setInsecure();
  #endif

  // Setup AXP2101
  Wire.begin(21, 22);
  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, 21, 22)) {
    Serial.println("[PAPA] AXP2101 Begin FAIL");
  } else {
    Serial.println("[PAPA] AXP2101 Begin PASS");
    PMU.enableBattDetection();
    PMU.enableVbusVoltageMeasure();
    PMU.enableBattVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.enableTemperatureMeasure();
  }

  timer.every(INTERVAL_HEALTH, runHealthCheck);

  Serial.println("[PAPA] Setup OK!");
}

void loop() {

  // --- WiFi downtime tracking ---
  bool wifiNow = duck.isWifiConnected();
  if (wifiPreviouslyConnected && !wifiNow) {
    // WiFi just dropped
    wifiOfflineStart = millis();
    Serial.println("[PAPA] WiFi lost, tracking downtime...");
  } else if (!wifiPreviouslyConnected && wifiNow) {
    // WiFi just came back
    unsigned long durationSec = (millis() - wifiOfflineStart) / 1000;
    Serial.printf("[PAPA] WiFi restored after %lu seconds\n", durationSec);
    if (durationSec >= 60) {
      pendingWifiDowntime = durationSec;  // published once MQTT reconnects
    }
    wifiOfflineStart = 0;
  }
  wifiPreviouslyConnected = wifiNow;

  if (!wifiNow && retry) {
    Serial.printf("[PAPA] WiFi disconnected, reconnecting to: %s\n", WIFI_SSID);
    duck.joinWifiNetwork(WIFI_SSID, WIFI_PASSWORD);
    retry = false;
    timer.in(5000, enableRetry);
  }

  if (!client.loop()) {
    if (wifiNow) {
      mqttConnect();
    }
  }

  duck.run();
  timer.tick();
}

void dmsCmdReceived(char* topic, byte* payload, unsigned int payloadLength) {
  Serial.print("[PAPA] DMS Command Received: invoked for topic: "); Serial.println(topic);

  if (std::string(topic).find(CMD_STATE_WIFI) > 0) {
    Serial.println("[PAPA] Start WiFi Command");
    byte sCmd = 1;
    std::vector<byte> sValue = {payload[0]};

    if (payloadLength > 3) {
      std::string destination = "";
      for (unsigned int i = 0; i < payloadLength; i++) {
        destination += (char)payload[i];
      }
      std::array<byte, 8> dDevId;
      std::copy(destination.begin(), destination.end(), dDevId.begin());
      // duck.sendCommand(sCmd, sValue, dDevId);
    }
  } else if (std::string(topic).find(CMD_STATE_HEALTH) > 0) {
    byte sCmd = 0;
    std::vector<byte> sValue = {payload[0]};
    if (payloadLength >= 8) {
      std::string destination = "";
      for (unsigned int i = 1; i < payloadLength; i++) {
        destination += (char)payload[i];
      }
      std::array<byte, 8> dDevId;
      std::copy(destination.begin(), destination.end(), dDevId.begin());
      // duck.sendCommand(sCmd, sValue, dDevId);
    } else {
      Serial.println("[PAPA] Payload size too small");
    }
  } else {
    Serial.print("[PAPA] dmsCmdReceived: unexpected topic: "); Serial.println(topic);
  }
}

void mqttConnect() {
  if (!!!client.connected()) {
    Serial.print("[PAPA] Reconnecting MQTT client to "); Serial.println(AWS_IOT_ENDPOINT);
    if (!!!client.connect(THINGNAME) && retry) {
      Serial.print("[PAPA] Connection failed, retry in 5 seconds");
      retry = false;
      disconnectTime = millis();
      disconnect = true;
      timer.in(5000, enableRetry);
    }
    Serial.println();
  } else {
    if (pendingWifiDowntime > 0) {
      publishWifiDowntime(pendingWifiDowntime);
      pendingWifiDowntime = 0;
    }
    if (packetQueue.size() > 0) {
      publishQueue();
    }
    disconnect = false;
    int timeDisconnected = (millis() - disconnectTime) / 1000;
    Serial.printf("[PAPA] Reconnected after %d seconds\n", timeDisconnected);
    subscribeTo(commandTopic);
  }
}

void subscribeTo(const char* topic) {
  Serial.print("[PAPA] subscribe to "); Serial.print(topic);
  if (client.subscribe(topic)) {
    Serial.println(" OK");
  } else {
    Serial.println(" FAILED");
  }
}

bool enableRetry(void*) {
  retry = true;
  return retry;
}

void publishQueue() {
  while (!packetQueue.empty()) {
    CdpPacket packet = CdpPacket(packetQueue.front());
    if (quackJson(packet) == 0) {
      packetQueue.pop();
      Serial.print("[PAPA] Queue size: ");
      Serial.println(packetQueue.size());
    } else {
      return;
    }
  }
}

/**
 * @brief Runs the Papa Duck health check and publishes telemetry to MQTT.
 */
bool runHealthCheck(void*) {
  Serial.println("[PAPA] Running PAPA health check");

  JsonDocument healthData = getBatteryData();

  // Publish fail rate
  float pfr = messagesSeen > 0 ? (float)publishFailed / (float)messagesSeen : 0.0f;
  healthData["PFR"] = pfr;

  // Free memory percent
  size_t freeB  = duckesp::freeHeapMemory();
  size_t totalB = duckesp::getTotalHeap();
  float  pct    = totalB ? (freeB * 100.0f) / static_cast<float>(totalB) : 0.0f;
  healthData["PFM"] = pct;

  JsonDocument doc = createJsonDoc(healthData);
  publishJson(topics::health, doc);

  return true;
}

/**
 * @brief Publishes a health packet reporting how long WiFi was offline.
 *        Only called when the outage was >= 60 seconds.
 *
 * @param seconds Duration of the WiFi outage in seconds
 */
void publishWifiDowntime(unsigned long seconds) {
  Serial.printf("[PAPA] Publishing WiFi downtime: %lu seconds, missed: %d\n", seconds, wifiOfflineMissed);
  JsonDocument payload;
  payload["wifiDownSec"] = seconds;
  payload["wifiMissed"]  = wifiOfflineMissed;
  wifiOfflineMissed = 0;
  JsonDocument doc = createJsonDoc(payload);
  publishJson(topics::health, doc);
}

/**
 * @brief Reads battery telemetry from the AXP2101 PMIC.
 */
JsonDocument getBatteryData() {
  JsonDocument payload;

  if (!PMU.isBatteryConnect()) {
    Serial.println("[PAPA] No battery connected");
    payload["error"] = "No battery";
    return payload;
  }

  payload["V"]    = PMU.getBattVoltage() / 1000.0f;
  payload["PCT"]  = PMU.getBatteryPercent();
  payload["CHRG"] = PMU.getVbusVoltage() > 4000;
  payload["BT"]   = PMU.getTemperature();

  return payload;
}

/**
 * @brief Wraps a health payload into the standard CDP envelope used by the cloud.
 */
JsonDocument createJsonDoc(JsonDocument& payload) {
  JsonDocument doc;
  doc["DeviceID"]  = THINGNAME;
  doc["MessageID"] = "PAPA";
  doc["Payload"]   = payload;
  doc["hops"]      = 0;
  doc["duckType"]  = 1;
  return doc;
}

/**
 * @brief Serializes a JSON document and publishes it to the MQTT topic for the given CDP topic.
 *
 * @param eventTopic CDP topic byte (used to build the MQTT topic path)
 * @param doc        JSON document to publish
 * @return 0 on success, -1 on failure
 */
int publishJson(byte eventTopic, JsonDocument& doc) {
  Serial.println("[PAPA] Publishing packet...");

  messagesSeen++;

  std::string jsonstat;
  serializeJson(doc, jsonstat);
  serializeJsonPretty(doc, Serial);

  std::string cdpTopic = toTopicString(eventTopic);
  std::string topic = "owl/device/" + std::string(THINGNAME) + "/evt/" + cdpTopic;

  if (client.publish(topic.c_str(), jsonstat.c_str())) {
    Serial.println("\n[PAPA] Publish ok");
    Serial.printf("[PAPA] Messages seen: %d, failed: %d\n", messagesSeen, publishFailed);
    return 0;
  } else {
    Serial.println("[PAPA] Publish failed");
    publishFailed++;
    Serial.printf("[PAPA] Messages seen: %d, failed: %d\n", messagesSeen, publishFailed);
    return -1;
  }
}
