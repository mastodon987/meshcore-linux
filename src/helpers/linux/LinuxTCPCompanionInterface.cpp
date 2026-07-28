#ifdef ARDULINUX_PLATFORM

#include "LinuxTCPCompanionInterface.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

// Frame parser states (RX path: client -> server)
#define RECV_STATE_IDLE       0
#define RECV_STATE_HDR_FOUND  1
#define RECV_STATE_LEN1_FOUND 2
#define RECV_STATE_DATA       3

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_nodelay(int fd) {
  int val = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
}

LinuxTCPCompanionInterface::LinuxTCPCompanionInterface()
  : _server_fd(-1), _enabled(false), _last_sender_slot(-1)
{
  for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
    _clients[i].fd = -1;
    _clients[i].rx_len = 0;
    _clients[i].frame_len = 0;
    _clients[i].state = RECV_STATE_IDLE;
  }
}

LinuxTCPCompanionInterface::~LinuxTCPCompanionInterface() {
  stop();
}

void LinuxTCPCompanionInterface::begin(uint16_t port) {
  if (_server_fd >= 0) return;

  _server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (_server_fd < 0) {
    perror("tcp_companion: socket");
    return;
  }

  int reuse = 1;
  setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  set_nonblock(_server_fd);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons(port);

  if (bind(_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("tcp_companion: bind");
    close(_server_fd);
    _server_fd = -1;
    return;
  }

  if (listen(_server_fd, 4) < 0) {
    perror("tcp_companion: listen");
    close(_server_fd);
    _server_fd = -1;
    return;
  }

  printf("MeshCore companion: listening on TCP port %u (MeshCore Android app compatible)\n", (unsigned)port);
  _enabled = true;
}

void LinuxTCPCompanionInterface::stop() {
  for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
    _closeClient(i);
  }
  if (_server_fd >= 0) {
    close(_server_fd);
    _server_fd = -1;
  }
}

bool LinuxTCPCompanionInterface::isConnected() const {
  for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].fd >= 0) return true;
  }
  return false;
}

int LinuxTCPCompanionInterface::connectedCount() const {
  int n = 0;
  for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].fd >= 0) n++;
  }
  return n;
}

void LinuxTCPCompanionInterface::_closeClient(int slot) {
  if (_clients[slot].fd < 0) return;
  close(_clients[slot].fd);
  _clients[slot].fd = -1;
  _clients[slot].rx_len = 0;
  _clients[slot].frame_len = 0;
  _clients[slot].state = RECV_STATE_IDLE;
  if (_last_sender_slot == slot) _last_sender_slot = -1;
}

void LinuxTCPCompanionInterface::_acceptNewClients() {
  if (_server_fd < 0) return;

  for (;;) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int fd = accept(_server_fd, (struct sockaddr*)&peer, &peer_len);
    if (fd < 0) break; // EAGAIN / EWOULDBLOCK

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
      if (_clients[i].fd < 0) { slot = i; break; }
    }
    if (slot < 0) {
      // No room -- close immediately
      close(fd);
      printf("MeshCore companion: rejected client (max %d clients connected)\n",
             TCP_COMPANION_MAX_CLIENTS);
      continue;
    }

    set_nonblock(fd);
    set_nodelay(fd);

    _clients[slot].fd = fd;
    _clients[slot].rx_len = 0;
    _clients[slot].frame_len = 0;
    _clients[slot].state = RECV_STATE_IDLE;

    printf("MeshCore companion: client connected from %s (slot %d)\n",
           inet_ntoa(peer.sin_addr), slot);
  }
}

size_t LinuxTCPCompanionInterface::_writeToSlot(int slot, const uint8_t* data, size_t len) {
  if (slot < 0 || slot >= TCP_COMPANION_MAX_CLIENTS) return 0;
  if (_clients[slot].fd < 0) return 0;
  if (len > MAX_FRAME_SIZE) return 0;

  uint8_t hdr[3];
  hdr[0] = '>';
  hdr[1] = (uint8_t)(len & 0xFF);
  hdr[2] = (uint8_t)(len >> 8);

  ssize_t r = send(_clients[slot].fd, hdr, 3, MSG_NOSIGNAL);
  if (r < 3) {
    _closeClient(slot);
    return 0;
  }
  r = send(_clients[slot].fd, data, len, MSG_NOSIGNAL);
  if (r < (ssize_t)len) {
    _closeClient(slot);
    return 0;
  }
  return len;
}

size_t LinuxTCPCompanionInterface::writeFrame(const uint8_t src[], size_t len) {
  if (!_enabled || len > MAX_FRAME_SIZE) return 0;

  // Reply to the client that originated the last command; fall back to all
  if (_last_sender_slot >= 0 && _clients[_last_sender_slot].fd >= 0) {
    return _writeToSlot(_last_sender_slot, src, len);
  }
  return writeFrameToAll(src, len);
}

size_t LinuxTCPCompanionInterface::writeFrameToAll(const uint8_t src[], size_t len) {
  if (!_enabled || len > MAX_FRAME_SIZE) return 0;
  size_t sent = 0;
  for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].fd >= 0) {
      if (_writeToSlot(i, src, len) > 0) sent = len;
    }
  }
  return sent;
}

size_t LinuxTCPCompanionInterface::checkRecvFrame(uint8_t dest[]) {
  if (!_enabled || _server_fd < 0) return 0;

  _acceptNewClients();

  for (int i = 0; i < TCP_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].fd < 0) continue;

    Client& c = _clients[i];

    // Read as many bytes as available this tick
    for (;;) {
      uint8_t byte;
      ssize_t n = recv(c.fd, &byte, 1, 0);
      if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          _closeClient(i);
        }
        break;
      }

      switch (c.state) {
        case RECV_STATE_IDLE:
          if (byte == '<') c.state = RECV_STATE_HDR_FOUND;
          break;

        case RECV_STATE_HDR_FOUND:
          c.frame_len = byte;       // LSB
          c.state = RECV_STATE_LEN1_FOUND;
          break;

        case RECV_STATE_LEN1_FOUND:
          c.frame_len |= ((uint16_t)byte) << 8; // MSB
          c.rx_len = 0;
          if (c.frame_len == 0 || c.frame_len > MAX_FRAME_SIZE) {
            c.state = RECV_STATE_IDLE; // bad/oversized frame, resync
          } else {
            c.state = RECV_STATE_DATA;
          }
          break;

        case RECV_STATE_DATA:
          if (c.rx_len < MAX_FRAME_SIZE) {
            c.rx_buf[c.rx_len] = byte;
          }
          c.rx_len++;
          if (c.rx_len >= c.frame_len) {
            // Complete frame received
            uint16_t copy_len = c.frame_len < MAX_FRAME_SIZE ? c.frame_len : MAX_FRAME_SIZE;
            memcpy(dest, c.rx_buf, copy_len);
            c.state = RECV_STATE_IDLE;
            c.rx_len = 0;
            _last_sender_slot = i;
            return copy_len;
          }
          break;
      }
    }
  }
  return 0;
}

#endif // ARDULINUX_PLATFORM
