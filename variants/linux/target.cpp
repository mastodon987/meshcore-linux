#include <helpers/ArduinoHelpers.h>
#include <Arduino.h>
#include "target.h"

LinuxBoard board;
LinuxRTCClock rtc_clock;
LinuxSensorManager sensors;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

// ─────────────────────────────────────────────────────────────────────────────
// RADIO_NONE path — MQTT-only nodes (linux_room_mqtt_only).
// No SPI, no GPIO, no RadioLib at all.
// ─────────────────────────────────────────────────────────────────────────────
#if defined(RADIO_NONE)

NullRadio radio_driver;

bool radio_init() {
  rtc_clock.begin();
  return true;   // nothing to initialise
}

uint32_t radio_get_rng_seed() {
  // No hardware RNG — use time-based seed, good enough for an MQTT-only node.
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t)(tv.tv_sec ^ tv.tv_usec);
}

void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  (void)freq; (void)bw; (void)sf; (void)cr;
}

void radio_set_tx_power(uint8_t dbm) {
  (void)dbm;
}

mesh::LocalIdentity radio_new_identity() {
  // Seed a software RNG from the system clock so the identity is unique
  // even without a hardware noise source.
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint32_t seed = (uint32_t)(tv.tv_sec ^ tv.tv_usec ^ (tv.tv_usec << 16));
  StdRNG rng;
  rng.begin(seed);
  return mesh::LocalIdentity(&rng);
}

// ─────────────────────────────────────────────────────────────────────────────
// Normal RadioLib path — all other linux envs.
// ─────────────────────────────────────────────────────────────────────────────
#else

class ArduLinuxHal : public ArduinoHal {
public:
  ArduLinuxHal(SPIClass &spi, SPISettings spiSettings) : ArduinoHal(spi, spiSettings) {}
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in) {
    memcpy(in, out, len);
    spi->transfer(in, len);
  }
};

SPISettings spiSettings = SPISettings(2000000, MSBFIRST, SPI_MODE0);
ArduinoHal *hal = new ArduLinuxHal(SPI, spiSettings);
RADIO_CLASS radio = new Module(hal, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC);
WRAPPER_CLASS radio_driver(radio, board);

bool radio_init() {
  rtc_clock.begin();

  radio = new Module(hal,
                     board.config.lora_nss_pin,
                     board.config.lora_irq_pin,
                     board.config.lora_reset_pin,
                     board.config.lora_busy_pin);
  return radio.std_init(&SPI);
}

uint32_t radio_get_rng_seed() {
  return radio.random(0x7FFFFFFF);
}

void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio.setFrequency(freq);
  radio.setSpreadingFactor(sf);
  radio.setBandwidth(bw);
  radio.setCodingRate(cr);
}

void radio_set_tx_power(uint8_t dbm) {
  radio.setOutputPower(dbm);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}

#endif // RADIO_NONE
