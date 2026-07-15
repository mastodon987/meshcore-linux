#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Read-only data provider consulted by SNMPAgent to answer GET /
 *        GETNEXT / GETBULK requests.
 *
 * SNMPAgent itself knows nothing about mesh::Dispatcher, mesh::Mesh, or any
 * particular radio driver -- it only deals with OIDs and BER encoding. Each
 * firmware role (repeater / room_server / room_server_mqtt_only) implements
 * this interface (typically directly on its MyMesh class, since MyMesh
 * already extends mesh::Mesh -> mesh::Dispatcher and so already has all of
 * these values a method call away) and hands a pointer to it to SNMPAgent.
 *
 * All getters must be cheap, non-blocking, and safe to call from the main
 * loop() at any time (no heap allocation, no I/O).
 */
class SNMPDataSource {
public:
  virtual ~SNMPDataSource() = default;

  // ---- sysInfo ----
  virtual const char* getNodeName() = 0;
  virtual const char* getFirmwareRole() = 0;
  virtual const char* getFirmwareVersion() = 0;

  /** Writes the node's public key as upper-case hex into 'buf' (len incl. null terminator). */
  virtual void getPublicKeyHex(char* buf, size_t len) = 0;

  /** Node uptime, in milliseconds, since boot. */
  virtual uint32_t getUptimeMillis() = 0;

  /** Battery voltage in millivolts (0 if not available on this board). */
  virtual uint16_t getBattMilliVolts() = 0;

  // ---- radio / airtime stats (src/Dispatcher.h) ----
  virtual uint32_t getTotalAirTime() = 0;       // Dispatcher::getTotalAirTime()
  virtual uint32_t getReceiveAirTime() = 0;      // Dispatcher::getReceiveAirTime()
  virtual uint32_t getRemainingTxBudget() = 0;   // Dispatcher::getRemainingTxBudget()

  virtual int32_t getNoiseFloor() = 0;
  virtual int32_t getLastRSSI() = 0;
  /** SNR in dB, unscaled (e.g. 4.25). SNMPAgent formats this as a decimal-string OCTET STRING on the wire -- no integer scale factor. */
  virtual float getLastSNR() = 0;
  /** Configured duty-cycle allowance, as a percent 0-100, unscaled (e.g. 12.5). This is the firmware's configured ceiling, not live usage -- see SNMPAgent's airtime-utilization sampling (computed from getTotalAirTime() deltas, not from this class) for actual real-time usage. */
  virtual float getDutyCyclePct() = 0;

  // ---- packet counters (src/Dispatcher.h + radio driver) ----
  virtual uint32_t getNumSentFlood() = 0;
  virtual uint32_t getNumSentDirect() = 0;
  virtual uint32_t getNumRecvFlood() = 0;
  virtual uint32_t getNumRecvDirect() = 0;
  virtual uint32_t getRadioPacketsRecv() = 0;
  virtual uint32_t getRadioPacketsSent() = 0;
  virtual uint32_t getRadioPacketsRecvErrors() = 0;
  virtual uint32_t getOutboundQueueLen() = 0;
  virtual uint32_t getErrFlags() = 0;

  // ---- optional bridge stats (0 if no bridge compiled in) ----
  virtual uint32_t getBridgeTxPackets() { return 0; }
  virtual uint32_t getBridgeRxPackets() { return 0; }
  virtual uint32_t getBridgeReconnects() { return 0; }
};

/**
 * @brief A single control-tree field's identity: (subgroup, leaf) within
 * SNMP_GRP_CONTROL, using the constants from SNMPOids.h. SNMPAgent passes
 * one of these to SNMPControlSource::getControlField()/setControlField()
 * for every scalar under base.5.*, so adding a new controllable setting
 * only requires: (1) a new leaf #define in SNMPOids.h, (2) one new case in
 * the switch inside MyMesh's getControlField()/setControlField(), (3) one
 * new line in docs/SNMP_MIB.txt. SNMPAgent's BER/table plumbing needs no
 * changes.
 */
struct SNMPControlField {
  uint16_t subgroup; // SNMP_CTRL_RADIO_HW / SNMP_CTRL_ROUTING / SNMP_CTRL_SYSTEM / SNMP_CTRL_BRIDGE / SNMP_CTRL_ACTIONS
  uint16_t leaf;
};

/**
 * @brief Generic typed value used for both directions of control-tree
 * access. 'kind' mirrors ber::AnyValue::Kind but is redeclared here so this
 * header has no BER.h dependency (SNMPControlSource is meant to be
 * implementable without knowing anything about wire encoding).
 */
struct SNMPControlValue {
  enum class Kind { Integer, OctetString } kind = Kind::Integer;
  int64_t int_value = 0;
  char str_value[160] = {0};

  static SNMPControlValue ofInt(int64_t v) { SNMPControlValue r; r.kind = Kind::Integer; r.int_value = v; return r; }
  static SNMPControlValue ofStr(const char* s) {
    SNMPControlValue r; r.kind = Kind::OctetString;
    size_t i = 0;
    while (s[i] && i < sizeof(r.str_value) - 1) { r.str_value[i] = s[i]; i++; }
    r.str_value[i] = 0;
    return r;
  }
};

/**
 * @brief Result of a control-tree SET attempt, used to choose the right
 * SNMP error-status (see ber:: ERR_* in BER.h) without SNMPControlSource
 * needing to depend on BER.h itself.
 */
enum class SNMPSetResult {
  Ok,
  NotWritable,     // -> ber::ERR_NOT_WRITABLE   (e.g. read-only table column)
  WrongType,       // -> ber::ERR_WRONG_TYPE     (e.g. sent OCTET STRING for an INTEGER field)
  WrongValue,      // -> ber::ERR_WRONG_VALUE    (out of range / fails firmware validation)
  GenErr,          // -> ber::ERR_GEN_ERR        (e.g. table index doesn't exist, region table full)
  NoSuchInstance,  // -> noSuchInstance exception varbind (table row doesn't exist)
};

/**
 * @brief Read/write control surface over the same settings exposed by the
 * `get`/`set` CLI commands (see docs/cli_commands.md) -- implemented by each
 * role's MyMesh, which already owns NodePrefs, ClientACL, and RegionMap.
 *
 * Design choice: rather than re-implementing each setting's validation and
 * persistence in SNMP-specific code, every setControlField()/ACL/region
 * SET implementation is expected to internally synthesize the equivalent
 * CLI command string and run it through the SAME CommonCLI::handleCommand()
 * (or MyMesh::handleCommand() for setperm) path used by every other
 * transport (serial, BLE companion, etc.) -- so SNMP SET behaves identically
 * to typing the command, including range checks, rounding, and side
 * effects like triggering a bridge restart or advert-timer update. See the
 * .cpp implementations in examples / MyMesh.cpp for the exact command
 * strings used per field.
 *
 * Security note: there is no SNMPv3, so write access is gated only by the
 * (cleartext, UDP) community string in SNMP_COMMUNITY -- treat it as a weak
 * shared secret. Consider a *different*, harder-to-guess community for
 * read-write access than for read-only monitoring, and firewall UDP/SNMP_PORT
 * to a trusted network (LAN/VPN) given the destructive actions reachable
 * from base.5.5 (reboot/erase).
 */
class SNMPControlSource {
public:
  virtual ~SNMPControlSource() = default;

  /** Reads the live value of a scalar control field. Returns false if 'field' isn't recognized (shouldn't happen if SNMPOids.h and the switch are in sync). */
  virtual bool getControlField(SNMPControlField field, SNMPControlValue* out) = 0;

  /** Applies a SET to a scalar control field. 'err' (if non-null on return) is a short human-readable reason, surfaced only in SNMP_DEBUG logs. */
  virtual SNMPSetResult setControlField(SNMPControlField field, const SNMPControlValue& value, char* err, size_t err_len) = 0;

  // ---- aclTable (base.6.1.<col>.<index>, index = 1..getAclCount()) ----
  virtual int getAclCount() = 0;
  /** idx is 1-based (matches the SNMP table index), i.e. row 1 == ClientACL::getClientByIdx(0). Returns false if idx is out of range. */
  virtual bool getAclPubKeyHex(int idx, char* buf, size_t len) = 0;
  virtual bool getAclPermissions(int idx, uint8_t* out) = 0;
  virtual bool getAclLastActivityMillis(int idx, uint32_t* out) = 0;
  virtual SNMPSetResult setAclPermissions(int idx, uint8_t perms, char* err, size_t err_len) = 0;

  // ---- regionTable (base.7.1.<col>.<index>, index = 0..getRegionCount()-1) ----
  virtual int getRegionCount() = 0;
  virtual bool getRegionId(int idx, uint16_t* out) = 0;
  virtual bool getRegionParentId(int idx, uint16_t* out) = 0;
  virtual bool getRegionName(int idx, char* buf, size_t len) = 0;
  virtual bool getRegionFloodDenied(int idx, uint8_t* out) = 0;
  virtual bool getRegionIsHome(int idx, uint8_t* out) = 0;
  virtual bool getRegionIsDefault(int idx, uint8_t* out) = 0;
  /** Renaming isn't supported by RegionMap (no rename primitive) -- a write here always creates/repositions a NEW region named 'name' as a child of the existing region at 'idx', mirroring `region put <name> [parent]`; it does not touch row 'idx' itself. */
  virtual SNMPSetResult setRegionName(int idx, const char* name, char* err, size_t err_len) = 0;
  virtual SNMPSetResult setRegionFloodDenied(int idx, uint8_t deny, char* err, size_t err_len) = 0;
  virtual SNMPSetResult setRegionIsHome(int idx, uint8_t is_home, char* err, size_t err_len) = 0;
  virtual SNMPSetResult setRegionIsDefault(int idx, uint8_t is_default, char* err, size_t err_len) = 0;
};

