"""
scripts/patch_busio.py  —  pre-build script for PlatformIO
Idempotent: safe to re-run after pio pkg update / cache clear.

Fix A — Adafruit_SPIDevice.h  (linux_repeater_mqtt, linux_room_mqtt)
    Linux ARM hits #define BUSIO_USE_FAST_PINIO via the __arm__ branch.
    Inject !defined(__linux__) to skip it.

Fix B — Adafruit_I2CDevice.cpp  (linux_repeater_mqtt, linux_room_mqtt)
    write(0) ambiguous between write(uint8_t) and write(const char*).

Fix C — examples/simple_room_server/main.cpp  (linux_room_mqtt)
    Add ARDULINUX_PLATFORM branch for filesystem + IdentityStore init,
    mirroring exactly what simple_repeater/main.cpp already has.

Fix D — examples/simple_room_server/MyMesh.cpp  (linux_room_mqtt)
    Three platform guards missing the ARDULINUX_PLATFORM case:
      D1  openAppend()      — drop 3-arg open (not supported on ardulinux)
      D2  formatFileSystem()— return false (no format on Linux)
      D3  saveIdentity()    — use IdentityStore with /identity dir
"""

Import("env")   # noqa: F821  PlatformIO SCons context
import os

PROJECT_DIR = env.subst("$PROJECT_DIR")   # noqa: F821


def libdep_dir(env_name):
    base = os.path.join(PROJECT_DIR, ".pio", "libdeps", env_name)
    if not os.path.isdir(base):
        return os.path.join(base, "Adafruit BusIO")
    candidates = [n for n in os.listdir(base) if n.startswith("Adafruit BusIO")]
    if not candidates:
        return os.path.join(base, "Adafruit BusIO")
    # Prefer a versioned dir (e.g. "Adafruit BusIO@1.16.1") over the bare one
    versioned = [c for c in candidates if "@" in c]
    chosen = versioned[0] if versioned else candidates[0]
    return os.path.join(base, chosen)


def patch_file(path, replacements, label):
    if not os.path.exists(path):
        print("  [patch] SKIP not found: " + label)
        return False
    with open(path, "r") as f:
        src = f.read()
    out = src
    for old, new in replacements:
        out = out.replace(old, new)
    if out != src:
        with open(path, "w") as f:
            f.write(out)
        print("  [patch] OK: " + label)
        return True
    print("  [patch] already patched: " + label)
    return False


# ---------------------------------------------------------------------------
# Fix A
# ---------------------------------------------------------------------------
A_OLD = "#elif (defined(__arm__) || defined(ARDUINO_FEATHER52)) &&\\"
A_NEW = "#elif (defined(__arm__) || defined(ARDUINO_FEATHER52)) && !defined(__linux__) &&\\"

# ---------------------------------------------------------------------------
# Fix A2 — Adafruit_SPIDevice.h  (linux_repeater and others)
#    On ARM64 Linux, the ESP8266/ESP32/__SAM3X8E__/ARDUINO_ARCH_SAMD branch
#    is the one that actually fires and defines BUSIO_USE_FAST_PINIO.
#    Exclude __linux__ from that branch too.
# ---------------------------------------------------------------------------
A2_OLD = ("#elif defined(ESP8266) || defined(ESP32) || defined(__SAM3X8E__) ||"
          "            \\\n    defined(ARDUINO_ARCH_SAMD)")
A2_NEW = ("#elif (defined(ESP8266) || defined(ESP32) || defined(__SAM3X8E__) ||"
          "            \\\n    defined(ARDUINO_ARCH_SAMD)) && !defined(__linux__)")

# ---------------------------------------------------------------------------
# Fix A3 — belt-and-braces: unconditionally undef BUSIO_USE_FAST_PINIO on
# Linux right before the Adafruit_SPIDevice class definition, regardless of
# which #elif branch in the platform-detection chain fired.
# ---------------------------------------------------------------------------
A3_OLD = "class Adafruit_SPIDevice {"
A3_NEW = ("#ifdef __linux__\n"
          "#ifdef BUSIO_USE_FAST_PINIO\n"
          "#undef BUSIO_USE_FAST_PINIO\n"
          "#endif\n"
          "#endif\n\n"
          "class Adafruit_SPIDevice {")

# ---------------------------------------------------------------------------
# Fix B
# ---------------------------------------------------------------------------
B_OLD = "_wire->write(0);"
B_NEW = "_wire->write((uint8_t)0);"

# ---------------------------------------------------------------------------
# Fix C  — simple_room_server/main.cpp
# Add ARDULINUX_PLATFORM branch before the #else #error block
# ---------------------------------------------------------------------------
C_OLD = """\
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {"""

C_NEW = """\
#elif defined(ARDULINUX_PLATFORM)
  fs = &ArduLinuxFS;
  IdentityStore store(ArduLinuxFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {"""

# ---------------------------------------------------------------------------
# Fix D1 — openAppend(): add ARDULINUX branch (same as RP2040: 2-arg open)
# ---------------------------------------------------------------------------
D1_OLD = """\
#if defined(NRF52_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif"""

D1_NEW = """\
#if defined(NRF52_PLATFORM)
  return _fs->open(fname, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM) || defined(ARDULINUX_PLATFORM)
  return _fs->open(fname, "a");
#else
  return _fs->open(fname, "a", true);
#endif"""

# ---------------------------------------------------------------------------
# Fix D2 — formatFileSystem(): add ARDULINUX branch (not supported, return false)
# ---------------------------------------------------------------------------
D2_OLD = """\
#elif defined(ESP32)
  return SPIFFS.format();
#else
#error "need to implement file system erase"
  return false;
#endif"""

D2_NEW = """\
#elif defined(ESP32)
  return SPIFFS.format();
#elif defined(ARDULINUX_PLATFORM)
  return false;  // filesystem format not supported on Linux
#else
#error "need to implement file system erase"
  return false;
#endif"""

# ---------------------------------------------------------------------------
# Fix D3 — saveIdentity(): add ARDULINUX to the ESP32/RP2040 branch
# ---------------------------------------------------------------------------
D3_OLD = """\
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32)
  IdentityStore store(*_fs, "/identity");
#elif defined(RP2040_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif"""

D3_NEW = """\
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#elif defined(ESP32) || defined(RP2040_PLATFORM) || defined(ARDULINUX_PLATFORM)
  IdentityStore store(*_fs, "/identity");
#else
#error "need to define saveIdentity()"
#endif"""

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

ENVS_AB = ["linux_repeater", "linux_repeater_mqtt", "linux_room_mqtt", "linux_room_mqtt_only"]

ROOM_MAIN   = os.path.join(PROJECT_DIR, "examples", "simple_room_server", "main.cpp")
ROOM_MYMESH = os.path.join(PROJECT_DIR, "examples", "simple_room_server", "MyMesh.cpp")

print("[patch] Applying Linux build fixes ...")

for env_name in ENVS_AB:
    d = libdep_dir(env_name)
    if not os.path.isdir(d):
        print("  [patch] libdep dir not yet present for " + env_name)
        continue
    patch_file(os.path.join(d, "Adafruit_SPIDevice.h"),   [(A_OLD, A_NEW), (A2_OLD, A2_NEW), (A3_OLD, A3_NEW)], "SPIDevice.h (Fix A+A2+A3) [" + env_name + "]")
    patch_file(os.path.join(d, "Adafruit_I2CDevice.cpp"), [(B_OLD, B_NEW)], "I2CDevice.cpp (Fix B) [" + env_name + "]")

patch_file(ROOM_MAIN,   [(C_OLD,  C_NEW)],  "simple_room_server/main.cpp (Fix C)")
patch_file(ROOM_MYMESH, [(D1_OLD, D1_NEW),
                          (D2_OLD, D2_NEW),
                          (D3_OLD, D3_NEW)], "simple_room_server/MyMesh.cpp (Fix D1+D2+D3)")

print("[patch] Done.")
