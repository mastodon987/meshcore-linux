#ifdef ARDULINUX_PLATFORM

#include "LinuxConsoleServer.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <stddef.h>

void mc_console_default_socket_path(const char* instance, char* out, size_t out_len) {
  if (!instance || instance[0] == '\0') instance = "default";
  snprintf(out, out_len, "/tmp/meshcored-%s.sock", instance);
}

static const char* CONSOLE_PREFIX = "mcore> ";
static const int   CONSOLE_PREFIX_LEN = 7;

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static const char* HELP_TEXT =
  "mcore> MeshCore console commands:\r\n"
  "mcore>   ver                           -- firmware version\r\n"
  "mcore>   board                         -- board info\r\n"
  "mcore>   neighbors                     -- list neighbors\r\n"
  "mcore>   stats-packets                 -- packet statistics\r\n"
  "mcore>   clear stats                   -- reset packet counters\r\n"
  "mcore>   reboot                        -- reboot node\r\n"
  "mcore>   poweroff / shutdown           -- power off node\r\n"
  "mcore>   advert                        -- send flood advert\r\n"
  "mcore>   advert.zerohop                -- send zero-hop advert\r\n"
  "mcore>   clock                         -- show clock\r\n"
  "mcore>   powersaving on|off            -- power saving mode\r\n"
  "mcore>   log start|stop|erase          -- logging control\r\n"
  "mcore>   region <subcommand>           -- region management\r\n"
  "mcore>   gps on|off|sync|setloc|advert -- GPS control\r\n"
  "mcore>   password <pw>                 -- set admin password\r\n"
  "mcore>   get <key>                     -- get a setting value\r\n"
  "mcore>   set <key> <value>             -- set a setting value\r\n"
  "mcore> \r\n"
  "mcore> Common get/set keys:\r\n"
  "mcore>   name  lat  lon  freq  radio  tx  rx\r\n"
  "mcore>   dutycycle  txdelay  rxdelay  direct.txdelay\r\n"
  "mcore>   flood.max  flood.max.unscoped  flood.max.advert\r\n"
  "mcore>   path.hash.mode  loop.detect  int.thresh\r\n"
  "mcore>   agc.reset.interval  multi.acks  repeat\r\n"
  "mcore>   advert.interval  flood.advert.interval\r\n"
  "mcore>   allow.read.only  guest.password  owner.info\r\n"
  "mcore>   bridge.enabled  bridge.delay  bridge.source\r\n"
  "mcore>   bridge.mqtt.server  bridge.mqtt.port\r\n"
  "mcore>   bridge.mqtt.topic  bridge.mqtt.user  bridge.mqtt.pass\r\n"
  "mcore>   radio.rxgain  radio.rxboost\r\n"
  "mcore> \r\n"
  "mcore>   exit / quit                  -- disconnect from console\r\n";

LinuxConsoleServer::LinuxConsoleServer()
  : _server_fd(-1), _last_sender(-1)
{
  _node_name[0]   = '\0';
  _socket_path[0] = '\0';
  for (int i = 0; i < MC_CONSOLE_MAX_CLIENTS; i++) {
    _clients[i].fd     = -1;
    _clients[i].rx_len = 0;
  }
}

LinuxConsoleServer::~LinuxConsoleServer() {
  stop();
}

void LinuxConsoleServer::begin(const char* socket_path, const char* node_name) {
  if (_server_fd >= 0) return;

  strncpy(_socket_path, socket_path, sizeof(_socket_path) - 1);
  _socket_path[sizeof(_socket_path) - 1] = '\0';
  strncpy(_node_name, node_name ? node_name : "meshcored", sizeof(_node_name) - 1);
  _node_name[sizeof(_node_name) - 1] = '\0';

  unlink(_socket_path);

  _server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (_server_fd < 0) {
    perror("mc_console: socket");
    return;
  }

  set_nonblock(_server_fd);

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, _socket_path, sizeof(addr.sun_path) - 1);

  if (bind(_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("mc_console: bind");
    close(_server_fd);
    _server_fd = -1;
    return;
  }

  if (listen(_server_fd, 4) < 0) {
    perror("mc_console: listen");
    close(_server_fd);
    _server_fd = -1;
    return;
  }

  printf("MeshCore console: listening on %s  (connect with: mcore -s %s)\n",
         _socket_path, _socket_path);
}

void LinuxConsoleServer::stop() {
  for (int i = 0; i < MC_CONSOLE_MAX_CLIENTS; i++) {
    _closeClient(i);
  }
  if (_server_fd >= 0) {
    close(_server_fd);
    _server_fd = -1;
    unlink(_socket_path);
  }
}

void LinuxConsoleServer::_closeClient(int slot) {
  if (_clients[slot].fd < 0) return;
  close(_clients[slot].fd);
  _clients[slot].fd     = -1;
  _clients[slot].rx_len = 0;
  if (_last_sender == slot) _last_sender = -1;
}

void LinuxConsoleServer::_writeTo(int slot, const char* line) {
  if (slot < 0 || slot >= MC_CONSOLE_MAX_CLIENTS) return;
  if (_clients[slot].fd < 0) return;
  if (!line) return;

  struct iovec iov[3];
  char nl[3] = "\r\n";

  iov[0].iov_base = (void*)CONSOLE_PREFIX;
  iov[0].iov_len  = CONSOLE_PREFIX_LEN;
  iov[1].iov_base = (void*)line;
  iov[1].iov_len  = strlen(line);
  iov[2].iov_base = nl;
  iov[2].iov_len  = 2;

  ssize_t r = writev(_clients[slot].fd, iov, 3);
  if (r < 0 && errno != EAGAIN && errno != EPIPE) {
    _closeClient(slot);
  } else if (r == 0) {
    _closeClient(slot);
  }
}

void LinuxConsoleServer::_acceptNewClients() {
  if (_server_fd < 0) return;

  for (;;) {
    int fd = accept(_server_fd, nullptr, nullptr);
    if (fd < 0) break;

    int slot = -1;
    for (int i = 0; i < MC_CONSOLE_MAX_CLIENTS; i++) {
      if (_clients[i].fd < 0) { slot = i; break; }
    }
    if (slot < 0) {
      const char* msg = "mcore> Too many console clients connected.\r\n";
      ssize_t r = write(fd, msg, strlen(msg));
      (void)r;
      close(fd);
      continue;
    }

    set_nonblock(fd);
    _clients[slot].fd     = fd;
    _clients[slot].rx_len = 0;

    char banner[200];
    snprintf(banner, sizeof(banner),
             "\r\nConnected to MeshCore console (%s).\r\n"
             "Type 'help' for commands, 'exit' or Ctrl-D to quit.\r\n",
             _node_name);
    ssize_t r = write(fd, banner, strlen(banner));
    (void)r;
  }
}

bool LinuxConsoleServer::poll(char* cmd_out, size_t cmd_max) {
  if (_server_fd < 0) return false;

  _acceptNewClients();

  for (int i = 0; i < MC_CONSOLE_MAX_CLIENTS; i++) {
    if (_clients[i].fd < 0) continue;

    ConsoleClient& c = _clients[i];

    for (;;) {
      char ch;
      ssize_t n = read(c.fd, &ch, 1);
      if (n <= 0) {
        if (n == 0) {
          _closeClient(i);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          _closeClient(i);
        }
        break;
      }

      if (ch == '\r') continue;

      if (ch == '\n' || ch == '\0') {
        c.rx_buf[c.rx_len] = '\0';

        // trim trailing whitespace
        while (c.rx_len > 0 &&
               (c.rx_buf[c.rx_len - 1] == ' ' || c.rx_buf[c.rx_len - 1] == '\t')) {
          c.rx_buf[--c.rx_len] = '\0';
        }

        if (c.rx_len == 0) continue;

        // built-in: exit / quit
        if (strcmp(c.rx_buf, "exit") == 0 || strcmp(c.rx_buf, "quit") == 0) {
          const char* bye = "mcore> Disconnecting from MeshCore console.\r\n";
          ssize_t r = write(c.fd, bye, strlen(bye));
          (void)r;
          _closeClient(i);
          c.rx_len = 0;
          break;
        }

        // built-in: help
        if (strcmp(c.rx_buf, "help") == 0 || strcmp(c.rx_buf, "?") == 0) {
          ssize_t r = write(c.fd, HELP_TEXT, strlen(HELP_TEXT));
          (void)r;
          c.rx_len = 0;
          break;
        }

        // forward to firmware CLI
        strncpy(cmd_out, c.rx_buf, cmd_max - 1);
        cmd_out[cmd_max - 1] = '\0';
        c.rx_len = 0;
        _last_sender = i;
        return true;

      } else {
        if (c.rx_len < (int)(sizeof(c.rx_buf) - 1)) {
          c.rx_buf[c.rx_len++] = ch;
        }
      }
    }
  }
  return false;
}

void LinuxConsoleServer::sendReply(const char* reply) {
  if (!reply || reply[0] == '\0') return;
  if (_last_sender >= 0) {
    _writeTo(_last_sender, reply);
  }
}

void LinuxConsoleServer::broadcastLine(const char* line) {
  if (!line) return;
  for (int i = 0; i < MC_CONSOLE_MAX_CLIENTS; i++) {
    if (_clients[i].fd >= 0) {
      _writeTo(i, line);
    }
  }
}

#endif // ARDULINUX_PLATFORM
