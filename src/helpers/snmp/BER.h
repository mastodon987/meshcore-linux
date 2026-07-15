#pragma once

#ifdef WITH_SNMP

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Minimal BER (Basic Encoding Rules) reader/writer -- just the subset
 *        needed to speak SNMPv1/v2c: definite-length, primitive INTEGER /
 *        OCTET STRING / NULL / OBJECT IDENTIFIER, and constructed SEQUENCE,
 *        plus the SNMP-specific application tags (Counter32, Gauge32,
 *        TimeTicks) and context tags (the PDU types).
 *
 * Not a general-purpose ASN.1 library: no support for indefinite lengths,
 * BIT STRING, REAL, tags > 30 (no multi-byte tag numbers), or OID
 * sub-identifiers that don't fit in 32 bits. All of that is outside what
 * SNMPv1/v2c PDUs ever use in practice.
 */
namespace ber {

// ASN.1 universal tags
constexpr uint8_t TAG_INTEGER      = 0x02;
constexpr uint8_t TAG_OCTET_STRING = 0x04;
constexpr uint8_t TAG_NULL         = 0x05;
constexpr uint8_t TAG_OID          = 0x06;
constexpr uint8_t TAG_SEQUENCE     = 0x30;

// SNMP application-class tags (RFC 1155 / RFC 1902), class bits 0x40 | tag
constexpr uint8_t TAG_IPADDRESS    = 0x40;
constexpr uint8_t TAG_COUNTER32    = 0x41;
constexpr uint8_t TAG_GAUGE32      = 0x42;
constexpr uint8_t TAG_TIMETICKS    = 0x43;
constexpr uint8_t TAG_OPAQUE       = 0x44;
constexpr uint8_t TAG_COUNTER64    = 0x46;

// SNMP context-class tags for PDU types (class bits 0xA0 | pdu type)
constexpr uint8_t PDU_GET_REQUEST      = 0xA0;
constexpr uint8_t PDU_GET_NEXT_REQUEST = 0xA1;
constexpr uint8_t PDU_GET_RESPONSE     = 0xA2;
constexpr uint8_t PDU_SET_REQUEST      = 0xA3;
constexpr uint8_t PDU_GET_BULK_REQUEST = 0xA5; // v2c only

// v2c error-status values (RFC 1905 §3)
constexpr int ERR_NO_ERROR             = 0;
constexpr int ERR_TOO_BIG              = 1;
constexpr int ERR_NO_SUCH_NAME         = 2; // v1; v2c uses noSuchObject/noSuchInstance varbind instead
constexpr int ERR_BAD_VALUE            = 3;
constexpr int ERR_GEN_ERR              = 5;
constexpr int ERR_NO_ACCESS            = 6;
constexpr int ERR_WRONG_TYPE           = 7;
constexpr int ERR_WRONG_LENGTH         = 8;
constexpr int ERR_WRONG_VALUE          = 9;
constexpr int ERR_NOT_WRITABLE         = 17;
constexpr int ERR_INCONSISTENT_VALUE   = 12;

// exception values used in place of a varbind's value in GETNEXT/GETBULK
// responses when walking off the end of the MIB (RFC 1905 §3, context tags)
constexpr uint8_t EXC_NO_SUCH_OBJECT   = 0x80;
constexpr uint8_t EXC_NO_SUCH_INSTANCE = 0x81;
constexpr uint8_t EXC_END_OF_MIB_VIEW  = 0x82;

constexpr size_t MAX_OID_LEN = 24; // sub-identifiers; plenty for our shallow tree

/**
 * @brief Decoded value of an incoming varbind, used for SET requests where
 * the value tag (INTEGER vs OCTET STRING) is whatever the manager sent.
 * 'str_value' is also used to hold the original raw bytes for diagnostics
 * when kind == Unsupported.
 */
struct AnyValue {
  enum class Kind { Integer, OctetString, Null, Unsupported } kind = Kind::Unsupported;
  int64_t int_value = 0;
  char str_value[192] = {0};
  size_t str_len = 0;
};

struct Oid {
  uint32_t ids[MAX_OID_LEN];
  size_t len = 0;

  bool operator==(const Oid& other) const {
    if (len != other.len) return false;
    for (size_t i = 0; i < len; i++) if (ids[i] != other.ids[i]) return false;
    return true;
  }

  // lexicographic compare, like real SNMP OID ordering
  int compare(const Oid& other) const {
    size_t n = len < other.len ? len : other.len;
    for (size_t i = 0; i < n; i++) {
      if (ids[i] != other.ids[i]) return ids[i] < other.ids[i] ? -1 : 1;
    }
    if (len == other.len) return 0;
    return len < other.len ? -1 : 1;
  }

  bool startsWith(const Oid& prefix) const {
    if (prefix.len > len) return false;
    for (size_t i = 0; i < prefix.len; i++) if (ids[i] != prefix.ids[i]) return false;
    return true;
  }
};

/**
 * @brief Cursor over a read-only byte buffer, used while decoding an
 *        incoming SNMP message. All read*() methods return false (leaving
 *        *this unmodified other than internal bounds bookkeeping) on
 *        malformed/truncated input, so callers can bail out cleanly.
 */
class Reader {
public:
  Reader(const uint8_t* data, size_t len) : _data(data), _len(len), _pos(0) {}

  bool atEnd() const { return _pos >= _len; }
  size_t remaining() const { return _len - _pos; }
  size_t pos() const { return _pos; }

  bool readByte(uint8_t* out) {
    if (_pos >= _len) return false;
    *out = _data[_pos++];
    return true;
  }

  bool peekByte(uint8_t* out) const {
    if (_pos >= _len) return false;
    *out = _data[_pos];
    return true;
  }

  /** Reads a BER definite-length field (short or long form, up to 4 length-octets). */
  bool readLength(size_t* out_len) {
    uint8_t b;
    if (!readByte(&b)) return false;
    if ((b & 0x80) == 0) { // short form
      *out_len = b;
      return true;
    }
    uint8_t n_octets = b & 0x7F;
    if (n_octets == 0 || n_octets > 4) return false; // indefinite or absurd length, reject
    uint32_t len = 0;
    for (uint8_t i = 0; i < n_octets; i++) {
      uint8_t lb;
      if (!readByte(&lb)) return false;
      len = (len << 8) | lb;
    }
    *out_len = len;
    return true;
  }

  /** Reads tag + length, leaves cursor at the start of the value; value not bounds-checked here. */
  bool readTagAndLength(uint8_t* out_tag, size_t* out_len) {
    if (!readByte(out_tag)) return false;
    if (!readLength(out_len)) return false;
    if (*out_len > remaining()) return false; // truncated
    return true;
  }

  /** Reads a TLV whose tag must exactly match 'expect_tag'. */
  bool expectTagAndLength(uint8_t expect_tag, size_t* out_len) {
    size_t save = _pos;
    uint8_t tag;
    if (!readTagAndLength(&tag, out_len)) { _pos = save; return false; }
    if (tag != expect_tag) { _pos = save; return false; }
    return true;
  }

  bool skip(size_t n) {
    if (n > remaining()) return false;
    _pos += n;
    return true;
  }

  /** Decodes a BER INTEGER value (signed, big-endian two's complement, value already past tag+len). */
  bool readIntegerValue(size_t len, int64_t* out) {
    if (len == 0 || len > 8 || len > remaining()) return false;
    int64_t v = (_data[_pos] & 0x80) ? -1 : 0; // sign-extend from the leading byte
    for (size_t i = 0; i < len; i++) v = (v << 8) | _data[_pos + i];
    _pos += len;
    *out = v;
    return true;
  }

  /** Decodes a full INTEGER TLV (tag must be TAG_INTEGER). */
  bool readInteger(int64_t* out) {
    size_t len;
    if (!expectTagAndLength(TAG_INTEGER, &len)) return false;
    return readIntegerValue(len, out);
  }

  /** Decodes an OCTET STRING TLV into caller's buffer (truncates if too small; null-terminates if room). */
  bool readOctetString(char* out, size_t out_cap, size_t* out_len) {
    size_t len;
    if (!expectTagAndLength(TAG_OCTET_STRING, &len)) return false;
    size_t n = len < out_cap ? len : out_cap;
    for (size_t i = 0; i < n; i++) out[i] = (char)_data[_pos + i];
    if (n < out_cap) out[n] = 0;
    _pos += len;
    if (out_len) *out_len = len;
    return true;
  }

  /** Decodes an OBJECT IDENTIFIER TLV. */
  bool readOid(Oid* out) {
    size_t len;
    if (!expectTagAndLength(TAG_OID, &len)) return false;
    if (len == 0) return false;
    size_t end = _pos + len;
    out->len = 0;

    uint8_t first = _data[_pos++];
    if (out->len < MAX_OID_LEN) out->ids[out->len++] = first / 40;
    if (out->len < MAX_OID_LEN) out->ids[out->len++] = first % 40;

    while (_pos < end) {
      uint32_t v = 0;
      bool more;
      do {
        if (_pos >= end) return false;
        uint8_t b = _data[_pos++];
        v = (v << 7) | (b & 0x7F);
        more = (b & 0x80) != 0;
      } while (more);
      if (out->len < MAX_OID_LEN) out->ids[out->len++] = v;
      // if the OID is longer than MAX_OID_LEN we simply stop recording
      // further sub-identifiers; it cannot match anything in our shallow
      // MIB so over-length OIDs will correctly fall through to
      // noSuchObject/endOfMibView regardless.
    }
    return true;
  }

  /**
   * Decodes whatever value TLV comes next (INTEGER or OCTET STRING; NULL
   * recognized but carries no payload) without knowing its tag ahead of
   * time -- used for SET requests, where the manager chooses the encoding.
   * Any other tag is captured as Kind::Unsupported (caller should reject
   * with wrongType).
   */
  bool readAnyValue(AnyValue* out) {
    size_t save = _pos;
    uint8_t tag;
    size_t len;
    if (!readTagAndLength(&tag, &len)) { _pos = save; return false; }

    if (tag == TAG_INTEGER) {
      out->kind = AnyValue::Kind::Integer;
      return readIntegerValue(len, &out->int_value);
    }
    if (tag == TAG_OCTET_STRING) {
      out->kind = AnyValue::Kind::OctetString;
      size_t n = len < sizeof(out->str_value) - 1 ? len : sizeof(out->str_value) - 1;
      for (size_t i = 0; i < n; i++) out->str_value[i] = (char)_data[_pos + i];
      out->str_value[n] = 0;
      out->str_len = len; // true length, even if truncated in str_value
      return skip(len);
    }
    if (tag == TAG_NULL) {
      out->kind = AnyValue::Kind::Null;
      return skip(len);
    }
    out->kind = AnyValue::Kind::Unsupported;
    return skip(len);
  }

  /** Reads tag+length for a SEQUENCE and returns the length (does not consume the body). */
  bool enterSequence(size_t* out_len) {
    return expectTagAndLength(TAG_SEQUENCE, out_len);
  }

  /** Reads tag+length for any context-class constructed tag (used for PDU framing); returns the tag seen. */
  bool enterAnyConstructed(uint8_t* out_tag, size_t* out_len) {
    return readTagAndLength(out_tag, out_len);
  }

private:
  const uint8_t* _data;
  size_t _len;
  size_t _pos;
};

/**
 * @brief Append-only writer building a BER message into a fixed-size buffer.
 *
 * To avoid a second length-prefixing pass, callers build inner-to-outer:
 * write a TLV's *value* bytes first via the raw write*() helpers into a
 * scratch buffer, then wrap with writeTLV(tag, ...). For SEQUENCEs (whose
 * content is itself a sequence of TLVs of unknown total size up front) we
 * use a small helper, Writer::Marker, that reserves room for the length
 * octets, lets the caller write the body, then backpatches the length --
 * see beginSequence()/endSequence().
 */
class Writer {
public:
  Writer(uint8_t* buf, size_t cap) : _buf(buf), _cap(cap), _len(0) {}

  size_t length() const { return _len; }
  const uint8_t* data() const { return _buf; }

  bool writeRaw(const uint8_t* bytes, size_t n) {
    if (_len + n > _cap) return false;
    memcpy_(bytes, n);
    return true;
  }

  bool writeByte(uint8_t b) {
    if (_len + 1 > _cap) return false;
    _buf[_len++] = b;
    return true;
  }

  /** Writes a BER length field (short or long form, up to 4 octets). */
  bool writeLength(size_t len) {
    if (len < 0x80) return writeByte((uint8_t)len);
    uint8_t tmp[4];
    int n = 0;
    size_t v = len;
    do { tmp[n++] = (uint8_t)(v & 0xFF); v >>= 8; } while (v != 0);
    if (!writeByte((uint8_t)(0x80 | n))) return false;
    for (int i = n - 1; i >= 0; i--) if (!writeByte(tmp[i])) return false;
    return true;
  }

  bool writeTagAndLength(uint8_t tag, size_t len) {
    return writeByte(tag) && writeLength(len);
  }

  /** Encodes a signed integer in minimal big-endian two's-complement form, with the given outer tag. */
  bool writeIntegerTagged(uint8_t tag, int64_t value) {
    uint8_t bytes[8];
    int n = 1;
    bytes[0] = (uint8_t)(value & 0xFF);
    int64_t rem = value >> 8;
    // emit at least 1 byte; keep emitting while sign bits aren't fully
    // represented by what we already have
    while (true) {
      uint8_t top = bytes[n - 1];
      bool sign_extended = (rem == 0 && !(top & 0x80)) || (rem == -1 && (top & 0x80));
      if (sign_extended) break;
      if (n >= 8) break;
      bytes[n] = (uint8_t)(rem & 0xFF);
      rem >>= 8;
      n++;
    }
    if (!writeTagAndLength(tag, n)) return false;
    for (int i = n - 1; i >= 0; i--) if (!writeByte(bytes[i])) return false;
    return true;
  }

  bool writeInteger(int64_t value) { return writeIntegerTagged(TAG_INTEGER, value); }

  /** Encodes an *unsigned* 32-bit value (Counter32/Gauge32/TimeTicks): always non-negative,
   *  needs a leading 0x00 pad byte if the top bit of the value is set. */
  bool writeUnsigned32Tagged(uint8_t tag, uint32_t value) {
    // Always start from the full 4-byte big-endian form, trim redundant
    // leading 0x00 bytes, then re-add a single 0x00 pad byte if the
    // remaining leading bit is 1 (so it isn't misread as negative).
    uint8_t full[4] = {
      (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value
    };
    int start = 0;
    while (start < 3 && full[start] == 0x00 && !(full[start + 1] & 0x80)) start++;
    int len = 4 - start;
    bool need_pad = (full[start] & 0x80) != 0;
    if (!writeTagAndLength(tag, len + (need_pad ? 1 : 0))) return false;
    if (need_pad && !writeByte(0x00)) return false;
    for (int i = start; i < 4; i++) if (!writeByte(full[i])) return false;
    return true;
  }

  bool writeOctetString(const char* str, size_t len) {
    if (!writeTagAndLength(TAG_OCTET_STRING, len)) return false;
    return writeRaw((const uint8_t*)str, len);
  }

  bool writeOctetStringCStr(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return writeOctetString(str, len);
  }

  bool writeNull() { return writeTagAndLength(TAG_NULL, 0); }

  bool writeExceptionValue(uint8_t exc_tag) { return writeTagAndLength(exc_tag, 0); }

  bool writeOid(const Oid& oid) {
    if (oid.len < 2) return false;
    // encode the body into a small scratch area first so we know its length
    uint8_t body[64];
    size_t blen = 0;
    auto put7 = [&](uint32_t v) -> bool {
      uint8_t stack[5];
      int n = 0;
      do { stack[n++] = (uint8_t)(v & 0x7F); v >>= 7; } while (v != 0);
      for (int i = n - 1; i >= 0; i--) {
        if (blen >= sizeof(body)) return false;
        uint8_t b = stack[i];
        if (i != 0) b |= 0x80;
        body[blen++] = b;
      }
      return true;
    };
    if (blen >= sizeof(body)) return false;
    body[blen++] = (uint8_t)(oid.ids[0] * 40 + oid.ids[1]);
    for (size_t i = 2; i < oid.len; i++) {
      if (!put7(oid.ids[i])) return false;
    }
    if (!writeTagAndLength(TAG_OID, blen)) return false;
    return writeRaw(body, blen);
  }

  /** Marker for backpatching a length field once the body size is known. */
  struct Marker { size_t tag_pos; size_t len_pos_reserved; };

  /**
   * Reserves space for tag + a worst-case 4-octet long-form length, writes
   * a placeholder, and returns a Marker to later fix up with endSequence().
   * We always reserve the long form up front (rather than re-flowing the
   * buffer) to keep this writer simple/allocation-free.
   */
  bool beginConstructed(uint8_t tag, Marker* marker) {
    marker->tag_pos = _len;
    if (!writeByte(tag)) return false;
    marker->len_pos_reserved = _len;
    // reserve 5 bytes: 0x84 + 4 length octets (covers up to 4GB, way more
    // than SNMP_MAX_PACKET will ever need, but keeps the math simple)
    for (int i = 0; i < 5; i++) if (!writeByte(0)) return false;
    return true;
  }

  bool endConstructed(const Marker& marker) {
    size_t body_len = _len - (marker.len_pos_reserved + 5);
    _buf[marker.len_pos_reserved] = 0x84;
    _buf[marker.len_pos_reserved + 1] = (uint8_t)(body_len >> 24);
    _buf[marker.len_pos_reserved + 2] = (uint8_t)(body_len >> 16);
    _buf[marker.len_pos_reserved + 3] = (uint8_t)(body_len >> 8);
    _buf[marker.len_pos_reserved + 4] = (uint8_t)body_len;
    return true;
  }

private:
  uint8_t* _buf;
  size_t _cap;
  size_t _len;

  void memcpy_(const uint8_t* src, size_t n) {
    for (size_t i = 0; i < n; i++) _buf[_len + i] = src[i];
    _len += n;
  }
};

} // namespace ber

#endif // WITH_SNMP
