#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#elif defined(ARDULINUX_PLATFORM)
  #include <ArduLinuxFS.h>
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_MQTT_BRIDGE
#include "helpers/bridges/MQTTBridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_SNMP
#include "helpers/snmp/SNMPAgent.h"
#include "helpers/snmp/SNMPOids.h"
#endif

#ifdef ARDULINUX_PLATFORM
#ifdef WITH_TCP_COMPANION
#include "helpers/linux/LinuxTCPCompanionInterface.h"
#endif
#ifdef WITH_MC_CONSOLE
#include "helpers/linux/LinuxConsoleServer.h"
extern const char *meshcoredConsoleSocketPath;
#endif
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/RegionMap.h>
#include "RateLimiter.h"

#ifdef WITH_BRIDGE
extern AbstractBridge* bridge;
#endif

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
};

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "6 Jun 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.16.0"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

#ifdef WITH_SNMP
class MyMesh : public mesh::Mesh, public CommonCLICallbacks, public SNMPDataSource, public SNMPControlSource {
#else
class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
#endif
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  int8_t  reply_path_len;
  uint8_t reply_path_hash_size;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  RateLimiter discover_limiter, anon_limiter;
  uint32_t pending_discover_tag;
  unsigned long pending_discover_until;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#elif defined(WITH_MQTT_BRIDGE)
  MQTTBridge bridge;
#endif
#ifdef WITH_SNMP
  SNMPAgent snmp_agent;
#endif
#if defined(ARDULINUX_PLATFORM) && defined(WITH_TCP_COMPANION)
  LinuxTCPCompanionInterface tcp_companion;
#endif
#if defined(ARDULINUX_PLATFORM) && defined(WITH_MC_CONSOLE)
  LinuxConsoleServer mc_console;
#endif

  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  File openAppend(const char* fname);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  bool filterRecvFloodPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void sendNodeDiscoverReq();
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
#ifdef WITH_SNMP
  const char* getNodeName() override { return _prefs.node_name; }
#else
  const char* getNodeName() { return _prefs.node_name; }
#endif
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  void loop();

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
#if defined(WITH_MQTT_BRIDGE)
    if (enable) bridge.startMQTT();
    else        bridge.stopMQTT();
#else
    if (enable == bridge.isRunning()) return;
    if (enable)
    {
      bridge.begin();
    }
    else 
    {
      bridge.end();
    }
#endif
  }

  void restartBridge() override {
#if defined(WITH_MQTT_BRIDGE)
    bridge.stopMQTT();
    bridge.startMQTT();
#else
    if (!bridge.isRunning()) return;
    bridge.end();
    bridge.begin();
#endif
  }

#if defined(WITH_MQTT_BRIDGE)
  void getBridgeStatus(char* buf) override {
    bridge.getStatusStr(buf, 159);
  }
  const MQTTBridge::Stats& getMQTTStats() const {
    return bridge.getStats();
  }
#endif
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

#ifdef WITH_SNMP
  // SNMPDataSource (getNodeName() is declared earlier in this class, made
  // an override of SNMPDataSource when WITH_SNMP is defined)
  const char* getFirmwareRole() override { return FIRMWARE_ROLE; }
  const char* getFirmwareVersion() override { return FIRMWARE_VERSION; }
  void getPublicKeyHex(char* buf, size_t len) override;
  uint32_t getUptimeMillis() override { return (uint32_t)uptime_millis; }
  uint16_t getBattMilliVolts() override { return board.getBattMilliVolts(); }

  // Dispatcher::getTotalAirTime()/getReceiveAirTime()/getRemainingTxBudget()
  // return 'unsigned long' (64-bit on this Linux target); SNMP Counter32/
  // Gauge32 are inherently 32-bit wire types, so we narrow explicitly here
  // (matches the existing (int16_t)/%u casts in StatsFormatHelper.h).
  uint32_t getTotalAirTime() override { return (uint32_t)mesh::Dispatcher::getTotalAirTime(); }
  uint32_t getReceiveAirTime() override { return (uint32_t)mesh::Dispatcher::getReceiveAirTime(); }
  uint32_t getRemainingTxBudget() override { return (uint32_t)mesh::Dispatcher::getRemainingTxBudget(); }
  int32_t getNoiseFloor() override { return _radio->getNoiseFloor(); }
  int32_t getLastRSSI() override { return (int32_t)radio_driver.getLastRSSI(); }
  float getLastSNR() override { return radio_driver.getLastSNR(); }
  float getDutyCyclePct() override {
    float duty_cycle = 1.0f / (1.0f + getAirtimeBudgetFactor());
    return duty_cycle * 100.0f;
  }

  uint32_t getNumSentFlood() override { return mesh::Dispatcher::getNumSentFlood(); }
  uint32_t getNumSentDirect() override { return mesh::Dispatcher::getNumSentDirect(); }
  uint32_t getNumRecvFlood() override { return mesh::Dispatcher::getNumRecvFlood(); }
  uint32_t getNumRecvDirect() override { return mesh::Dispatcher::getNumRecvDirect(); }
  uint32_t getRadioPacketsRecv() override { return radio_driver.getPacketsRecv(); }
  uint32_t getRadioPacketsSent() override { return radio_driver.getPacketsSent(); }
  uint32_t getRadioPacketsRecvErrors() override { return radio_driver.getPacketsRecvErrors(); }
  uint32_t getOutboundQueueLen() override { return _mgr->getOutboundTotal(); }
  uint32_t getErrFlags() override { return _err_flags; }

#if defined(WITH_MQTT_BRIDGE)
  uint32_t getBridgeTxPackets() override { return bridge.getStats().tx_packets; }
  uint32_t getBridgeRxPackets() override { return bridge.getStats().rx_packets; }
  uint32_t getBridgeReconnects() override { return bridge.getStats().reconnects; }
#endif
#endif

#ifdef WITH_SNMP
  // ---- SNMPControlSource ----
  bool getControlField(SNMPControlField f, SNMPControlValue* out) override;
  SNMPSetResult setControlField(SNMPControlField f, const SNMPControlValue& v, char* err, size_t err_len) override;
  int getAclCount() override { return acl.getNumClients(); }
  bool getAclPubKeyHex(int idx, char* buf, size_t len) override;
  bool getAclPermissions(int idx, uint8_t* out) override;
  bool getAclLastActivityMillis(int idx, uint32_t* out) override;
  SNMPSetResult setAclPermissions(int idx, uint8_t perms, char* err, size_t err_len) override;
  int getRegionCount() override { return region_map.getCount(); }
  bool getRegionId(int idx, uint16_t* out) override;
  bool getRegionParentId(int idx, uint16_t* out) override;
  bool getRegionName(int idx, char* buf, size_t len) override;
  bool getRegionFloodDenied(int idx, uint8_t* out) override;
  bool getRegionIsHome(int idx, uint8_t* out) override;
  bool getRegionIsDefault(int idx, uint8_t* out) override;
  SNMPSetResult setRegionName(int idx, const char* name, char* err, size_t err_len) override;
  SNMPSetResult setRegionFloodDenied(int idx, uint8_t deny, char* err, size_t err_len) override;
  SNMPSetResult setRegionIsHome(int idx, uint8_t is_home, char* err, size_t err_len) override;
  SNMPSetResult setRegionIsDefault(int idx, uint8_t is_default, char* err, size_t err_len) override;
#endif

#if defined(USE_SX1262) || defined(USE_SX1268)
  void setRxBoostedGain(bool enable) override;
#endif
};
