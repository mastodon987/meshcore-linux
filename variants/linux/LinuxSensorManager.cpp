#include "LinuxSensorManager.h"
#include "LinuxBoard.h"   // GPS_SERIAL_DEVICE / GPS_BAUD_RATE defaults
#include "target.h"       // board.config.ds18b20_id

#include <string.h>
#include <stdio.h>

// ─────────────────────────────────────────────────────────────────────────
// GPS (optional)
//
// GPS_SERIAL_DEVICE / GPS_BAUD_RATE are always defined (LinuxBoard.h
// defaults GPS_SERIAL_DEVICE to "" and GPS_BAUD_RATE to 9600). Set real
// values per-build in variants/linux/platformio.ini, e.g.:
//
//   -D GPS_SERIAL_DEVICE='"/dev/serial0"'
//   -D GPS_BAUD_RATE=9600
//
// With GPS_SERIAL_DEVICE left empty (the default), GPS is simply never
// enabled -- same as "no GPS" on any other board variant.
// ─────────────────────────────────────────────────────────────────────────
static bool linuxGpsEnabled() {
  return GPS_SERIAL_DEVICE[0] != '\0';
}

#include <helpers/sensors/GenericNMEALocationProvider.h>
static GenericNMEALocationProvider* linux_nmea = nullptr;

bool LinuxSensorManager::begin() {
  if (linuxGpsEnabled()) {
    Serial1.setPath(GPS_SERIAL_DEVICE);
    Serial1.begin(GPS_BAUD_RATE);

    // rtc clock pointer is optional -- pass nullptr here; the linux variant
    // already syncs system time independently. Pass &rtc_clock instead if
    // you want the GPS fix to set the system clock too.
    //
    // IMPORTANT: do not define PIN_GPS_EN / PIN_GPS_RESET (or pass non -1
    // pin_en/pin_reset args here) on this variant unless you've also
    // registered that GPIO line with initGPIOPin() in LinuxBoard.cpp first.
    // ardulinux's digitalWrite()/pinMode() assert() (crashing the whole
    // process) on any pin number that wasn't pre-registered.
    linux_nmea = new GenericNMEALocationProvider(Serial1, &rtc_clock);
    linux_nmea->begin();
    _location = linux_nmea;
    gps_active = true;
    printf("LinuxSensorManager: GPS enabled on %s @ %d baud\n", GPS_SERIAL_DEVICE, (int)GPS_BAUD_RATE);
  } else {
    printf("LinuxSensorManager: no GPS_SERIAL_DEVICE configured, GPS disabled\n");
  }

  // ── Add any one-time custom sensor init here (open a file handle,
  //    spawn a helper process, configure a serial/I2C/USB device, etc). ──

  return true;
}

void LinuxSensorManager::loop() {
  if (gps_active && linux_nmea) {
    linux_nmea->loop();
    node_lat = (double)linux_nmea->getLatitude() / 1000000.0;
    node_lon = (double)linux_nmea->getLongitude() / 1000000.0;
    node_altitude = (double)linux_nmea->getAltitude() / 1000.0;
  }
}

bool LinuxSensorManager::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  uint8_t channel = TELEM_CHANNEL_SELF + 1;

  if ((requester_permissions & TELEM_PERM_LOCATION) && gps_active && linux_nmea && linux_nmea->isValid()) {
    telemetry.addGPS(TELEM_CHANNEL_SELF, node_lat, node_lon, node_altitude);
  }

  if (requester_permissions & TELEM_PERM_ENVIRONMENT) {
    queryCustomSensors(channel, telemetry);
  }

  return true;
}

// ─────────────────────────────────────────────────────────────────────────
// Custom sensors -- EDIT THIS FUNCTION
//
// This runs once per telemetry request (e.g. when a phone app or another
// node sends a "get telemetry" request over the mesh -- LoRa or, for
// linux_room_mqtt_only, over the MQTT bridge; both paths call
// sensors.querySensors() the same way).
//
// `telemetry` is a CayenneLPP buffer -- use any of its add*() methods.
// `channel` is the next free LPP channel; increment it for each extra
// "virtual sensor" you add so multiple readings don't collide on one
// channel number.
//
// Examples of "custom sensors grabbed from Linux":
//   - shell out to a script/binary and parse its stdout
//   - read a sysfs/hwmon file (e.g. CPU temp, throttling flags)
//   - talk to a USB/serial device directly with your own protocol
//   - read an I2C chip yourself via LinuxHardwareI2C without going through
//     EnvironmentSensorManager's built-in chip table
// ─────────────────────────────────────────────────────────────────────────
void LinuxSensorManager::queryCustomSensors(uint8_t& channel, CayenneLPP& telemetry) {
  // DS18B20 (1-Wire), address set via [sensors] ds18b20_id in meshcored.ini.
  // e.g.  ds18b20_id = 28-00000099921a
  // reads /sys/devices/w1_bus_master1/<ds18b20_id>/temperature (millideg C).
  if (board.config.ds18b20_id != nullptr) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/w1_bus_master1/%s/temperature", board.config.ds18b20_id);
    FILE* f = fopen(path, "r");
    if (f) {
      int milli_c = 0;
      if (fscanf(f, "%d", &milli_c) == 1) {
        telemetry.addTemperature(channel++, milli_c / 1000.0f);
      }
      fclose(f);
    } else {
      printf("LinuxSensorManager: could not read DS18B20 at %s\n", path);
    }
  }

  // Example: Raspberry Pi SoC temperature from sysfs (works on any Pi
  // running a stock kernel -- no extra deps). Remove/replace with your
  // own sensor(s).
  FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  if (f) {
    int milli_c = 0;
    if (fscanf(f, "%d", &milli_c) == 1) {
      telemetry.addTemperature(channel++, milli_c / 1000.0f);
    }
    fclose(f);
  }

  // Add more telemetry.addXxx(channel++, ...) calls here for additional
  // custom sensors.
}

int LinuxSensorManager::getNumSettings() const {
  return linuxGpsEnabled() ? 1 : 0;
}

const char* LinuxSensorManager::getSettingName(int i) const {
  if (linuxGpsEnabled() && i == 0) return "gps";
  return NULL;
}

const char* LinuxSensorManager::getSettingValue(int i) const {
  if (linuxGpsEnabled() && i == 0) return gps_active ? "1" : "0";
  return NULL;
}

bool LinuxSensorManager::setSettingValue(const char* name, const char* value) {
  if (linuxGpsEnabled() && strcmp(name, "gps") == 0) {
    bool enable = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
    gps_active = enable;
    if (linux_nmea) {
      if (enable) linux_nmea->begin(); else linux_nmea->stop();
    }
    return true;
  }
  return false;
}
