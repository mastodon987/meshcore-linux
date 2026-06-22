#pragma once

#include <MeshCore.h>
#include <Arduino.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>
#include <RadioLib.h>

// Default location of the meshcored config file, used when -c/--conf is
// not passed on the command line.
#define DEFAULT_MESHCORED_CONF "/etc/meshcored/meshcored.ini"

// Path to the config file to load, set via -c/--conf (see ardulinuxCustomInit()
// in LinuxBoard.cpp). Defaults to DEFAULT_MESHCORED_CONF.
extern const char *meshcoredConfPath;

// ── GPS serial device (optional) ──────────────────────────────────────────
// Set per-build in variants/linux/platformio.ini, e.g.:
//   -D ENV_INCLUDE_GPS=1
//   -D GPS_SERIAL_DEVICE='"/dev/serial0"'
//   -D GPS_BAUD_RATE=9600
// Left empty ("") by default -- meaning "no GPS attached" -- so boards/users
// who don't define it get exactly the previous no-GPS behaviour. Whether or
// not GPS_SERIAL_DEVICE is set, ENV_INCLUDE_GPS=1 should also be defined so
// the shared `gps on`/`gps off`/`gps sync` CLI commands (CommonCLI.cpp) and
// the persisted gps_enabled NodePref (applyGpsPrefs()) are compiled in.
#ifndef GPS_SERIAL_DEVICE
#define GPS_SERIAL_DEVICE ""
#endif
#ifndef GPS_BAUD_RATE
#define GPS_BAUD_RATE 9600
#endif

class LinuxConfig {
public:
  float lora_freq = LORA_FREQ;
  float lora_bw = LORA_BW;
  uint8_t lora_sf = LORA_SF;
#ifdef LORA_CR
  uint8_t lora_cr = LORA_CR;
#else
  uint8_t lora_cr = 5;
#endif

  uint32_t lora_irq_pin = RADIOLIB_NC;
  uint32_t lora_reset_pin = RADIOLIB_NC;
  uint32_t lora_nss_pin = RADIOLIB_NC;
  uint32_t lora_busy_pin = RADIOLIB_NC;
  uint32_t lora_rxen_pin = RADIOLIB_NC;
  uint32_t lora_txen_pin = RADIOLIB_NC;

  int8_t lora_tx_power = 22;
  float current_limit = 140;
  bool dio2_as_rf_switch = false;
  bool rx_boosted_gain = true;

  char* spidev = "/dev/spidev0.0";
  char* lora_gpiochip = "gpiochip0";

  float lora_tcxo = 1.8f;

  char *advert_name = "Linux Repeater";
  char *admin_password = "password";
  float lat = 0.0f;
  float lon = 0.0f;

  // ── MQTT bridge settings (optional [mqtt] section) ───────────────────────
  // mqtt_enabled tri-states: -1 = key absent from ini (don't touch the
  // NodePrefs autostart flag at all), 0 = explicitly disabled, 1 = enabled.
  int8_t mqtt_enabled = -1;
  char* mqtt_broker = nullptr;     // "broker" key      -> NodePrefs.mqtt_server
  uint16_t mqtt_port = 0;          // "port" key        -> NodePrefs.mqtt_port (0 = unset)
  char* mqtt_topic = nullptr;      // "topic" key       -> NodePrefs.mqtt_topic
  char* mqtt_username = nullptr;   // "username" key    -> NodePrefs.mqtt_user
  char* mqtt_password = nullptr;   // "password" key    -> NodePrefs.mqtt_pass

  // ── [sensors] section ──────────────────────────────────────────────────
  // ds18b20_id: the w1 slave address (e.g. "28-00000099921a") of a DS18B20
  // temperature sensor on the Pi's 1-Wire bus. Read from
  // /sys/devices/w1_bus_master1/<id>/temperature. nullptr = no DS18B20.
  char* ds18b20_id = nullptr;

  int load(const char *filename);
};

class LinuxBoard : public mesh::MainBoard {
protected:
  uint8_t startup_reason;
  uint8_t btn_prev_state;

public:
  void begin();

  uint16_t getBattMilliVolts() override {
    return 0;
  }

  uint8_t getStartupReason() const override { return startup_reason; }

  const char* getManufacturerName() const override {
    return "Linux";
  }

  int buttonStateChanged() {
    return 0;
  }

  void powerOff() override {
    exit(0);
  }

  void reboot() override {
    exit(0);
  }

  LinuxConfig config;
};

class LinuxRTCClock : public mesh::RTCClock {
public:
  LinuxRTCClock() { }
  void begin() {
  }
  uint32_t getCurrentTime() override {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
  }
  void setCurrentTime(uint32_t time) override {
    struct timeval tv;
    tv.tv_sec = time;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
  }
};
