#pragma once

#include <Mesh.h>
#include <sys/time.h>

/**
 * @brief A "no radio" implementation of mesh::Radio.
 *
 * Used by MQTT-only nodes (e.g. linux_room_mqtt_only) that must never open the
 * SPI device or claim any LoRa GPIO lines — all packet I/O for these nodes
 * happens exclusively via MQTTBridge (src/helpers/bridges/MQTTBridge.*):
 *
 *   - Inbound:  MQTTBridge::handleMQTTMessage() -> handleReceivedPacket()
 *               -> _mgr->queueInbound(...)   (normal Dispatcher RX path)
 *   - Outbound: Dispatcher::checkSend() calls into this NullRadio for the
 *               *framing/airtime* side, but the actual bytes are published
 *               by MQTTBridge from MyMesh::logTx()/logRx().
 *
 * Every method here is a harmless stub:
 *   - recvRaw() never returns data (no RF packets are ever received here)
 *   - startSendRaw()/isSendComplete() report immediate success, so the
 *     Dispatcher's TX state machine never stalls waiting on hardware
 *   - getEstAirtimeFor() returns 0, so airtime-budget throttling (which is
 *     meaningless without a radio) never blocks sends
 *   - isInRecvMode() is always true, so the node never tries to "switch to
 *     TX" on real hardware
 *
 * No PhysicalLayer / RadioLib / SPI / GPIO dependency at all.
 */
class NullRadio : public mesh::Radio {
public:
  NullRadio() { }

  void begin() override { }

  int recvRaw(uint8_t* bytes, int sz) override {
    return 0;
  }

  uint32_t getEstAirtimeFor(int len_bytes) override {
    return 0;
  }

  float packetScore(float snr, int packet_len) override {
    return 0.0f;
  }

  bool startSendRaw(const uint8_t* bytes, int len) override {
    return true;
  }

  bool isSendComplete() override {
    return true;
  }

  void onSendFinished() override { }

  void loop() override { }

  int getNoiseFloor() const override { return 0; }

  void triggerNoiseFloorCalibrate(int threshold) override { }

  void resetAGC() override { }

  bool isInRecvMode() const override { return true; }

  bool isReceiving() override { return false; }

  float getLastRSSI() const override { return 0.0f; }
  float getLastSNR() const override { return 0.0f; }

  // ── Stats (no real radio, always zero) ───────────────────────────────────
  uint32_t getPacketsRecv() const { return 0; }
  uint32_t getPacketsSent() const { return 0; }
  uint32_t getPacketsRecvErrors() const { return 0; }
  void resetStats() { }

  // ── Parameter setters (no-ops without hardware) ──────────────────────────
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) {
    (void)freq; (void)bw; (void)sf; (void)cr;
  }

  void setTxPower(int8_t dbm) { (void)dbm; }

  // ── RNG seed (time-based fallback, no hardware noise source) ─────────────
  uint32_t getRngSeed() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec ^ tv.tv_usec);
  }
};
