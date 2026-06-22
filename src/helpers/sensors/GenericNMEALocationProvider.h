#pragma once

#include "LocationProvider.h"
#include <RTClib.h>
#include <Stream.h>
#include <string.h>
#include <stdlib.h>

// ─────────────────────────────────────────────────────────────────────────
// GenericNMEALocationProvider
//
// Minimal, dependency-free NMEA-0183 parser written to replace MicroNMEA
// for GPS modules that emit multi-constellation talker IDs.
//
// Why this exists: MicroNMEA only recognizes a fixed set of talker IDs
// (historically just "GP"), so a module configured for combined
// GPS+GLONASS+BeiDou output -- which emits $GNGGA / $GNRMC / $GNGSA /
// $GNVTG / $GNZDA / $GNGLL / $BDGSV instead of $GPxxx -- is silently
// ignored sentence-by-sentence: MicroNMEA's internal state never updates,
// isValid() stays false, and getNumSatellites() stays 0, even though the
// module is streaming valid fix data the whole time. That exactly matches
// "on, active, no fix, 0 sats" while raw GNGGA/GNRMC are visible on the
// wire.
//
// This parser only looks at the 3-character sentence ID after the talker
// prefix (i.e. the last 3 chars of the 5-char header: "GGA", "RMC", ...)
// and ignores the first two (talker) characters entirely -- so GP, GN,
// GL, GA, GB, and BD all work identically. GSA/GSV/VTG/ZDA/GLL are not
// needed for a fix and are skipped; GGA + RMC alone provide fix quality,
// satellite count, lat/lon, altitude, and UTC date+time.
// ─────────────────────────────────────────────────────────────────────────
class GenericNMEALocationProvider : public LocationProvider {
  Stream* _ser;
  mesh::RTCClock* _clock;
  unsigned long _last_time_sync = 0;
  static const unsigned long TIME_SYNC_INTERVAL = 1800000; // 30 min

  // line assembly buffer
  char _line[120];
  uint8_t _line_len = 0;

  // parsed state
  bool   _has_fix = false;
  int    _fix_quality = 0;   // GGA field 6: 0 = no fix
  int    _num_sats = 0;
  long   _lat = 0;            // 1e-6 degrees, matches MicroNMEA convention
  long   _lon = 0;            // 1e-6 degrees
  long   _alt_mm = 0;         // millimetres (matches MicroNMEA getAltitude semantics: caller divides by 1000)
  bool   _have_date = false, _have_time = false;
  int    _year = 1970, _month = 1, _day = 1, _hour = 0, _minute = 0, _second = 0;

  long  next_check = 0;
  long  time_valid = 0;
  bool  _enabled = true;

  static bool checksumOk(const char* s) {
    // s points at '$'; checksum is after '*' as 2 hex chars
    const char* star = strchr(s, '*');
    if (!star) return false;
    uint8_t cks = 0;
    for (const char* p = s + 1; p < star; p++) cks ^= (uint8_t)*p;
    uint8_t given = (uint8_t)strtol(star + 1, nullptr, 16);
    return cks == given;
  }

  // Returns pointer to the 3-letter sentence type (e.g. "GGA") regardless
  // of the 2-letter talker ID, or nullptr if the line isn't a recognized
  // NMEA sentence header.
  static const char* sentenceType(const char* s) {
    if (s[0] != '$' || strlen(s) < 6) return nullptr;
    return s + 3; // skip '$' + 2 talker chars
  }

  // Splits the comma-delimited fields of an NMEA sentence in-place.
  // Returns the number of fields found; fields[] are pointers into buf.
  static int splitFields(char* buf, char* fields[], int max_fields) {
    int n = 0;
    char* p = buf;
    fields[n++] = p;
    while (*p && n < max_fields) {
      if (*p == ',' || *p == '*') {
        *p = '\0';
        p++;
        fields[n++] = p;
      } else {
        p++;
      }
    }
    return n;
  }

  static double nmeaCoordToDeg(const char* field, char hemi) {
    if (!field || field[0] == '\0') return 0.0;
    double raw = atof(field);
    int deg = (int)(raw / 100.0);
    double minutes = raw - (deg * 100.0);
    double dec = deg + minutes / 60.0;
    if (hemi == 'S' || hemi == 'W') dec = -dec;
    return dec;
  }

  void parseGGA(char* body) {
    // GGA fields after sentence id: time,lat,N/S,lon,E/W,fixQual,numSV,HDOP,alt,M,...
    char* f[16];
    int n = splitFields(body, f, 16);
    if (n < 10) return;

    _fix_quality = atoi(f[5]);
    _num_sats    = atoi(f[6]);

    if (_fix_quality > 0) {
      double lat = nmeaCoordToDeg(f[1], f[2][0]);
      double lon = nmeaCoordToDeg(f[3], f[4][0]);
      _lat = (long)(lat * 1000000.0);
      _lon = (long)(lon * 1000000.0);
      _alt_mm = (long)(atof(f[8]) * 1000.0);
      _has_fix = true;
    } else {
      _has_fix = false;
    }

    // time field (hhmmss.ss) — keep in sync with RMC if RMC hasn't run yet
    if (strlen(f[0]) >= 6) {
      char tmp[3] = {0};
      tmp[0] = f[0][0]; tmp[1] = f[0][1]; _hour   = atoi(tmp);
      tmp[0] = f[0][2]; tmp[1] = f[0][3]; _minute = atoi(tmp);
      tmp[0] = f[0][4]; tmp[1] = f[0][5]; _second = atoi(tmp);
      _have_time = true;
    }
  }

  void parseRMC(char* body) {
    // RMC fields: time,status(A/V),lat,N/S,lon,E/W,speed,course,date,...
    char* f[16];
    int n = splitFields(body, f, 16);
    if (n < 9) return;

    bool active = (f[1][0] == 'A');
    if (active) {
      double lat = nmeaCoordToDeg(f[2], f[3][0]);
      double lon = nmeaCoordToDeg(f[4], f[5][0]);
      _lat = (long)(lat * 1000000.0);
      _lon = (long)(lon * 1000000.0);
    }

    // date field ddmmyy
    if (strlen(f[8]) >= 6) {
      char tmp[3] = {0};
      tmp[0] = f[8][0]; tmp[1] = f[8][1]; _day   = atoi(tmp);
      tmp[0] = f[8][2]; tmp[1] = f[8][3]; _month = atoi(tmp);
      tmp[0] = f[8][4]; tmp[1] = f[8][5]; _year  = 2000 + atoi(tmp);
      _have_date = true;
    }
    if (strlen(f[0]) >= 6) {
      char tmp[3] = {0};
      tmp[0] = f[0][0]; tmp[1] = f[0][1]; _hour   = atoi(tmp);
      tmp[0] = f[0][2]; tmp[1] = f[0][3]; _minute = atoi(tmp);
      tmp[0] = f[0][4]; tmp[1] = f[0][5]; _second = atoi(tmp);
      _have_time = true;
    }
  }

  void processLine(char* line) {
    if (!checksumOk(line)) return;
    const char* type = sentenceType(line);
    if (!type) return;

    // body starts right after the 5-char header ($ + talker[2] + type[3])
    char* body = line + 6; // points just past header, at the comma (or '\0')
    if (*body == ',') body++;

    if (strncmp(type, "GGA", 3) == 0) {
      parseGGA(body);
    } else if (strncmp(type, "RMC", 3) == 0) {
      parseRMC(body);
    }
    // GSA/GSV/VTG/ZDA/GLL intentionally ignored — not needed for a fix.
  }

public:
  GenericNMEALocationProvider(Stream& ser, mesh::RTCClock* clock = nullptr)
    : _ser(&ser), _clock(clock) {}

  void begin() override { _enabled = true; }
  void reset() override { _has_fix = false; _num_sats = 0; _line_len = 0; }
  void stop() override { _enabled = false; }
  bool isEnabled() override { return _enabled; }

  void syncTime() override { LocationProvider::syncTime(); }

  long getLatitude() override { return _lat; }
  long getLongitude() override { return _lon; }
  long getAltitude() override { return _alt_mm; }
  long satellitesCount() override { return _num_sats; }
  bool isValid() override { return _has_fix && _fix_quality > 0; }

  long getTimestamp() override {
    if (!_have_date || !_have_time) return 0;
    DateTime dt(_year, _month, _day, _hour, _minute, _second);
    return dt.unixtime();
  }

  void sendSentence(const char* sentence) override {
    _ser->print(sentence);
    _ser->print("\r\n");
  }

  void loop() override {
    if (!_enabled) return;

    while (_ser->available()) {
      char c = _ser->read();
      #ifdef GPS_NMEA_DEBUG
      Serial.print(c);
      #endif

      if (c == '\n') {
        // ignore stray leading \r already trimmed below
        continue;
      }
      if (c == '\r') {
        if (_line_len > 0) {
          _line[_line_len] = '\0';
          processLine(_line);
          _line_len = 0;
        }
        continue;
      }
      if (_line_len < sizeof(_line) - 1) {
        _line[_line_len++] = c;
      } else {
        // overflow — drop this malformed/oversized line
        _line_len = 0;
      }
    }

    if (!isValid()) time_valid = 0;

    if (millis() > next_check) {
      next_check = millis() + 1000;
      if (!_time_sync_needed && _clock != nullptr && (millis() - _last_time_sync) > TIME_SYNC_INTERVAL) {
        _time_sync_needed = true;
      }
      if (_time_sync_needed && time_valid > 2) {
        if (_clock != nullptr && _have_date && _have_time) {
          _clock->setCurrentTime(getTimestamp());
          _time_sync_needed = false;
          _last_time_sync = millis();
        }
      }
      if (isValid()) time_valid++;
    }
  }
};
