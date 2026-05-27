#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include "mqtt/MQTTBridge.h"

// Forward declaration
class Mesh;

/**
 * @brief Integration layer between MeshCore and MQTT Bridge
 * 
 * This class handles bidirectional message forwarding:
 * 
 * INBOUND: Mesh Packets → MQTT
 *   1. MeshCore receives encrypted packet from LoRa radio
 *   2. Packet is decrypted and validated
 *   3. MeshCoreIntegration::onPacketReceived() is called by Mesh class
 *   4. Extracts hash destination from packet
 *   5. Routes through MQTT bridge to MQTT broker
 * 
 * OUTBOUND: MQTT → Mesh Packets
 *   1. Application publishes to MQTT topic
 *   2. MQTT bridge receives and queues message
 *   3. MeshCoreIntegration::loop() polls outbound queue
 *   4. Creates mesh packet with destination hash
 *   5. Sends to MeshCore dispatcher
 * 
 * Configuration is loaded from meshcored.ini [mqtt] section
 */
class MeshCoreIntegration {
private:
    Mesh* _mesh;
    std::shared_ptr<mqtt::MQTTBridge> _mqtt_bridge;
    bool _initialized;
    
    /**
     * @brief Extract destination hash from mesh packet
     * @param payload Mesh packet payload
     * @param payload_len Payload length
     * @param hash_size Output: detected hash size
     * @param hash Output buffer for hash (must be 3 bytes)
     * @return true if hash extracted successfully
     */
    bool extractHashFromPacket(const uint8_t* payload, size_t payload_len,
                               mqtt::HashSize& hash_size, uint8_t* hash) const;
    
public:
    /**
     * @brief Constructor
     * @param mesh Reference to the Mesh instance
     * @param config MQTT configuration
     */
    MeshCoreIntegration(Mesh* mesh, const mqtt::MQTTConfig& config);
    
    /**
     * @brief Destructor
     */
    ~MeshCoreIntegration();
    
    /**
     * @brief Initialize the integration
     * Starts MQTT bridge and subscribes to routes
     * @param routes Route definitions from config parser
     * @return true if successful
     */
    bool begin();
    
    /**
     * @brief Shut down the integration
     */
    void end();
    
    /**
     * @brief Main loop - processes queued messages
     * Should be called regularly from main mesh loop
     * Polls MQTT outbound queue and creates mesh packets
     */
    void loop();
    
    /**
     * @brief Called when MeshCore receives a decrypted packet
     * Routes the packet to MQTT if a matching route exists
     * 
     * @param payload Decrypted packet payload
     * @param payload_len Length of payload
     * @param sender_node_id ID of sending node (optional)
     * @return true if packet was routed to MQTT
     */
    bool onPacketReceived(const uint8_t* payload, size_t payload_len,
                         const uint8_t* sender_node_id = nullptr);
    
    /**
     * @brief Add a hash-to-topic route
     * @param hash_size Size of hash (1, 2, or 3 bytes)
     * @param hash Hash bytes
     * @param topic MQTT topic
     * @param direction Publish/Subscribe/Bidirectional
     * @return true if added successfully
     */
    bool addRoute(mqtt::HashSize hash_size, const uint8_t* hash,
                  const std::string& topic, mqtt::HashRoute::Direction direction);
    
    /**
     * @brief Check if integration is active
     * @return true if initialized and ready
     */
    bool isActive() const { return _initialized; }
    
    /**
     * @brief Get the MQTT bridge
     * @return Pointer to MQTT bridge
     */
    mqtt::MQTTBridge* getBridge() { return _mqtt_bridge.get(); }
    
    /**
     * @brief Get queue counts for monitoring
     * @return Outbound message count waiting to be sent
     */
    size_t getOutboundQueueCount() const;
    
    /**
     * @brief Get inbound message count
     * @return Number of messages received from MQTT waiting for processing
     */
    size_t getInboundQueueCount() const;
};
