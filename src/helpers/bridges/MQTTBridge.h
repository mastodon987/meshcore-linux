#pragma once

#ifdef WITH_MQTT_BRIDGE

#include "helpers/bridges/BridgeBase.h"
#include "helpers/LinuxMQTTClient.h"

/**
 * @brief Bridge implementation that carries MeshCore packets over MQTT,
 *        enabling two (or more) geographically separate mesh networks to
 *        behave as a single logical mesh.
 */

#ifndef WITH_MQTT_BRIDGE_PORT
  #define WITH_MQTT_BRIDGE_PORT 1883
#endif

#ifndef WITH_MQTT_BRIDGE_TOPIC
  #define WITH_MQTT_BRIDGE_TOPIC "meshcore/bridge"
#endif

#ifndef WITH_MQTT_BRIDGE_USER
  #define WITH_MQTT_BRIDGE_USER ""
#endif

#ifndef WITH_MQTT_BRIDGE_PASS
  #define WITH_MQTT_BRIDGE_PASS ""
#endif

class MQTTBridge : public BridgeBase {
public:
  MQTTBridge(NodePrefs* prefs, mesh::PacketManager* mgr, mesh::RTCClock* rtc);

  void begin() override;
  void end() override;
  void startMQTT();
  void stopMQTT();
  void loop() override;

  void sendPacket(mesh::Packet* packet) override;
  void onPacketReceived(mesh::Packet* packet) override;

  void getStatusStr(char* buf, int len);

  struct Stats {
    uint32_t tx_packets;   // packets successfully published to MQTT
    uint32_t rx_packets;   // packets received from MQTT and injected into mesh
    uint32_t tx_filtered;  // packets dropped by shouldBridgePacket() or ban list
    uint32_t rx_banned;    // packets dropped because source is banned
    uint32_t reconnects;   // successful MQTT (re)connections
    uint32_t tx_blocked_hashrules; // tx blocked by hash rules
    uint32_t rx_blocked_hashrules; // rx blocked by hash rules or rate-limited
  };
  const Stats& getStats() const { return _stats; }

  bool banNode(const uint8_t prefix[4]);
  bool unbanNode(const uint8_t prefix[4]);
  void getBanListStr(char* buf, int len) const;
  void setAppCallbacks(CommonCLICallbacks* cb) { _app_cb = cb; }

  static constexpr int MQTT_BAN_LIST_SIZE = 16;
  static constexpr uint8_t BAN_CMD_MAGIC[3] = {0xBA, 0x4E, 0xED};
  static constexpr uint8_t BAN_CMD_LEN = 7; // 3 magic + 4 pubkey prefix bytes

private:
  static MQTTBridge* _instance;
  bool _mqtt_running = false;
  Stats _stats = {};

  uint8_t _ban_prefixes[MQTT_BAN_LIST_SIZE][4];
  uint8_t _ban_count = 0;

  CommonCLICallbacks* _app_cb = nullptr;
  bool _deferred_self_ban = false;

  void sendBanCommand(const uint8_t prefix[4]);
  void executeSelfBan();

  LinuxMQTTClient _mqttClient;
  unsigned long _lastReconnectAttempt;

  static constexpr size_t MAX_MQTT_PAYLOAD = MAX_TRANS_UNIT + 1;
  uint8_t _tx_buffer[MAX_MQTT_PAYLOAD];

  bool connectMQTT();
  static void mqttCallback(char* topic, uint8_t* payload, unsigned int length);
  void handleMQTTMessage(const uint8_t* payload, unsigned int length);
};

#endif // WITH_MQTT_BRIDGE
