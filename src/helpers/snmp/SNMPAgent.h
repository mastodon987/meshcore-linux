#pragma once

#ifdef WITH_SNMP

#include "helpers/snmp/SNMPDataSource.h"
#include <stdint.h>
#include <stddef.h>

#ifndef SNMP_PORT
  #define SNMP_PORT 161
#endif

#ifndef SNMP_COMMUNITY
  #define SNMP_COMMUNITY "public"
#endif

#ifndef SNMP_RW_COMMUNITY
  #define SNMP_RW_COMMUNITY SNMP_COMMUNITY
#endif

// Only v2c is implemented (GETBULK, 32-bit Counter32/Gauge32, the v2c
// error-status codes). SNMP_VERSION is kept as a build flag for forward
// compatibility / documentation purposes, but values other than 2 currently
// just fall back to the same v2c behaviour with version field == 0 (v1) in
// outgoing PDUs where that matters.
#ifndef SNMP_VERSION
  #define SNMP_VERSION 2
#endif

#ifndef SNMP_MAX_PACKET
  #define SNMP_MAX_PACKET 1024
#endif

/**
 * @brief Minimal read/write SNMPv2c agent over a non-blocking UDP socket.
 *
 * Implements just enough of RFC 1157 / RFC 1905 BER encoding to serve
 * GetRequest, GetNextRequest, GetBulkRequest, and SetRequest against the
 * MIB described in SNMPOids.h: read-only stats (groups 1-4, from
 * SNMPDataSource), plus a read/write control tree and two indexed tables
 * (groups 5-7, from the optional SNMPControlSource). There is no SNMPv3
 * (no encryption/per-user auth) -- treat SNMP_COMMUNITY like a weak shared
 * secret, not real auth, and only expose the UDP port on networks you
 * trust (e.g. LAN/VPN, not the public internet), especially once SET is
 * enabled, since base.5.5 (actions) includes reboot/erase.
 *
 * Designed for the Linux/ArduLinux build: uses raw POSIX UDP sockets
 * (no Arduino UDP class is available on this target), and is driven from
 * the existing main loop() via a non-blocking poll() in loop() -- there is
 * no separate thread.
 *
 * Required build defines:
 *   WITH_SNMP=1
 *
 * Optional build defines:
 *   SNMP_PORT=161            (default 161 -- needs root or
 *                             `setcap cap_net_bind_service+ep` on Linux to
 *                             bind without root; use e.g. 1161 otherwise)
 *   SNMP_COMMUNITY="public"  (default "public" -- used for BOTH read and
 *                             write access if SNMP_RW_COMMUNITY isn't set)
 *   SNMP_RW_COMMUNITY        (optional separate, harder-to-guess community
 *                             required for SetRequest; if unset,
 *                             SNMP_COMMUNITY is accepted for SET too)
 *   SNMP_VERSION=2           (documentation only at present; v2c semantics
 *                             are always used)
 *   SNMP_BASE_OID            (see SNMPOids.h -- defaults to the experimental
 *                             arc 1.3.6.1.3.7491.1)
 *
 * Runtime: none of the above are currently exposed via the CLI `set`/`get`
 * commands -- SNMP is intended as an always-on, locally-trusted monitoring
 * +control endpoint configured once at build/deploy time, mirroring how
 * WITH_MQTT_BRIDGE_SERVER et al. work as build-time defaults.
 */
class SNMPAgent {
public:
  /** control may be nullptr to run read-only (groups 1-4 only; SET and groups 5-7 GETs return errors/empty). */
  SNMPAgent(SNMPDataSource* source, SNMPControlSource* control = nullptr)
    : _source(source), _control(control) {}

  /** Opens the UDP socket and starts listening. Safe to call once at startup. */
  void begin();

  /** Closes the UDP socket. */
  void end();

  /** Non-blocking: polls the socket for at most one pending datagram per call. */
  void loop();

  bool isRunning() const { return _sock >= 0; }

  /**
   * Computes the airtime-utilization OID's value and formats it as a
   * "0.00"-"100.00" decimal string into 'out' (out_cap >= 8 recommended).
   * Public (despite mostly being agent-internal plumbing) because it's
   * called from SNMPAgent.cpp's free functions in response to a GET/
   * GETNEXT/GETBULK read of SNMP_RADIO_AIRTIME_UTIL_PCT, which aren't
   * members of this class. NOT idempotent: it updates the rolling sample
   * as a side effect, so call it exactly once per read of that OID, never
   * speculatively, or utilization readings will be thrown off.
   */
  void sampleAirtimeUtilizationPct(char* out, size_t out_cap);

private:
  SNMPDataSource* _source;
  SNMPControlSource* _control;
  int _sock = -1;

  uint8_t _rx_buf[SNMP_MAX_PACKET];
  uint8_t _tx_buf[SNMP_MAX_PACKET];

  // Airtime-utilization sampling state (see SNMP_RADIO_AIRTIME_UTIL_PCT in
  // SNMPOids.h): each time that OID is read, we diff against the previous
  // (wallclock_ms, total_airtime_ms) sample taken at the previous read of
  // it, then overwrite the sample with the current values. There is
  // intentionally no fixed averaging window -- the ratio is exactly
  // "airtime consumed since you last asked, divided by how long ago you
  // last asked" -- so a poller that asks once a minute gets a 1-minute
  // average, and one that asks once a second gets a 1-second average.
  bool _util_has_sample = false;
  uint32_t _util_last_wallclock_ms = 0;
  uint32_t _util_last_airtime_ms = 0;

  void handleDatagram(const uint8_t* data, size_t len, const void* from_addr, size_t from_addr_len);
};

#endif // WITH_SNMP
