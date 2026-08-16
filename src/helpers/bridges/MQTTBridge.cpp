#include "helpers/bridges/MQTTBridge.h"
#include "HashRules.h"
#include <cstring>

void MQTTBridge::sendPacket(mesh::Packet* packet) {
  if (!_mqtt_running || !_mqttClient.connected() || !packet) return;
  if (!shouldBridgePacket(packet)) {
    _stats.tx_filtered++;
    return;
  }

  // Drop outbound RF packets whose source is on the ban list.
  if (_ban_count > 0) {
    uint8_t type = packet->getPayloadType();
    if (type == PAYLOAD_TYPE_ADVERT && packet->payload_len >= 4) {
      for (uint8_t i = 0; i < _ban_count; i++) {
        if (memcmp(_ban_prefixes[i], packet->payload, 4) == 0) {
          _stats.tx_filtered++;
          BRIDGE_DEBUG_PRINTLN("TX drop banned advert\n");
          return;
        }
      }
    } else {
      uint8_t src = getSourceHash(packet);
      if (src != 0xFF) {
        for (uint8_t i = 0; i < _ban_count; i++) {
          if (_ban_prefixes[i][0] == src) {
            _stats.tx_filtered++;
            BRIDGE_DEBUG_PRINTLN("TX drop banned src %02x\n", src);
            return;
          }
        }
      }
    }
  }

  // --- new: HashRules check for outbound packet (origin: lora -> mqtt path)
  uint8_t dest_full[32]; memset(dest_full, 0, sizeof(dest_full));
  if (packet->payload_len >= 1) {
    int n = packet->payload_len >= 3 ? 3 : packet->payload_len;
    memcpy(dest_full, packet->payload, n);
  }
  auto chk = HashRules::instance().isAllowed(dest_full, sizeof(dest_full), "mqtt");
  if (!chk.allowed) {
    _stats.tx_filtered++;
    BRIDGE_DEBUG_PRINTLN("TX blocked by hashrules (%s)\n", chk.matched_pattern.c_str());
    return;
  }

  if (!_seen_packets.hasSeen(packet)) {
    uint16_t len = packet->writeTo(_tx_buffer);
    if (len > 0 && len <= (uint16_t)MAX_MQTT_PAYLOAD) {
      const char* topic = _prefs->mqtt_topic[0] ? _prefs->mqtt_topic : WITH_MQTT_BRIDGE_TOPIC;
      if (_mqttClient.publish(topic, _tx_buffer, len, /*retain=*/false)) {
        _stats.tx_packets++;
        BRIDGE_DEBUG_PRINTLN("TX %d bytes\n", (int)len);
      }
    }
  }
}

void MQTTBridge::handleMQTTMessage(const uint8_t* payload, unsigned int length) {
  if (length == 0 || length > MAX_MQTT_PAYLOAD) {
    BRIDGE_DEBUG_PRINTLN("RX invalid length %d, dropping\n", (int)length);
    return;
  }

  if (length == BAN_CMD_LEN &&
      payload[0] == BAN_CMD_MAGIC[0] &&
      payload[1] == BAN_CMD_MAGIC[1] &&
      payload[2] == BAN_CMD_MAGIC[2]) {
    const uint8_t* target = &payload[3]; // 4-byte pubkey prefix
    if (_app_cb && memcmp(_app_cb->getSelfId().pub_key, target, 4) == 0) {
      BRIDGE_DEBUG_PRINTLN("BAN: received ban command targeting self (%02x%02x%02x%02x)\n",
        target[0], target[1], target[2], target[3]);
      _deferred_self_ban = true; // execute after the MQTT callback returns
    }
    return; // never forward to mesh
  }

  mesh::Packet* pkt = _mgr->allocNew();
  if (!pkt) {
    BRIDGE_DEBUG_PRINTLN("RX failed to allocate packet\n");
    return;
  }

  if (pkt->readFrom(payload, (uint8_t)length)) {
    // Drop packets from banned sources before injecting into the local mesh.
    if (_ban_count > 0) {
      uint8_t type = pkt->getPayloadType();
      bool drop = false;
      if (type == PAYLOAD_TYPE_ADVERT && pkt->payload_len >= 4) {
        for (uint8_t i = 0; i < _ban_count; i++) {
          if (memcmp(_ban_prefixes[i], pkt->payload, 4) == 0) { drop = true; break; }
        }
      } else {
        uint8_t src = getSourceHash(pkt);
        if (src != 0xFF) {
          for (uint8_t i = 0; i < _ban_count; i++) {
            if (_ban_prefixes[i][0] == src) { drop = true; break; }
          }
        }
      }
      if (drop) {
        BRIDGE_DEBUG_PRINTLN("RX drop banned source\n");
        _stats.rx_banned++;
        _mgr->free(pkt);
        return;
      }
    }

    // --- new: HashRules check for inbound packet (origin: mqtt -> lora path)
    uint8_t src_full[32]; memset(src_full, 0, sizeof(src_full));
    if (pkt->payload_len >= 2) {
      int n = pkt->payload_len >= 3 ? 3 : pkt->payload_len;
      memcpy(src_full, pkt->payload, n);
    }
    auto chk_rx = HashRules::instance().isAllowed(src_full, sizeof(src_full), "lora");
    if (!chk_rx.allowed) {
      _stats.rx_banned++;
      BRIDGE_DEBUG_PRINTLN("RX blocked by hashrules (%s)\n", chk_rx.matched_pattern.c_str());
      _mgr->free(pkt);
      return;
    }

    BRIDGE_DEBUG_PRINTLN("RX %d bytes\n", (int)length);
    _stats.rx_packets++;
    onPacketReceived(pkt);
  } else {
    BRIDGE_DEBUG_PRINTLN("RX failed to parse packet, dropping\n");
    _mgr->free(pkt);
  }
}
