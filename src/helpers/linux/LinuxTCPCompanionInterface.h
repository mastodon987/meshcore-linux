#pragma once

#ifdef ARDULINUX_PLATFORM

#include <helpers/BaseSerialInterface.h>
#include <stdint.h>
#include <stddef.h>

#ifndef TCP_COMPANION_PORT
  #define TCP_COMPANION_PORT 5000
#endif

#ifndef TCP_COMPANION_MAX_CLIENTS
  #define TCP_COMPANION_MAX_CLIENTS 3
#endif

/**
 * Linux-native TCP companion server implementing BaseSerialInterface.
 *
 * Speaks the same binary framing as ArduinoSerialInterface / the MeshCore
 * Android/desktop app companion protocol:
 *   TX (server -> client):  '>',  len_lo,  len_hi, <payload>
 *   RX (client -> server):  '<',  len_lo,  len_hi, <payload>
 *
 * Multiple simultaneous clients are accepted.  All clients receive every
 * frame sent via writeFrame() / writeFrameToAll().  The first client that
 * sends a frame "wins" the command slot for that frame; the reply goes to
 * that client only.  This mirrors how the ESP32 MultiTransportCompanionInterface
 * handles TCP (last-writer-wins for _last_reply_target).
 *
 * The socket is non-blocking; call loop() or poll() from the main mesh loop.
 */
class LinuxTCPCompanionInterface : public BaseSerialInterface {
public:
  LinuxTCPCompanionInterface();
  ~LinuxTCPCompanionInterface();

  /**
   * Open the listening socket.  Idempotent (no-op if already open).
   * @param port  TCP port to listen on (default TCP_COMPANION_PORT).
   */
  void begin(uint16_t port = TCP_COMPANION_PORT);

  /** Close the server and disconnect all clients. */
  void stop();

  bool isRunning() const { return _server_fd >= 0; }

  // ---- BaseSerialInterface ----
  void enable() override   { _enabled = true; }
  void disable() override  { _enabled = false; }
  bool isEnabled() const override { return _enabled; }

  bool isConnected() const override;
  bool isWriteBusy() const override { return false; }

  /**
   * Send a framed message to whichever client last sent us a command.
   * Falls back to broadcast if no client has spoken yet.
   */
  size_t writeFrame(const uint8_t src[], size_t len) override;

  /**
   * Broadcast a framed message to all connected clients.
   */
  size_t writeFrameToAll(const uint8_t src[], size_t len);

  /**
   * Accept new connections + read one complete frame from any client.
   * Returns frame length (> 0) when a complete frame has arrived; 0
   * otherwise.  Sets _last_sender_slot to the originating client index.
   */
  size_t checkRecvFrame(uint8_t dest[]) override;

  /** Number of currently connected clients. */
  int connectedCount() const;

private:
  struct Client {
    int fd;
    uint8_t rx_buf[MAX_FRAME_SIZE];
    uint16_t rx_len;
    uint16_t frame_len;
    uint8_t  state;   // parse state machine
  };

  int    _server_fd;
  bool   _enabled;
  Client _clients[TCP_COMPANION_MAX_CLIENTS];
  int    _last_sender_slot;   // slot of the client that sent the last command (-1 = none/broadcast)

  void _acceptNewClients();
  void _closeClient(int slot);
  size_t _writeToSlot(int slot, const uint8_t* data, size_t len);
};

#endif // ARDULINUX_PLATFORM
