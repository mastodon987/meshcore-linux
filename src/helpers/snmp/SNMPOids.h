#pragma once

/**
 * @brief OID tree served by SNMPAgent.
 *
 * Base arc -- configurable at compile time:
 *
 *   SNMP_BASE_OID  default: 1.3.6.1.4.1.62911.1
 *                  (iso.org.dod.internet.private.enterprises.62911.1)
 *
 * NOTE ON THE OID ARC: 62911 is used here as a placeholder Private
 * Enterprise Number (PEN) under the standard IANA "enterprises" arc
 * (1.3.6.1.4.1.<PEN>). This number has NOT been verified as actually
 * registered to/reserved for this project with IANA. Before relying on
 * this OID tree against any shared/standard tooling, register a real PEN
 * (it's free) at https://pen.iana.org/pen/PenApplication.page and update
 * SNMP_BASE_OID accordingly -- every OID below moves with it automatically
 * since they're all defined relative to SNMP_BASE_OID_LEN/SNMP_BASE_OID.
 *
 * Layout under the base arc -- read-only stats (groups 1-4) plus the
 * read/write control tree (groups 5-7). See docs/SNMP_MIB.txt (sibling MIB
 * file) for the canonical, fully-commented module-form definition; this
 * header is the authoritative *numbering*, the MIB file is documentation
 * derived from it -- keep them in sync if you renumber anything here.
 *
 *   .1.x   sysInfo            (read-only scalars)
 *   .2.x   radioStats         (read-only scalars)
 *   .3.x   packetStats        (read-only scalars)
 *   .4.x   bridgeStats        (read-only scalars; reads as 0 with no bridge)
 *
 *   .5.1.x  radioHw           (read-write scalars -- physical radio config)
 *   .5.2.x  routing           (read-write scalars -- forwarding/duty-cycle/etc)
 *   .5.3.x  system            (read-write scalars -- identity/location/etc)
 *   .5.4.x  bridgeCfg         (read-write scalars -- bridge/MQTT config)
 *   .5.5.x  actions           (write-only trigger scalars; SET any nonzero
 *                              value to fire; reads back as 0 always)
 *
 *   .6.1.<col>.<index>   aclTable     (indexed table, 1..getAclCount())
 *   .7.1.<col>.<index>   regionTable  (indexed table, 0..getRegionCount()-1)
 *
 * Every scalar leaf is base.<group>.<leaf>.0 (a MIB-2-style scalar
 * instance). Table entries follow the standard SMIv2 convention of
 * base.<group>.1.<column>.<index> (the literal "1" is the conceptual row
 * -- "Entry" -- under the table object; there's only one row-type per table
 * here so it's always 1).
 *
 * DECIMAL VALUES: this MIB never multiplies a fractional value by a scale
 * factor to fit it into an INTEGER (no "x10"/"x100"/"x1000" leaves). Every
 * field that is inherently fractional in the firmware (frequency in MHz,
 * bandwidth in kHz, SNR in dB, duty-cycle/airtime percentages, lat/lon in
 * degrees, delay factors, the ADC multiplier) is instead an OCTET STRING
 * holding a plain decimal ASCII number, e.g. "869.525" or "4.25" or
 * "12.50". Reading one of these and parsing it with atof()/strtod() (or
 * just eyeballing it in snmpwalk output) gives you the real value
 * directly -- no scale factor to remember or get wrong. Only values that
 * are genuinely integers in the firmware (spreading factor, coding rate,
 * tx power in dBm, counters, flags, permissions, table indices, etc.) use
 * INTEGER/Counter32/Gauge32/TimeTicks.
 */

#ifndef SNMP_BASE_OID
  // iso(1).org(3).dod(6).internet(1).private(4).enterprises(1).62911.1
  #define SNMP_BASE_OID { 1, 3, 6, 1, 4, 1, 62911, 1 }
  #define SNMP_BASE_OID_LEN 8
#endif

// Top-level group numbers under the base arc.
#define SNMP_GRP_SYSINFO      1
#define SNMP_GRP_RADIO        2
#define SNMP_GRP_PACKETS      3
#define SNMP_GRP_BRIDGE       4
#define SNMP_GRP_CONTROL      5
#define SNMP_GRP_ACL_TABLE    6
#define SNMP_GRP_REGION_TABLE 7

// Sub-groups under SNMP_GRP_CONTROL (i.e. base.5.<subgroup>.<leaf>.0).
#define SNMP_CTRL_RADIO_HW    1
#define SNMP_CTRL_ROUTING     2
#define SNMP_CTRL_SYSTEM      3
#define SNMP_CTRL_BRIDGE      4
#define SNMP_CTRL_ACTIONS     5

// -- base.1 sysInfo -----------------------------------------------------------
#define SNMP_SYSINFO_NODE_NAME        1   // OCTET STRING
#define SNMP_SYSINFO_ROLE             2   // OCTET STRING ("repeater"/"room_server")
#define SNMP_SYSINFO_FW_VERSION       3   // OCTET STRING
#define SNMP_SYSINFO_PUBKEY_HEX       4   // OCTET STRING (64 hex chars)
#define SNMP_SYSINFO_UPTIME           5   // TimeTicks (centiseconds since boot)
#define SNMP_SYSINFO_BATT_MV          6   // Gauge32, millivolts
#define SNMP_SYSINFO_MAX              6

// -- base.2 radioStats ---------------------------------------------------------
#define SNMP_RADIO_TOTAL_AIRTIME_MS   1   // Counter32, ms, cumulative since boot
#define SNMP_RADIO_RX_AIRTIME_MS      2   // Counter32, ms, cumulative since boot
#define SNMP_RADIO_TX_BUDGET_MS       3   // Gauge32, ms, remaining duty-cycle TX budget right now
#define SNMP_RADIO_NOISE_FLOOR        4   // INTEGER, dBm
#define SNMP_RADIO_LAST_RSSI          5   // INTEGER, dBm
#define SNMP_RADIO_LAST_SNR           6   // OCTET STRING, decimal dB (e.g. "4.25") -- unscaled
#define SNMP_RADIO_DUTY_CYCLE_PCT     7   // OCTET STRING, decimal percent (e.g. "12.50") -- the firmware's CONFIGURED duty-cycle allowance, not live usage; see leaf 8 for that
#define SNMP_RADIO_AIRTIME_UTIL_PCT   8   // OCTET STRING, decimal percent 0.00-100.00. Live, self-measuring airtime utilization: (airtime consumed since this OID was last read) / (wall-clock time since this OID was last read) * 100. See SNMPAgent.cpp for the sampling logic. The FIRST read after boot/agent start has no prior sample to diff against and returns "0.00".
#define SNMP_RADIO_MAX                8

// -- base.3 packetStats ---------------------------------------------------------
#define SNMP_PKT_SENT_FLOOD           1   // Counter32
#define SNMP_PKT_SENT_DIRECT          2   // Counter32
#define SNMP_PKT_RECV_FLOOD           3   // Counter32
#define SNMP_PKT_RECV_DIRECT          4   // Counter32
#define SNMP_PKT_RADIO_RECV           5   // Counter32
#define SNMP_PKT_RADIO_SENT           6   // Counter32
#define SNMP_PKT_RADIO_RECV_ERRORS    7   // Counter32
#define SNMP_PKT_OUTBOUND_QUEUE_LEN   8   // Gauge32
#define SNMP_PKT_ERR_FLAGS            9   // Gauge32, bitmask -- see docs/SNMP_MIB.txt for bit meanings
#define SNMP_PKT_MAX                  9

// -- base.4 bridgeStats (present even with no bridge compiled in; reads as 0) --
#define SNMP_BRIDGE_TX_PACKETS        1   // Counter32
#define SNMP_BRIDGE_RX_PACKETS        2   // Counter32
#define SNMP_BRIDGE_RECONNECTS        3   // Counter32
#define SNMP_BRIDGE_MAX               3

#define SNMP_GRP_MAX                  4

// ============================================================================
// Control tree (read-write): base.5.<subgroup>.<leaf>.0
// ============================================================================

// -- base.5.1 radioHw ---------------------------------------------------------
#define SNMP_RHW_FREQ_MHZ              1   // OCTET STRING, decimal MHz (e.g. "869.525") -- unscaled
#define SNMP_RHW_BW_KHZ                2   // OCTET STRING, decimal kHz (e.g. "250.000") -- unscaled
#define SNMP_RHW_SF                    3   // INTEGER 5-12
#define SNMP_RHW_CR                    4   // INTEGER 5-8
#define SNMP_RHW_TX_POWER_DBM          5   // INTEGER 1-22
#define SNMP_RHW_RX_BOOSTED_GAIN       6   // INTEGER 0/1
#define SNMP_RHW_MAX                   6

// -- base.5.2 routing ----------------------------------------------------------
#define SNMP_RT_REPEAT_ENABLED         1   // INTEGER 0/1
#define SNMP_RT_DUTY_CYCLE_PCT         2   // INTEGER 1-100 (the CLI "dutycycle" whole-percent setting; not the same leaf as base.2.7's decimal read-back of the same allowance)
#define SNMP_RT_TX_DELAY_FACTOR        3   // OCTET STRING, decimal factor (e.g. "1.50") -- unscaled
#define SNMP_RT_DIRECT_TX_DELAY        4   // OCTET STRING, decimal factor -- unscaled
#define SNMP_RT_RX_DELAY_BASE          5   // OCTET STRING, decimal factor -- unscaled
#define SNMP_RT_FLOOD_MAX              6   // INTEGER 0-64
#define SNMP_RT_FLOOD_MAX_UNSCOPED     7   // INTEGER 0-64, or 255 = "track flood.max"
#define SNMP_RT_FLOOD_MAX_ADVERT       8   // INTEGER 0-64
#define SNMP_RT_PATH_HASH_MODE         9   // INTEGER 0-2
#define SNMP_RT_LOOP_DETECT            10  // INTEGER 0-3 (0=off, 1=minimal, 2=moderate, 3=strict)
#define SNMP_RT_INTERFERENCE_THRESH    11  // INTEGER
#define SNMP_RT_AGC_RESET_INTERVAL_S   12  // INTEGER seconds (rounded to a multiple of 4 by firmware)
#define SNMP_RT_MULTI_ACKS             13  // INTEGER 0/1
#define SNMP_RT_FLOOD_ADVERT_HRS       14  // INTEGER 0, or 3-168
#define SNMP_RT_ADVERT_INTERVAL_MINS   15  // INTEGER 0, or 60-240 (rounded to a multiple of 2 by firmware)
#define SNMP_RT_MAX                    15

// -- base.5.3 system -------------------------------------------------------------
#define SNMP_SYS_NODE_NAME             1   // OCTET STRING, max length varies by role (24/32 bytes)
#define SNMP_SYS_LAT                   2   // OCTET STRING, decimal degrees, signed (e.g. "51.500000") -- unscaled
#define SNMP_SYS_LON                   3   // OCTET STRING, decimal degrees, signed -- unscaled
#define SNMP_SYS_OWNER_INFO            4   // OCTET STRING ('|' = newline, same convention as the CLI)
#define SNMP_SYS_GUEST_PASSWORD        5   // OCTET STRING; GET returns "set"/"" (never the real value); SET writes a new password
#define SNMP_SYS_ADMIN_PASSWORD        6   // OCTET STRING; GET returns "set"/"" (the CLI has no get for this either); SET writes a new password
#define SNMP_SYS_ALLOW_READ_ONLY       7   // INTEGER 0/1 (room_server role; harmless no-op elsewhere)
#define SNMP_SYS_ADC_MULTIPLIER        8   // OCTET STRING, decimal multiplier (e.g. "1.702") -- unscaled; "0" = board default
#define SNMP_SYS_POWER_SAVING          9   // INTEGER 0/1 (reserved -- see docs/SNMP_MIB.txt)
#define SNMP_SYS_MAX                   9

// -- base.5.4 bridgeCfg -----------------------------------------------------------
#define SNMP_BR_ENABLED                1   // INTEGER 0/1
#define SNMP_BR_DELAY_MS               2   // INTEGER 0-10000
#define SNMP_BR_SOURCE                 3   // INTEGER 0=logTx, 1=logRx
#define SNMP_BR_MQTT_SERVER            4   // OCTET STRING
#define SNMP_BR_MQTT_PORT              5   // INTEGER 0-65535 (0 = compile-time default)
#define SNMP_BR_MQTT_TOPIC             6   // OCTET STRING
#define SNMP_BR_MQTT_USER              7   // OCTET STRING
#define SNMP_BR_MQTT_PASS              8   // OCTET STRING; GET returns "set"/"" (never the real value)
#define SNMP_BR_MQTT_AUTOSTART         9   // INTEGER 0/1
#define SNMP_BR_MAX                    9

// -- base.5.5 actions (write-only triggers; GET always returns 0) -----------------
#define SNMP_ACT_REBOOT                1   // write 1 -> reboot. Response is sent before rebooting, but may not arrive if the reboot is too fast.
#define SNMP_ACT_CLKREBOOT             2   // write 1 -> clkreboot. Same caveat as reboot.
#define SNMP_ACT_ERASE                 3   // write 1 -> factory erase. DESTRUCTIVE AND IRREVERSIBLE. Same caveat as reboot.
#define SNMP_ACT_SEND_ADVERT           4   // write 1 -> flood advert, write 2 -> zero-hop advert
#define SNMP_ACT_CLEAR_STATS           5   // write 1 -> clear stats
#define SNMP_ACT_MAX                   5

// ============================================================================
// aclTable: base.6.1.<column>.<index>, index = 1..getAclCount()
// ============================================================================
#define SNMP_ACL_COL_PUBKEY_HEX        2   // OCTET STRING, read-only (64 hex chars)
#define SNMP_ACL_COL_PERMISSIONS       3   // INTEGER 0-3, read-write (write = setperm). Bit 0 (1) = read, bit 1 (2) = write/admin -- see docs/SNMP_MIB.txt for the exact bit meanings used by this firmware.
#define SNMP_ACL_COL_LAST_ACTIVITY     4   // TimeTicks, read-only (centiseconds since boot; 0 = never)
#define SNMP_ACL_COL_MIN               2
#define SNMP_ACL_COL_MAX               4

// ============================================================================
// regionTable: base.7.1.<column>.<index>, index = 0..getRegionCount()-1
// (index matches RegionMap::getByIdx(i); index 0 is usually the wildcard '*')
// ============================================================================
#define SNMP_REGION_COL_ID             2   // INTEGER, RegionEntry.id (0 = wildcard), read-only
#define SNMP_REGION_COL_PARENT_ID      3   // INTEGER, RegionEntry.parent, read-only
#define SNMP_REGION_COL_NAME           4   // OCTET STRING, read-write (write = "region put <name>" under the SAME parent as row idx; does not rename row idx itself -- see SNMPDataSource.h)
#define SNMP_REGION_COL_FLOOD_DENIED   5   // INTEGER 0/1, read-write (write 1 = region denyf, write 0 = region allowf)
#define SNMP_REGION_COL_IS_HOME        6   // INTEGER 0/1, read-write (write 1 = region home <name>; write 0 is rejected, clearing home isn't supported by the firmware)
#define SNMP_REGION_COL_IS_DEFAULT     7   // INTEGER 0/1, read-write (write 1 = region default <name>; write 0 only accepted on the row that IS currently default, and clears the default to <null>)
#define SNMP_REGION_COL_MIN            2
#define SNMP_REGION_COL_MAX            7
