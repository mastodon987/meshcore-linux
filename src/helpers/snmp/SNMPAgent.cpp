#include "helpers/snmp/SNMPAgent.h"

#ifdef WITH_SNMP

#include "helpers/snmp/SNMPOids.h"
#include "helpers/snmp/BER.h"

#include <Arduino.h> // millis(), for nothing time-critical here but kept consistent with the rest of this codebase

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#ifdef SNMP_DEBUG
  #define SNMP_DEBUG_PRINTLN(F, ...) Serial.printf("SNMP: " F "\n", ##__VA_ARGS__)
#else
  #define SNMP_DEBUG_PRINTLN(...) {}
#endif

namespace {

using ber::Oid;
using ber::Reader;
using ber::Writer;
using ber::AnyValue;

// ----------------------------------------------------------------------------
// Read-only scalar stats table (groups 1-4) -- unchanged in shape from the
// original monitoring-only agent. Listed in strict ascending (group, leaf)
// order, which -- given the fixed "<base>.G.L.0" instance shape -- is exactly
// SNMP lexicographic order.
// ----------------------------------------------------------------------------
struct OidEntry { uint16_t group; uint16_t leaf; };

constexpr OidEntry kStatTable[] = {
  { SNMP_GRP_SYSINFO, SNMP_SYSINFO_NODE_NAME },
  { SNMP_GRP_SYSINFO, SNMP_SYSINFO_ROLE },
  { SNMP_GRP_SYSINFO, SNMP_SYSINFO_FW_VERSION },
  { SNMP_GRP_SYSINFO, SNMP_SYSINFO_PUBKEY_HEX },
  { SNMP_GRP_SYSINFO, SNMP_SYSINFO_UPTIME },
  { SNMP_GRP_SYSINFO, SNMP_SYSINFO_BATT_MV },

  { SNMP_GRP_RADIO, SNMP_RADIO_TOTAL_AIRTIME_MS },
  { SNMP_GRP_RADIO, SNMP_RADIO_RX_AIRTIME_MS },
  { SNMP_GRP_RADIO, SNMP_RADIO_TX_BUDGET_MS },
  { SNMP_GRP_RADIO, SNMP_RADIO_NOISE_FLOOR },
  { SNMP_GRP_RADIO, SNMP_RADIO_LAST_RSSI },
  { SNMP_GRP_RADIO, SNMP_RADIO_LAST_SNR },
  { SNMP_GRP_RADIO, SNMP_RADIO_DUTY_CYCLE_PCT },
  { SNMP_GRP_RADIO, SNMP_RADIO_AIRTIME_UTIL_PCT },

  { SNMP_GRP_PACKETS, SNMP_PKT_SENT_FLOOD },
  { SNMP_GRP_PACKETS, SNMP_PKT_SENT_DIRECT },
  { SNMP_GRP_PACKETS, SNMP_PKT_RECV_FLOOD },
  { SNMP_GRP_PACKETS, SNMP_PKT_RECV_DIRECT },
  { SNMP_GRP_PACKETS, SNMP_PKT_RADIO_RECV },
  { SNMP_GRP_PACKETS, SNMP_PKT_RADIO_SENT },
  { SNMP_GRP_PACKETS, SNMP_PKT_RADIO_RECV_ERRORS },
  { SNMP_GRP_PACKETS, SNMP_PKT_OUTBOUND_QUEUE_LEN },
  { SNMP_GRP_PACKETS, SNMP_PKT_ERR_FLAGS },

  { SNMP_GRP_BRIDGE, SNMP_BRIDGE_TX_PACKETS },
  { SNMP_GRP_BRIDGE, SNMP_BRIDGE_RX_PACKETS },
  { SNMP_GRP_BRIDGE, SNMP_BRIDGE_RECONNECTS },
};
constexpr size_t kStatTableLen = sizeof(kStatTable) / sizeof(kStatTable[0]);

// ----------------------------------------------------------------------------
// Read/write control scalars (group 5, sub-grouped). Same ascending-order
// requirement as kStatTable.
// ----------------------------------------------------------------------------
struct CtrlEntry { uint16_t subgroup; uint16_t leaf; bool writable; };

constexpr CtrlEntry kCtrlTable[] = {
  { SNMP_CTRL_RADIO_HW, SNMP_RHW_FREQ_MHZ,   true },
  { SNMP_CTRL_RADIO_HW, SNMP_RHW_BW_KHZ,     true },
  { SNMP_CTRL_RADIO_HW, SNMP_RHW_SF,               true },
  { SNMP_CTRL_RADIO_HW, SNMP_RHW_CR,               true },
  { SNMP_CTRL_RADIO_HW, SNMP_RHW_TX_POWER_DBM,     true },
  { SNMP_CTRL_RADIO_HW, SNMP_RHW_RX_BOOSTED_GAIN,  true },

  { SNMP_CTRL_ROUTING, SNMP_RT_REPEAT_ENABLED,       true },
  { SNMP_CTRL_ROUTING, SNMP_RT_DUTY_CYCLE_PCT,       true },
  { SNMP_CTRL_ROUTING, SNMP_RT_TX_DELAY_FACTOR, true },
  { SNMP_CTRL_ROUTING, SNMP_RT_DIRECT_TX_DELAY, true },
  { SNMP_CTRL_ROUTING, SNMP_RT_RX_DELAY_BASE,   true },
  { SNMP_CTRL_ROUTING, SNMP_RT_FLOOD_MAX,            true },
  { SNMP_CTRL_ROUTING, SNMP_RT_FLOOD_MAX_UNSCOPED,   true },
  { SNMP_CTRL_ROUTING, SNMP_RT_FLOOD_MAX_ADVERT,     true },
  { SNMP_CTRL_ROUTING, SNMP_RT_PATH_HASH_MODE,       true },
  { SNMP_CTRL_ROUTING, SNMP_RT_LOOP_DETECT,          true },
  { SNMP_CTRL_ROUTING, SNMP_RT_INTERFERENCE_THRESH,  true },
  { SNMP_CTRL_ROUTING, SNMP_RT_AGC_RESET_INTERVAL_S, true },
  { SNMP_CTRL_ROUTING, SNMP_RT_MULTI_ACKS,           true },
  { SNMP_CTRL_ROUTING, SNMP_RT_FLOOD_ADVERT_HRS,     true },
  { SNMP_CTRL_ROUTING, SNMP_RT_ADVERT_INTERVAL_MINS, true },

  { SNMP_CTRL_SYSTEM, SNMP_SYS_NODE_NAME,            true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_LAT,             true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_LON,             true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_OWNER_INFO,           true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_GUEST_PASSWORD,       true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_ADMIN_PASSWORD,       true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_ALLOW_READ_ONLY,      true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_ADC_MULTIPLIER, true },
  { SNMP_CTRL_SYSTEM, SNMP_SYS_POWER_SAVING,         true },

  { SNMP_CTRL_BRIDGE, SNMP_BR_ENABLED,        true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_DELAY_MS,       true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_SOURCE,         true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_MQTT_SERVER,    true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_MQTT_PORT,      true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_MQTT_TOPIC,     true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_MQTT_USER,      true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_MQTT_PASS,      true },
  { SNMP_CTRL_BRIDGE, SNMP_BR_MQTT_AUTOSTART, true },

  { SNMP_CTRL_ACTIONS, SNMP_ACT_REBOOT,       true },
  { SNMP_CTRL_ACTIONS, SNMP_ACT_CLKREBOOT,    true },
  { SNMP_CTRL_ACTIONS, SNMP_ACT_ERASE,        true },
  { SNMP_CTRL_ACTIONS, SNMP_ACT_SEND_ADVERT,  true },
  { SNMP_CTRL_ACTIONS, SNMP_ACT_CLEAR_STATS,  true },
};
constexpr size_t kCtrlTableLen = sizeof(kCtrlTable) / sizeof(kCtrlTable[0]);

bool isStringField(const CtrlEntry& e) {
  if (e.subgroup == SNMP_CTRL_RADIO_HW) {
    return e.leaf == SNMP_RHW_FREQ_MHZ || e.leaf == SNMP_RHW_BW_KHZ;
  }
  if (e.subgroup == SNMP_CTRL_ROUTING) {
    return e.leaf == SNMP_RT_TX_DELAY_FACTOR || e.leaf == SNMP_RT_DIRECT_TX_DELAY ||
           e.leaf == SNMP_RT_RX_DELAY_BASE;
  }
  if (e.subgroup == SNMP_CTRL_SYSTEM) {
    return e.leaf == SNMP_SYS_NODE_NAME || e.leaf == SNMP_SYS_LAT || e.leaf == SNMP_SYS_LON ||
           e.leaf == SNMP_SYS_OWNER_INFO || e.leaf == SNMP_SYS_GUEST_PASSWORD ||
           e.leaf == SNMP_SYS_ADMIN_PASSWORD || e.leaf == SNMP_SYS_ADC_MULTIPLIER;
  }
  if (e.subgroup == SNMP_CTRL_BRIDGE) {
    return e.leaf == SNMP_BR_MQTT_SERVER || e.leaf == SNMP_BR_MQTT_TOPIC ||
           e.leaf == SNMP_BR_MQTT_USER || e.leaf == SNMP_BR_MQTT_PASS;
  }
  return false;
}

const uint32_t kBaseOidIds[] = SNMP_BASE_OID;

Oid baseOid() {
  Oid o;
  o.len = SNMP_BASE_OID_LEN;
  for (size_t i = 0; i < SNMP_BASE_OID_LEN; i++) o.ids[i] = kBaseOidIds[i];
  return o;
}

Oid statOid(const OidEntry& e) {
  Oid o = baseOid();
  o.ids[o.len++] = e.group;
  o.ids[o.len++] = e.leaf;
  o.ids[o.len++] = 0;
  return o;
}

Oid ctrlOid(const CtrlEntry& e) {
  Oid o = baseOid();
  o.ids[o.len++] = SNMP_GRP_CONTROL;
  o.ids[o.len++] = e.subgroup;
  o.ids[o.len++] = e.leaf;
  o.ids[o.len++] = 0;
  return o;
}

Oid aclCellOid(uint16_t column, int index) {
  Oid o = baseOid();
  o.ids[o.len++] = SNMP_GRP_ACL_TABLE;
  o.ids[o.len++] = 1; // aclEntry
  o.ids[o.len++] = column;
  o.ids[o.len++] = (uint32_t)index;
  return o;
}

Oid regionCellOid(uint16_t column, int index) {
  Oid o = baseOid();
  o.ids[o.len++] = SNMP_GRP_REGION_TABLE;
  o.ids[o.len++] = 1; // regionEntry
  o.ids[o.len++] = column;
  o.ids[o.len++] = (uint32_t)index;
  return o;
}

enum class LocKind { None, Stat, Ctrl, Acl, Region };

struct Loc {
  LocKind kind = LocKind::None;
  size_t stat_idx = 0;
  size_t ctrl_idx = 0;
  uint16_t table_col = 0;
  int table_row = 0;
};

Oid locOid(const Loc& loc) {
  switch (loc.kind) {
    case LocKind::Stat:   return statOid(kStatTable[loc.stat_idx]);
    case LocKind::Ctrl:   return ctrlOid(kCtrlTable[loc.ctrl_idx]);
    case LocKind::Acl:    return aclCellOid(loc.table_col, loc.table_row);
    case LocKind::Region: return regionCellOid(loc.table_col, loc.table_row);
    default: { Oid o; o.len = 0; return o; }
  }
}

bool nextAclCell(SNMPControlSource* ctrl, uint16_t after_col, int after_row, Loc* out) {
  if (!ctrl) return false;
  int n = ctrl->getAclCount();
  if (n <= 0) return false;

  uint16_t col = after_col;
  int row = after_row + 1;
  if (col < SNMP_ACL_COL_MIN) { col = SNMP_ACL_COL_MIN; row = 1; }

  while (col <= SNMP_ACL_COL_MAX) {
    if (row >= 1 && row <= n) {
      out->kind = LocKind::Acl;
      out->table_col = col;
      out->table_row = row;
      return true;
    }
    col++;
    row = 1;
  }
  return false;
}

bool nextRegionCell(SNMPControlSource* ctrl, uint16_t after_col, int after_row, Loc* out) {
  if (!ctrl) return false;
  int n = ctrl->getRegionCount();
  if (n <= 0) return false;

  uint16_t col = after_col;
  int row = after_row + 1;
  if (col < SNMP_REGION_COL_MIN) { col = SNMP_REGION_COL_MIN; row = 0; }

  while (col <= SNMP_REGION_COL_MAX) {
    if (row >= 0 && row < n) {
      out->kind = LocKind::Region;
      out->table_col = col;
      out->table_row = row;
      return true;
    }
    col++;
    row = 0;
  }
  return false;
}

Loc findNext(const Oid& requested, SNMPDataSource* /*src*/, SNMPControlSource* ctrl) {
  Loc best;
  Oid best_oid; best_oid.len = 0;
  bool have_best = false;

  auto consider = [&](const Loc& candidate) {
    Oid o = locOid(candidate);
    if (o.compare(requested) > 0 && (!have_best || o.compare(best_oid) < 0)) {
      best = candidate;
      best_oid = o;
      have_best = true;
    }
  };

  for (size_t i = 0; i < kStatTableLen; i++) {
    Loc l; l.kind = LocKind::Stat; l.stat_idx = i;
    consider(l);
  }
  for (size_t i = 0; i < kCtrlTableLen; i++) {
    Loc l; l.kind = LocKind::Ctrl; l.ctrl_idx = i;
    consider(l);
  }
  if (ctrl) {
    Oid acl_base = baseOid(); acl_base.ids[acl_base.len++] = SNMP_GRP_ACL_TABLE; acl_base.ids[acl_base.len++] = 1;
    Oid region_base = baseOid(); region_base.ids[region_base.len++] = SNMP_GRP_REGION_TABLE; region_base.ids[region_base.len++] = 1;

    Loc acl_candidate;
    if (requested.startsWith(acl_base) && requested.len >= acl_base.len + 2) {
      nextAclCell(ctrl, (uint16_t)requested.ids[acl_base.len], (int)requested.ids[acl_base.len + 1], &acl_candidate);
    } else {
      nextAclCell(ctrl, 0, 0, &acl_candidate);
    }
    if (acl_candidate.kind == LocKind::Acl) consider(acl_candidate);

    Loc region_candidate;
    if (requested.startsWith(region_base) && requested.len >= region_base.len + 2) {
      nextRegionCell(ctrl, (uint16_t)requested.ids[region_base.len], (int)requested.ids[region_base.len + 1], &region_candidate);
    } else {
      nextRegionCell(ctrl, 0, -1, &region_candidate);
    }
    if (region_candidate.kind == LocKind::Region) consider(region_candidate);
  }

  if (!have_best) best.kind = LocKind::None;
  return best;
}

Loc findExact(const Oid& requested, SNMPControlSource* ctrl) {
  for (size_t i = 0; i < kStatTableLen; i++) {
    if (statOid(kStatTable[i]) == requested) { Loc l; l.kind = LocKind::Stat; l.stat_idx = i; return l; }
  }
  for (size_t i = 0; i < kCtrlTableLen; i++) {
    if (ctrlOid(kCtrlTable[i]) == requested) { Loc l; l.kind = LocKind::Ctrl; l.ctrl_idx = i; return l; }
  }
  if (ctrl) {
    Oid acl_base = baseOid(); acl_base.ids[acl_base.len++] = SNMP_GRP_ACL_TABLE; acl_base.ids[acl_base.len++] = 1;
    if (requested.startsWith(acl_base) && requested.len == acl_base.len + 2) {
      uint16_t col = (uint16_t)requested.ids[acl_base.len];
      int row = (int)requested.ids[acl_base.len + 1];
      if (col >= SNMP_ACL_COL_MIN && col <= SNMP_ACL_COL_MAX && row >= 1 && row <= ctrl->getAclCount()) {
        Loc l; l.kind = LocKind::Acl; l.table_col = col; l.table_row = row; return l;
      }
    }
    Oid region_base = baseOid(); region_base.ids[region_base.len++] = SNMP_GRP_REGION_TABLE; region_base.ids[region_base.len++] = 1;
    if (requested.startsWith(region_base) && requested.len == region_base.len + 2) {
      uint16_t col = (uint16_t)requested.ids[region_base.len];
      int row = (int)requested.ids[region_base.len + 1];
      if (col >= SNMP_REGION_COL_MIN && col <= SNMP_REGION_COL_MAX && row >= 0 && row < ctrl->getRegionCount()) {
        Loc l; l.kind = LocKind::Region; l.table_col = col; l.table_row = row; return l;
      }
    }
  }
  Loc l; l.kind = LocKind::None; return l;
}

bool writeStatValue(const OidEntry& e, SNMPDataSource* src, SNMPAgent* agent, Writer& w) {
  char strbuf[160];
  switch (e.group) {
    case SNMP_GRP_SYSINFO:
      switch (e.leaf) {
        case SNMP_SYSINFO_NODE_NAME:  return w.writeOctetStringCStr(src->getNodeName());
        case SNMP_SYSINFO_ROLE:       return w.writeOctetStringCStr(src->getFirmwareRole());
        case SNMP_SYSINFO_FW_VERSION: return w.writeOctetStringCStr(src->getFirmwareVersion());
        case SNMP_SYSINFO_PUBKEY_HEX:
          src->getPublicKeyHex(strbuf, sizeof(strbuf));
          return w.writeOctetStringCStr(strbuf);
        case SNMP_SYSINFO_UPTIME:
          return w.writeUnsigned32Tagged(ber::TAG_TIMETICKS, src->getUptimeMillis() / 10);
        case SNMP_SYSINFO_BATT_MV:
          return w.writeUnsigned32Tagged(ber::TAG_GAUGE32, src->getBattMilliVolts());
      }
      break;
    case SNMP_GRP_RADIO:
      switch (e.leaf) {
        case SNMP_RADIO_TOTAL_AIRTIME_MS: return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getTotalAirTime());
        case SNMP_RADIO_RX_AIRTIME_MS:    return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getReceiveAirTime());
        case SNMP_RADIO_TX_BUDGET_MS:     return w.writeUnsigned32Tagged(ber::TAG_GAUGE32, src->getRemainingTxBudget());
        case SNMP_RADIO_NOISE_FLOOR:      return w.writeInteger(src->getNoiseFloor());
        case SNMP_RADIO_LAST_RSSI:        return w.writeInteger(src->getLastRSSI());
        case SNMP_RADIO_LAST_SNR:
          snprintf(strbuf, sizeof(strbuf), "%.2f", (double)src->getLastSNR());
          return w.writeOctetStringCStr(strbuf);
        case SNMP_RADIO_DUTY_CYCLE_PCT:
          snprintf(strbuf, sizeof(strbuf), "%.2f", (double)src->getDutyCyclePct());
          return w.writeOctetStringCStr(strbuf);
        case SNMP_RADIO_AIRTIME_UTIL_PCT:
          if (!agent) return false;
          agent->sampleAirtimeUtilizationPct(strbuf, sizeof(strbuf));
          return w.writeOctetStringCStr(strbuf);
      }
      break;
    case SNMP_GRP_PACKETS:
      switch (e.leaf) {
        case SNMP_PKT_SENT_FLOOD:         return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getNumSentFlood());
        case SNMP_PKT_SENT_DIRECT:        return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getNumSentDirect());
        case SNMP_PKT_RECV_FLOOD:         return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getNumRecvFlood());
        case SNMP_PKT_RECV_DIRECT:        return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getNumRecvDirect());
        case SNMP_PKT_RADIO_RECV:         return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getRadioPacketsRecv());
        case SNMP_PKT_RADIO_SENT:         return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getRadioPacketsSent());
        case SNMP_PKT_RADIO_RECV_ERRORS:  return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getRadioPacketsRecvErrors());
        case SNMP_PKT_OUTBOUND_QUEUE_LEN: return w.writeUnsigned32Tagged(ber::TAG_GAUGE32, src->getOutboundQueueLen());
        case SNMP_PKT_ERR_FLAGS:          return w.writeUnsigned32Tagged(ber::TAG_GAUGE32, src->getErrFlags());
      }
      break;
    case SNMP_GRP_BRIDGE:
      switch (e.leaf) {
        case SNMP_BRIDGE_TX_PACKETS:  return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getBridgeTxPackets());
        case SNMP_BRIDGE_RX_PACKETS:  return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getBridgeRxPackets());
        case SNMP_BRIDGE_RECONNECTS: return w.writeUnsigned32Tagged(ber::TAG_COUNTER32, src->getBridgeReconnects());
      }
      break;
  }
  return false;
}

bool isMaskedOnRead(const CtrlEntry& e) {
  return (e.subgroup == SNMP_CTRL_SYSTEM && (e.leaf == SNMP_SYS_GUEST_PASSWORD || e.leaf == SNMP_SYS_ADMIN_PASSWORD)) ||
         (e.subgroup == SNMP_CTRL_BRIDGE && e.leaf == SNMP_BR_MQTT_PASS);
}

bool writeCtrlValue(const CtrlEntry& e, SNMPControlSource* ctrl, Writer& w) {
  if (!ctrl) return false;
  if (e.subgroup == SNMP_CTRL_ACTIONS) {
    return w.writeInteger(0);
  }
  SNMPControlField field{ e.subgroup, e.leaf };
  SNMPControlValue val;
  if (!ctrl->getControlField(field, &val)) return false;

  if (isMaskedOnRead(e)) {
    return w.writeOctetStringCStr(val.str_value[0] ? "***" : "");
  }
  if (isStringField(e)) {
    return w.writeOctetStringCStr(val.str_value);
  }
  return w.writeInteger(val.int_value);
}

bool writeAclValue(uint16_t col, int row, SNMPControlSource* ctrl, Writer& w) {
  if (!ctrl) return false;
  char strbuf[160];
  switch (col) {
    case SNMP_ACL_COL_PUBKEY_HEX:
      if (!ctrl->getAclPubKeyHex(row, strbuf, sizeof(strbuf))) return false;
      return w.writeOctetStringCStr(strbuf);
    case SNMP_ACL_COL_PERMISSIONS: {
      uint8_t perms;
      if (!ctrl->getAclPermissions(row, &perms)) return false;
      return w.writeInteger(perms);
    }
    case SNMP_ACL_COL_LAST_ACTIVITY: {
      uint32_t ms;
      if (!ctrl->getAclLastActivityMillis(row, &ms)) return false;
      return w.writeUnsigned32Tagged(ber::TAG_TIMETICKS, ms / 10);
    }
  }
  return false;
}

bool writeRegionValue(uint16_t col, int row, SNMPControlSource* ctrl, Writer& w) {
  if (!ctrl) return false;
  char strbuf[40];
  switch (col) {
    case SNMP_REGION_COL_ID: {
      uint16_t id;
      if (!ctrl->getRegionId(row, &id)) return false;
      return w.writeInteger(id);
    }
    case SNMP_REGION_COL_PARENT_ID: {
      uint16_t id;
      if (!ctrl->getRegionParentId(row, &id)) return false;
      return w.writeInteger(id);
    }
    case SNMP_REGION_COL_NAME:
      if (!ctrl->getRegionName(row, strbuf, sizeof(strbuf))) return false;
      return w.writeOctetStringCStr(strbuf);
    case SNMP_REGION_COL_FLOOD_DENIED: {
      uint8_t v;
      if (!ctrl->getRegionFloodDenied(row, &v)) return false;
      return w.writeInteger(v);
    }
    case SNMP_REGION_COL_IS_HOME: {
      uint8_t v;
      if (!ctrl->getRegionIsHome(row, &v)) return false;
      return w.writeInteger(v);
    }
    case SNMP_REGION_COL_IS_DEFAULT: {
      uint8_t v;
      if (!ctrl->getRegionIsDefault(row, &v)) return false;
      return w.writeInteger(v);
    }
  }
  return false;
}

bool writeLocValue(const Loc& loc, SNMPDataSource* src, SNMPControlSource* ctrl, SNMPAgent* agent, Writer& w) {
  switch (loc.kind) {
    case LocKind::Stat:   return writeStatValue(kStatTable[loc.stat_idx], src, agent, w);
    case LocKind::Ctrl:   return writeCtrlValue(kCtrlTable[loc.ctrl_idx], ctrl, w);
    case LocKind::Acl:    return writeAclValue(loc.table_col, loc.table_row, ctrl, w);
    case LocKind::Region: return writeRegionValue(loc.table_col, loc.table_row, ctrl, w);
    default: return false;
  }
}

int setResultToErrorStatus(SNMPSetResult r) {
  switch (r) {
    case SNMPSetResult::Ok:             return ber::ERR_NO_ERROR;
    case SNMPSetResult::NotWritable:    return ber::ERR_NOT_WRITABLE;
    case SNMPSetResult::WrongType:      return ber::ERR_WRONG_TYPE;
    case SNMPSetResult::WrongValue:     return ber::ERR_WRONG_VALUE;
    case SNMPSetResult::GenErr:         return ber::ERR_GEN_ERR;
    case SNMPSetResult::NoSuchInstance: return ber::ERR_GEN_ERR;
    default:                            return ber::ERR_GEN_ERR;
  }
}

SNMPSetResult applySet(const Loc& loc, const AnyValue& incoming, SNMPControlSource* ctrl, char* err, size_t err_len) {
  if (!ctrl) { if (err) snprintf(err, err_len, "control disabled"); return SNMPSetResult::GenErr; }

  switch (loc.kind) {
    case LocKind::Stat:
      if (err) snprintf(err, err_len, "stat OIDs are read-only");
      return SNMPSetResult::NotWritable;

    case LocKind::Ctrl: {
      const CtrlEntry& e = kCtrlTable[loc.ctrl_idx];
      if (!e.writable) { if (err) snprintf(err, err_len, "not writable"); return SNMPSetResult::NotWritable; }

      SNMPControlValue val;
      if (isStringField(e)) {
        if (incoming.kind != AnyValue::Kind::OctetString) {
          if (err) snprintf(err, err_len, "expected OCTET STRING");
          return SNMPSetResult::WrongType;
        }
        val = SNMPControlValue::ofStr(incoming.str_value);
      } else {
        if (incoming.kind != AnyValue::Kind::Integer) {
          if (err) snprintf(err, err_len, "expected INTEGER");
          return SNMPSetResult::WrongType;
        }
        val = SNMPControlValue::ofInt(incoming.int_value);
      }
      SNMPControlField field{ e.subgroup, e.leaf };
      return ctrl->setControlField(field, val, err, err_len);
    }

    case LocKind::Acl: {
      if (loc.table_col != SNMP_ACL_COL_PERMISSIONS) {
        if (err) snprintf(err, err_len, "column is read-only");
        return SNMPSetResult::NotWritable;
      }
      if (incoming.kind != AnyValue::Kind::Integer) {
        if (err) snprintf(err, err_len, "expected INTEGER");
        return SNMPSetResult::WrongType;
      }
      if (incoming.int_value < 0 || incoming.int_value > 3) {
        if (err) snprintf(err, err_len, "permissions must be 0-3");
        return SNMPSetResult::WrongValue;
      }
      return ctrl->setAclPermissions(loc.table_row, (uint8_t)incoming.int_value, err, err_len);
    }

    case LocKind::Region: {
      switch (loc.table_col) {
        case SNMP_REGION_COL_NAME:
          if (incoming.kind != AnyValue::Kind::OctetString) { if (err) snprintf(err, err_len, "expected OCTET STRING"); return SNMPSetResult::WrongType; }
          return ctrl->setRegionName(loc.table_row, incoming.str_value, err, err_len);
        case SNMP_REGION_COL_FLOOD_DENIED:
          if (incoming.kind != AnyValue::Kind::Integer) { if (err) snprintf(err, err_len, "expected INTEGER"); return SNMPSetResult::WrongType; }
          return ctrl->setRegionFloodDenied(loc.table_row, incoming.int_value != 0, err, err_len);
        case SNMP_REGION_COL_IS_HOME:
          if (incoming.kind != AnyValue::Kind::Integer) { if (err) snprintf(err, err_len, "expected INTEGER"); return SNMPSetResult::WrongType; }
          return ctrl->setRegionIsHome(loc.table_row, incoming.int_value != 0, err, err_len);
        case SNMP_REGION_COL_IS_DEFAULT:
          if (incoming.kind != AnyValue::Kind::Integer) { if (err) snprintf(err, err_len, "expected INTEGER"); return SNMPSetResult::WrongType; }
          return ctrl->setRegionIsDefault(loc.table_row, incoming.int_value != 0, err, err_len);
        default:
          if (err) snprintf(err, err_len, "column is read-only");
          return SNMPSetResult::NotWritable;
      }
    }

    default:
      if (err) snprintf(err, err_len, "no such object");
      return SNMPSetResult::NoSuchInstance;
  }
}

enum class PduKind { GetRequest, GetNextRequest, GetBulkRequest, SetRequest, Unsupported };

struct ParsedVarBind { Oid oid; AnyValue value; };

struct ParsedRequest {
  int version = -1;
  char community[64] = {0};
  PduKind kind = PduKind::Unsupported;
  int64_t request_id = 0;
  int64_t arg2 = 0;
  int64_t arg3 = 0;
  ParsedVarBind varbinds[16];
  size_t n_varbinds = 0;
};

bool parseRequest(const uint8_t* data, size_t len, ParsedRequest* out) {
  Reader r(data, len);

  size_t seq_len;
  if (!r.enterSequence(&seq_len)) return false;

  int64_t version;
  if (!r.readInteger(&version)) return false;
  out->version = (int)version;

  size_t community_len;
  if (!r.readOctetString(out->community, sizeof(out->community), &community_len)) return false;

  uint8_t pdu_tag;
  size_t pdu_len;
  if (!r.enterAnyConstructed(&pdu_tag, &pdu_len)) return false;

  switch (pdu_tag) {
    case ber::PDU_GET_REQUEST:      out->kind = PduKind::GetRequest; break;
    case ber::PDU_GET_NEXT_REQUEST: out->kind = PduKind::GetNextRequest; break;
    case ber::PDU_GET_BULK_REQUEST: out->kind = PduKind::GetBulkRequest; break;
    case ber::PDU_SET_REQUEST:      out->kind = PduKind::SetRequest; break;
    default:                        out->kind = PduKind::Unsupported; return true;
  }

  if (!r.readInteger(&out->request_id)) return false;
  if (!r.readInteger(&out->arg2)) return false;
  if (!r.readInteger(&out->arg3)) return false;

  size_t vb_list_len;
  if (!r.enterSequence(&vb_list_len)) return false;
  size_t vb_list_end = r.pos() + vb_list_len;

  while (r.pos() < vb_list_end && out->n_varbinds < (sizeof(out->varbinds) / sizeof(out->varbinds[0]))) {
    size_t vb_len;
    if (!r.enterSequence(&vb_len)) return false;
    size_t vb_end = r.pos() + vb_len;

    Oid oid;
    if (!r.readOid(&oid)) return false;
    out->varbinds[out->n_varbinds].oid = oid;

    if (out->kind == PduKind::SetRequest) {
      if (!r.readAnyValue(&out->varbinds[out->n_varbinds].value)) return false;
    } else if (r.pos() < vb_end) {
      r.skip(vb_end - r.pos());
    }
    out->n_varbinds++;

    if (r.pos() < vb_end) r.skip(vb_end - r.pos());
  }

  return true;
}

bool writeVarBind(Writer& w, const Oid& oid, const Loc& loc, SNMPDataSource* src, SNMPControlSource* ctrl, SNMPAgent* agent, uint8_t exception_if_missing) {
  Writer::Marker vb_marker;
  if (!w.beginConstructed(ber::TAG_SEQUENCE, &vb_marker)) return false;
  if (!w.writeOid(oid)) return false;
  bool wrote = (loc.kind != LocKind::None) && writeLocValue(loc, src, ctrl, agent, w);
  if (!wrote) {
    if (!w.writeExceptionValue(exception_if_missing)) return false;
  }
  return w.endConstructed(vb_marker);
}

template <typename BuildVarbinds>
size_t buildResponse(uint8_t* out_buf, size_t out_cap, int version, const char* community,
                      int64_t request_id, int error_status, int error_index,
                      BuildVarbinds build_varbinds) {
  Writer w(out_buf, out_cap);

  Writer::Marker msg_marker;
  if (!w.beginConstructed(ber::TAG_SEQUENCE, &msg_marker)) return 0;
  if (!w.writeInteger(version)) return 0;
  if (!w.writeOctetStringCStr(community)) return 0;

  Writer::Marker pdu_marker;
  if (!w.beginConstructed(ber::PDU_GET_RESPONSE, &pdu_marker)) return 0;
  if (!w.writeInteger(request_id)) return 0;
  if (!w.writeInteger(error_status)) return 0;
  if (!w.writeInteger(error_index)) return 0;

  Writer::Marker vbs_marker;
  if (!w.beginConstructed(ber::TAG_SEQUENCE, &vbs_marker)) return 0;
  if (!build_varbinds(w)) return 0;
  if (!w.endConstructed(vbs_marker)) return 0;

  if (!w.endConstructed(pdu_marker)) return 0;
  if (!w.endConstructed(msg_marker)) return 0;

  return w.length();
}

bool rewriteOriginalValue(const AnyValue& v, Writer& w) {
  switch (v.kind) {
    case AnyValue::Kind::Integer:     return w.writeInteger(v.int_value);
    case AnyValue::Kind::OctetString: return w.writeOctetString(v.str_value, v.str_len < sizeof(v.str_value) ? v.str_len : strlen(v.str_value));
    case AnyValue::Kind::Null:        return w.writeNull();
    default:                          return w.writeNull();
  }
}

} // namespace

void SNMPAgent::begin() {
  _sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (_sock < 0) {
    SNMP_DEBUG_PRINTLN("socket() failed: %s", strerror(errno));
    return;
  }

  int reuse = 1;
  setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  int flags = fcntl(_sock, F_GETFL, 0);
  fcntl(_sock, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(SNMP_PORT);

  if (bind(_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    SNMP_DEBUG_PRINTLN("bind() to UDP/%d failed: %s (need root, or a lower port, or "
                        "`setcap cap_net_bind_service+ep` on the binary for ports < 1024)",
                        SNMP_PORT, strerror(errno));
    close(_sock);
    _sock = -1;
    return;
  }

  SNMP_DEBUG_PRINTLN("listening on UDP/%d, ro-community '%s', rw-community '%s'%s, base OID in SNMPOids.h",
                      SNMP_PORT, SNMP_COMMUNITY, SNMP_RW_COMMUNITY, _control ? "" : " (control disabled: no SNMPControlSource)");
}

void SNMPAgent::end() {
  if (_sock >= 0) {
    close(_sock);
    _sock = -1;
  }
}

void SNMPAgent::sampleAirtimeUtilizationPct(char* out, size_t out_cap) {
  uint32_t now_ms = _source->getUptimeMillis();
  uint32_t airtime_ms = _source->getTotalAirTime();

  if (!_util_has_sample) {
    // No prior sample to diff against yet (first read since boot/agent
    // start) -- report 0.00 rather than a meaningless huge ratio against
    // "time since boot", and start the rolling window from here.
    _util_has_sample = true;
    _util_last_wallclock_ms = now_ms;
    _util_last_airtime_ms = airtime_ms;
    snprintf(out, out_cap, "0.00");
    return;
  }

  // Both getUptimeMillis() and getTotalAirTime() are 'unsigned long'
  // counters that wrap (millis()-style); plain unsigned subtraction gives
  // the correct delta across a wraparound as long as it happened at most
  // once between samples, which holds for any realistic poll interval.
  uint32_t wallclock_delta = now_ms - _util_last_wallclock_ms;
  uint32_t airtime_delta = airtime_ms - _util_last_airtime_ms;

  _util_last_wallclock_ms = now_ms;
  _util_last_airtime_ms = airtime_ms;

  if (wallclock_delta == 0) {
    // Asked again in the same millisecond as last time (or clock hasn't
    // advanced) -- avoid a divide-by-zero; nothing meaningful to report.
    snprintf(out, out_cap, "0.00");
    return;
  }

  double pct = (double)airtime_delta / (double)wallclock_delta * 100.0;
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0; // clamp defensively; airtime_delta should never exceed wallclock_delta
  snprintf(out, out_cap, "%.2f", pct);
}

void SNMPAgent::loop() {
  if (_sock < 0) return;

  struct sockaddr_in from_addr;
  socklen_t from_len = sizeof(from_addr);

  ssize_t n = recvfrom(_sock, _rx_buf, sizeof(_rx_buf), 0, (struct sockaddr*)&from_addr, &from_len);
  if (n <= 0) return;

  handleDatagram(_rx_buf, (size_t)n, &from_addr, from_len);
}

void SNMPAgent::handleDatagram(const uint8_t* data, size_t len, const void* from_addr_ptr, size_t from_addr_len) {
  ParsedRequest req;
  if (!parseRequest(data, len, &req)) {
    SNMP_DEBUG_PRINTLN("dropped malformed packet (%zu bytes)", len);
    return;
  }

  if (req.version != 1) {
    SNMP_DEBUG_PRINTLN("dropped packet with unsupported version %d", req.version);
    return;
  }

  if (req.kind == PduKind::Unsupported) return;

  bool is_write = (req.kind == PduKind::SetRequest);
  bool community_ok = is_write
    ? (strcmp(req.community, SNMP_RW_COMMUNITY) == 0)
    : (strcmp(req.community, SNMP_COMMUNITY) == 0 || strcmp(req.community, SNMP_RW_COMMUNITY) == 0);

  if (!community_ok) {
    SNMP_DEBUG_PRINTLN("dropped packet with wrong community (write=%d)", is_write ? 1 : 0);
    return;
  }

  size_t resp_len = 0;
  const struct sockaddr_in* from_addr = (const struct sockaddr_in*)from_addr_ptr;

  if (req.kind == PduKind::GetRequest) {
    resp_len = buildResponse(_tx_buf, sizeof(_tx_buf), req.version, req.community, req.request_id,
      ber::ERR_NO_ERROR, 0,
      [&](Writer& w) -> bool {
        for (size_t i = 0; i < req.n_varbinds; i++) {
          Loc loc = findExact(req.varbinds[i].oid, _control);
          if (!writeVarBind(w, req.varbinds[i].oid, loc, _source, _control, this, ber::EXC_NO_SUCH_OBJECT)) return false;
        }
        return true;
      });

  } else if (req.kind == PduKind::GetNextRequest) {
    resp_len = buildResponse(_tx_buf, sizeof(_tx_buf), req.version, req.community, req.request_id,
      ber::ERR_NO_ERROR, 0,
      [&](Writer& w) -> bool {
        for (size_t i = 0; i < req.n_varbinds; i++) {
          Loc loc = findNext(req.varbinds[i].oid, _source, _control);
          Oid next_oid = (loc.kind != LocKind::None) ? locOid(loc) : req.varbinds[i].oid;
          if (!writeVarBind(w, next_oid, loc, _source, _control, this, ber::EXC_END_OF_MIB_VIEW)) return false;
        }
        return true;
      });

  } else if (req.kind == PduKind::GetBulkRequest) {
    int64_t non_repeaters = req.arg2;
    int64_t max_repetitions = req.arg3;
    if (non_repeaters < 0) non_repeaters = 0;
    if (max_repetitions < 0) max_repetitions = 0;
    if (max_repetitions > 64) max_repetitions = 64;

    resp_len = buildResponse(_tx_buf, sizeof(_tx_buf), req.version, req.community, req.request_id,
      ber::ERR_NO_ERROR, 0,
      [&](Writer& w) -> bool {
        for (size_t i = 0; i < req.n_varbinds; i++) {
          bool is_repeated = (int64_t)i >= non_repeaters;
          int64_t reps = is_repeated ? max_repetitions : 1;
          if (reps < 1) reps = 1;

          Oid cursor = req.varbinds[i].oid;
          for (int64_t rep = 0; rep < reps; rep++) {
            Loc loc = findNext(cursor, _source, _control);
            Oid next_oid = (loc.kind != LocKind::None) ? locOid(loc) : cursor;
            if (!writeVarBind(w, next_oid, loc, _source, _control, this, ber::EXC_END_OF_MIB_VIEW)) return false;
            if (loc.kind == LocKind::None) break;
            cursor = next_oid;
          }
        }
        return true;
      });

  } else { // SetRequest
    if (!_control) {
      resp_len = buildResponse(_tx_buf, sizeof(_tx_buf), req.version, req.community, req.request_id,
        ber::ERR_GEN_ERR, req.n_varbinds > 0 ? 1 : 0,
        [&](Writer& w) -> bool {
          for (size_t i = 0; i < req.n_varbinds; i++) {
            Writer::Marker vb_marker;
            if (!w.beginConstructed(ber::TAG_SEQUENCE, &vb_marker)) return false;
            if (!w.writeOid(req.varbinds[i].oid)) return false;
            if (!rewriteOriginalValue(req.varbinds[i].value, w)) return false;
            if (!w.endConstructed(vb_marker)) return false;
          }
          return true;
        });
      sendto(_sock, _tx_buf, resp_len, 0, (const struct sockaddr*)from_addr, (socklen_t)from_addr_len);
      return;
    }

    // RFC 1905 intends all-or-nothing application of a SetRequest's
    // varbinds. There's no cross-subsystem transaction (NodePrefs / ACL /
    // RegionMap) to roll back in this codebase, so we apply optimistically
    // in varbind order and stop at the first failure; send one varbind per
    // SetRequest if you need strict atomicity guarantees.
    int first_error_status = ber::ERR_NO_ERROR;
    size_t first_error_index = 0;

    for (size_t i = 0; i < req.n_varbinds && first_error_status == ber::ERR_NO_ERROR; i++) {
      Loc loc = findExact(req.varbinds[i].oid, _control);
      if (loc.kind == LocKind::None) {
        first_error_status = ber::ERR_GEN_ERR;
        first_error_index = i + 1;
        SNMP_DEBUG_PRINTLN("SET: no such object");
        break;
      }
      char errbuf[64] = {0};
      SNMPSetResult result = applySet(loc, req.varbinds[i].value, _control, errbuf, sizeof(errbuf));
      if (result != SNMPSetResult::Ok) {
        first_error_status = setResultToErrorStatus(result);
        first_error_index = i + 1;
        SNMP_DEBUG_PRINTLN("SET failed: %s", errbuf);
        break;
      }
    }

    resp_len = buildResponse(_tx_buf, sizeof(_tx_buf), req.version, req.community, req.request_id,
      first_error_status, (int)first_error_index,
      [&](Writer& w) -> bool {
        for (size_t i = 0; i < req.n_varbinds; i++) {
          Writer::Marker vb_marker;
          if (!w.beginConstructed(ber::TAG_SEQUENCE, &vb_marker)) return false;
          if (!w.writeOid(req.varbinds[i].oid)) return false;
          if (!rewriteOriginalValue(req.varbinds[i].value, w)) return false;
          if (!w.endConstructed(vb_marker)) return false;
        }
        return true;
      });
  }

  if (resp_len == 0) {
    SNMP_DEBUG_PRINTLN("failed to build response (request likely too large for SNMP_MAX_PACKET)");
    return;
  }

  sendto(_sock, _tx_buf, resp_len, 0, (const struct sockaddr*)from_addr, (socklen_t)from_addr_len);
}

#endif // WITH_SNMP
