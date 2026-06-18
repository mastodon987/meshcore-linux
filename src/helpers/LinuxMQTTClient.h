#pragma once

#if defined(ARDULINUX_PLATFORM) || defined(WITH_MQTT_BRIDGE)

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <Arduino.h> // for millis()

/**
 * @brief Minimal MQTT 3.1.1 client implemented over a raw POSIX TCP socket.
 *
 * Designed for the Linux/Ethernet repeater build, which has no Arduino
 * WiFiClient / PubSubClient stack available. This class talks directly to
 * the kernel's BSD socket API over the host's existing wired (or any other
 * already-configured) network interface — no WiFi association step needed.
 *
 * Implements just enough of the protocol for the MQTT bridge use case:
 *   - CONNECT (with optional username/password)
 *   - CONNACK parsing
 *   - PUBLISH (QoS 0, outbound)
 *   - SUBSCRIBE (QoS 0)
 *   - Inbound PUBLISH parsing (QoS 0)
 *   - PINGREQ / PINGRESP keepalive
 *   - DISCONNECT
 *
 * No TLS support — point this at a plain MQTT listener (e.g. mosquitto on
 * port 1883) or terminate TLS with a local stunnel/socat if needed.
 */
class LinuxMQTTClient {
public:
  // Callback signature mirrors PubSubClient's: topic, payload, length
  typedef void (*MessageCallback)(char* topic, uint8_t* payload, unsigned int length);

  LinuxMQTTClient() : _sock(-1), _callback(nullptr), _keepalive(60), _last_activity(0) {
    _rx_buf = nullptr;
    _rx_buf_size = 0;
  }

  ~LinuxMQTTClient() {
    disconnect();
    if (_rx_buf) free(_rx_buf);
  }

  /** Must be called before connect(). Buffer must be large enough for the
   *  biggest PUBLISH (payload + MQTT fixed/variable header overhead). */
  void setBufferSize(size_t size) {
    if (_rx_buf) free(_rx_buf);
    _rx_buf_size = size;
    _rx_buf = (uint8_t*)malloc(size);
  }

  void setKeepAlive(uint16_t seconds) { _keepalive = seconds; }

  void setCallback(MessageCallback cb) { _callback = cb; }

  bool connected() {
    return _sock >= 0;
  }

  /**
   * Open a TCP connection to host:port and perform the MQTT CONNECT
   * handshake. user/pass may be empty strings for anonymous auth.
   */
  bool connect(const char* host, uint16_t port, const char* client_id,
                const char* user, const char* pass) {
    if (!openSocket(host, port)) return false;

    uint8_t buf[256];
    size_t pos = 0;

    // Variable header
    buf[pos++] = 0x00; buf[pos++] = 0x04; // protocol name length
    buf[pos++] = 'M'; buf[pos++] = 'Q'; buf[pos++] = 'T'; buf[pos++] = 'T';
    buf[pos++] = 0x04; // protocol level (3.1.1)

    uint8_t connect_flags = 0x02; // clean session
    if (user && user[0]) {
      connect_flags |= 0x80; // username flag
      if (pass && pass[0]) connect_flags |= 0x40; // password flag
    }
    buf[pos++] = connect_flags;

    buf[pos++] = (uint8_t)(_keepalive >> 8);
    buf[pos++] = (uint8_t)(_keepalive & 0xFF);

    // Payload: client id, [username], [password] — each length-prefixed
    size_t hdr_len = pos;
    uint8_t payload[256];
    size_t ppos = 0;
    ppos += writeUTF8(payload + ppos, client_id);
    if (connect_flags & 0x80) ppos += writeUTF8(payload + ppos, user);
    if (connect_flags & 0x40) ppos += writeUTF8(payload + ppos, pass);

    uint32_t remaining = hdr_len + ppos;

    uint8_t packet[300];
    size_t plen = 0;
    packet[plen++] = 0x10; // CONNECT
    plen += encodeRemainingLength(packet + plen, remaining);
    memcpy(packet + plen, buf, hdr_len); plen += hdr_len;
    memcpy(packet + plen, payload, ppos); plen += ppos;

    if (!writeAll(packet, plen)) { closeSocket(); return false; }

    // Read CONNACK (fixed 4 bytes: 0x20 0x02 <flags> <return code>)
    uint8_t resp[4];
    if (!readExact(resp, 4, 5000)) { closeSocket(); return false; }
    if (resp[0] != 0x20 || resp[1] != 0x02) { closeSocket(); return false; }
    if (resp[3] != 0x00) { closeSocket(); return false; } // connack return code

    _last_activity = nowMs();
    return true;
  }

  void disconnect() {
    if (_sock < 0) return;
    uint8_t pkt[2] = { 0xE0, 0x00 }; // DISCONNECT
    writeAll(pkt, 2);
    closeSocket();
  }

  /** QoS 0 publish. payload may be binary (length given explicitly). */
  bool publish(const char* topic, const uint8_t* payload, unsigned int length, bool retain = false) {
    if (_sock < 0) return false;

    size_t tlen = strlen(topic);
    uint32_t remaining = 2 + tlen + length;

    uint8_t header[5];
    size_t hpos = 0;
    header[hpos++] = 0x30 | (retain ? 0x01 : 0x00); // PUBLISH, QoS0
    hpos += encodeRemainingLength(header + hpos, remaining);

    uint8_t tlen_be[2] = { (uint8_t)(tlen >> 8), (uint8_t)(tlen & 0xFF) };

    if (!writeAll(header, hpos)) return false;
    if (!writeAll(tlen_be, 2)) return false;
    if (!writeAll((const uint8_t*)topic, tlen)) return false;
    if (length > 0 && !writeAll(payload, length)) return false;

    _last_activity = nowMs();
    return true;
  }

  bool publish(const char* topic, const char* payload) {
    return publish(topic, (const uint8_t*)payload, strlen(payload), false);
  }

  /** QoS 0 subscribe. */
  bool subscribe(const char* topic) {
    if (_sock < 0) return false;

    size_t tlen = strlen(topic);
    uint32_t remaining = 2 /*packet id*/ + 2 /*topic len*/ + tlen + 1 /*qos*/;

    uint8_t header[5];
    size_t hpos = 0;
    header[hpos++] = 0x82; // SUBSCRIBE, flags must be 0b0010
    hpos += encodeRemainingLength(header + hpos, remaining);

    uint8_t body[256];
    size_t bpos = 0;
    body[bpos++] = 0x00; body[bpos++] = 0x01; // packet identifier = 1
    body[bpos++] = (uint8_t)(tlen >> 8);
    body[bpos++] = (uint8_t)(tlen & 0xFF);
    memcpy(body + bpos, topic, tlen); bpos += tlen;
    body[bpos++] = 0x00; // QoS 0

    if (!writeAll(header, hpos)) return false;
    if (!writeAll(body, bpos)) return false;

    _last_activity = nowMs();
    return true;
  }

  /**
   * Non-blocking poll. Reads and processes any available data, dispatching
   * inbound PUBLISH packets to the callback. Sends PINGREQ if the keepalive
   * interval has elapsed. Returns false if the connection was lost.
   */
  bool loop() {
    if (_sock < 0) return false;

    // keepalive
    unsigned long now = nowMs();
    if (_keepalive > 0 && (now - _last_activity) > (unsigned long)(_keepalive * 1000UL / 2)) {
      uint8_t ping[2] = { 0xC0, 0x00 };
      if (!writeAll(ping, 2)) {
        BRIDGE_DEBUG_PRINTLN("MQTT: PINGREQ write failed (errno=%d), closing\n", errno);
        closeSocket(); return false;
      }
      _last_activity = now;
    }

    // Try to read one packet (non-blocking)
    uint8_t type_byte;
    ssize_t n = recv(_sock, &type_byte, 1, MSG_DONTWAIT);
    if (n == 0) {
      BRIDGE_DEBUG_PRINTLN("MQTT: peer closed connection (recv returned 0)\n");
      closeSocket(); return false;
    } // peer closed
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true; // nothing to read
      BRIDGE_DEBUG_PRINTLN("MQTT: recv() error, errno=%d (%s)\n", errno, strerror(errno));
      closeSocket();
      return false;
    }

    // Read remaining length (variable, blocking with short timeout since
    // a packet header should arrive promptly once the first byte is here)
    uint32_t remaining = 0;
    uint32_t multiplier = 1;
    uint8_t enc_byte;
    do {
      if (!readExact(&enc_byte, 1, 2000)) {
        BRIDGE_DEBUG_PRINTLN("MQTT: timeout reading remaining-length byte\n");
        closeSocket(); return false;
      }
      remaining += (enc_byte & 0x7F) * multiplier;
      multiplier *= 128;
    } while (enc_byte & 0x80);

    if (remaining > _rx_buf_size) {
      // Packet too big for our buffer — drain and discard
      uint8_t discard[64];
      uint32_t left = remaining;
      while (left > 0) {
        uint32_t chunk = left > sizeof(discard) ? sizeof(discard) : left;
        if (!readExact(discard, chunk, 2000)) {
          BRIDGE_DEBUG_PRINTLN("MQTT: timeout draining oversized packet\n");
          closeSocket(); return false;
        }
        left -= chunk;
      }
      _last_activity = now;
      return true;
    }

    if (remaining > 0) {
      if (!readExact(_rx_buf, remaining, 2000)) {
        BRIDGE_DEBUG_PRINTLN("MQTT: timeout reading packet body (remaining=%u)\n", (unsigned)remaining);
        closeSocket(); return false;
      }
    }
    _last_activity = now;

    uint8_t ptype = type_byte & 0xF0;
    if (ptype == 0x30) { // PUBLISH
      if (remaining < 2) return true;
      uint16_t tlen = (_rx_buf[0] << 8) | _rx_buf[1];
      if (2 + (uint32_t)tlen > remaining) return true;

      char topic[128];
      size_t copy_len = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
      memcpy(topic, _rx_buf + 2, copy_len);
      topic[copy_len] = 0;

      uint8_t* payload = _rx_buf + 2 + tlen;
      unsigned int plen = remaining - 2 - tlen;

      // QoS 1/2 PUBLISH carry a 2-byte packet id immediately after the topic;
      // skip it (we don't send PUBACK — broker may redeliver, which is fine
      // for our duplicate-suppression logic).
      uint8_t qos = (type_byte & 0x06) >> 1;
      if (qos > 0 && plen >= 2) {
        payload += 2;
        plen -= 2;
      }

      if (_callback) _callback(topic, payload, plen);
    } else if (ptype == 0xD0) { // PINGRESP
      // nothing to do
    }

    return true;
  }

private:
  int _sock;
  MessageCallback _callback;
  uint16_t _keepalive;
  unsigned long _last_activity;
  uint8_t* _rx_buf;
  size_t _rx_buf_size;

  static unsigned long nowMs() { return millis(); }

  bool openSocket(const char* host, uint16_t port) {
    closeSocket();

    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return false;

    int fd = -1;
    for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
      fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
      if (fd < 0) continue;

      // Connect with a bounded timeout
      struct timeval tv;
      tv.tv_sec = 5; tv.tv_usec = 0;
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
        break;
      }
      ::close(fd);
      fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    _sock = fd;
    return true;
  }

  void closeSocket() {
    if (_sock >= 0) {
      ::close(_sock);
      _sock = -1;
    }
  }

  bool writeAll(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
      ssize_t n = send(_sock, data + sent, len - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR)) continue;
        return false;
      }
      sent += (size_t)n;
    }
    return true;
  }

  /** Blocking read of exactly `len` bytes, with an overall timeout (ms). */
  bool readExact(uint8_t* data, size_t len, unsigned long timeout_ms) {
    size_t got = 0;
    unsigned long start = nowMs();
    while (got < len) {
      ssize_t n = recv(_sock, data + got, len - got, 0);
      if (n > 0) {
        got += (size_t)n;
        continue;
      }
      if (n == 0) return false; // closed
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (nowMs() - start > timeout_ms) return false;
        continue;
      }
      return false;
    }
    return true;
  }

  static size_t writeUTF8(uint8_t* dst, const char* str) {
    size_t len = strlen(str);
    dst[0] = (uint8_t)(len >> 8);
    dst[1] = (uint8_t)(len & 0xFF);
    memcpy(dst + 2, str, len);
    return 2 + len;
  }

  /** Returns number of bytes written (1-4). */
  static size_t encodeRemainingLength(uint8_t* dst, uint32_t value) {
    size_t i = 0;
    do {
      uint8_t b = value % 128;
      value /= 128;
      if (value > 0) b |= 0x80;
      dst[i++] = b;
    } while (value > 0);
    return i;
  }
};

#endif // ARDULINUX_PLATFORM || WITH_MQTT_BRIDGE
