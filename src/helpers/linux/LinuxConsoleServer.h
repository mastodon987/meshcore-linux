#pragma once

#ifdef ARDULINUX_PLATFORM

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef MC_CONSOLE_MAX_CLIENTS
  #define MC_CONSOLE_MAX_CLIENTS 4
#endif

/**
 * LinuxConsoleServer -- Unix domain socket console server.
 *
 * Exposes the meshcored CLI (handleCommand) over a Unix stream socket so
 * that `mcore` (or any other tool) can connect as an interactive console.
 *
 * Protocol (plain text, line-oriented):
 *   - Server sends a one-line banner on connect:
 *       "Connected to MeshCore console (<node_name>). Press Ctrl-D or type 'exit' to quit.\n"
 *   - Every output line from the server is prefixed with "mcore> ".
 *   - Client sends lines terminated by '\n' or '\r\n'.
 *   - An empty line / Ctrl-D (EOF) / "exit" disconnects the client.
 *
 * The socket path defaults to /tmp/meshcored-<instance>.sock where <instance>
 * is determined by MC_CONSOLE_INSTANCE (build-time, default "default").
 * The `mcore` client uses the same default, or accepts a path via -s <path>.
 */
class LinuxConsoleServer {
public:
  LinuxConsoleServer();
  ~LinuxConsoleServer();

  /**
   * Open the Unix domain socket.
   * @param socket_path  Full path to the socket file.
   * @param node_name    Displayed in the banner on connect.
   */
  void begin(const char* socket_path, const char* node_name);

  /** Close server and disconnect all console clients. */
  void stop();

  bool isRunning() const { return _server_fd >= 0; }

  /**
   * Accept new console clients and process one line from any connected client.
   * Call from the mesh main loop.  When a complete command line is received,
   * it is stored in cmd_out (null-terminated) and true is returned.
   * The caller must call sendReply() (or sendLine()) with the response.
   */
  bool poll(char* cmd_out, size_t cmd_max);

  /** Send a reply line to whichever console client sent the last command. */
  void sendReply(const char* reply);

  /** Broadcast an unsolicited line to all connected console clients (e.g. log). */
  void broadcastLine(const char* line);

private:
  struct ConsoleClient {
    int  fd;
    char rx_buf[256];
    int  rx_len;
  };

  int           _server_fd;
  ConsoleClient _clients[MC_CONSOLE_MAX_CLIENTS];
  int           _last_sender;
  char          _node_name[48];
  char          _socket_path[128];

  void _acceptNewClients();
  void _closeClient(int slot);
  void _writeTo(int slot, const char* line);
};

/**
 * Returns the default console socket path for a given instance name.
 * Result is written into 'out' (at least 128 bytes).
 * Typical result: "/tmp/meshcored-default.sock"
 */
void mc_console_default_socket_path(const char* instance, char* out, size_t out_len);

#endif // ARDULINUX_PLATFORM
