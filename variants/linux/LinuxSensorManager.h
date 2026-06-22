#pragma once

#include <Mesh.h>
#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>

// ─────────────────────────────────────────────────────────────────────────
// LinuxSensorManager
//
// SensorManager implementation for the `linux` variant (meshcored). Two
// independent, opt-in pieces, both controlled entirely by build-time flags
// in variants/linux/platformio.ini -- nothing here requires editing C++ to
// turn on:
//
//   1. GPS, via any NMEA-speaking GPS module attached to a serial device
//      (e.g. an ATGM336H on /dev/serial0). Controlled by GPS_SERIAL_DEVICE /
//      GPS_BAUD_RATE (see LinuxBoard.h for the defaults/macros). If
//      GPS_SERIAL_DEVICE is left empty, no GPS code runs at all -- same as
//      "no GPS" on every other variant.
//
//   2. Custom sensors -- anything you can read from Linux userspace that
//      isn't one of the built-in I2C chips already supported by
//      EnvironmentSensorManager: a Python/shell helper, a sysfs/hwmon file,
//      a USB sensor, vcgencmd, a Modbus/serial device, etc. Add your code in
//      queryCustomSensors() below (in LinuxSensorManager.cpp) -- it runs on
//      every telemetry request, with normal CayenneLPP add*() calls.
// ─────────────────────────────────────────────────────────────────────────

class LinuxSensorManager : public SensorManager {
  LocationProvider* _location = nullptr;
  bool gps_active = false;

public:
  LinuxSensorManager() { }

  bool begin() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) override;
  void loop() override;

  LocationProvider* getLocationProvider() override { return _location; }

  int getNumSettings() const override;
  const char* getSettingName(int i) const override;
  const char* getSettingValue(int i) const override;
  bool setSettingValue(const char* name, const char* value) override;

private:
  // Implement this in LinuxSensorManager.cpp with whatever custom sensor
  // logic you want -- shelling out to a script, reading a file, talking to
  // a serial/USB/I2C device directly, etc. Called from querySensors() only
  // when the requester has TELEM_PERM_ENVIRONMENT permission.
  void queryCustomSensors(uint8_t& channel, CayenneLPP& telemetry);
};
