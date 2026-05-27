#include "MQTTBridge.h"
#include <ctime>
#include <cstring>
#include <iostream>

// Paho MQTT C++ client includes
#include "mqtt/async_client.h"
#include "mqtt/connect_options.h"

namespace mqtt {

// Forward declaration of callback class
class MQTTCallback : public mqtt::callback {
private:
    MQTTBridge* _bridge;

public:
    MQTTCallback(MQTTBridge* bridge) : _bridge(bridge) {}

    void connected(const std::string& cause) override {
        std::cout << "MQTT Connected: " << cause << std::endl;
    }

    void connection_lost(const std::string& cause) override {
        std::cout << "MQTT Connection lost: " << cause << std::endl;
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        if (!_bridge) return;
        
        const std::string& topic = msg->get_topic();
        const auto& payload = msg->get_payload();
        
        // Find matching route for this topic
        for (const auto& route : _bridge->_routes) {
            if (route.topic == topic) {
                if (route.direction == HashRoute::Direction::SUBSCRIBE ||
                    route.direction == HashRoute::Direction::BIDIRECTIONAL) {
                    
                    // Enqueue as inbound message
                    _bridge->enqueueInbound(
                        route.hash_size,
                        reinterpret_cast<const uint8_t*>(&route.hash),
                        reinterpret_cast<const uint8_t*>(payload.data()),
                        payload.size(),
                        topic
                    );
                }
                break;
            }
        }
    }

    void delivery_complete(mqtt::delivery_token_ptr token) override {
        std::cout << "MQTT Delivery complete for token: " << token->get_message_id() << std::endl;
    }
};

MQTTBridge::MQTTBridge(const MQTTConfig& config)
    : _config(config), _initialized(false), _last_cleanup(0), _client(nullptr), _conn_opts(nullptr) {
}

MQTTBridge::~MQTTBridge() {
    end();
}

bool MQTTBridge::begin() {
    if (_initialized) {
        return true; // already initialized
    }
    
    if (!_config.enabled) {
        return false; // MQTT not enabled in config
    }
    
    if (_config.broker_host.empty() || _config.client_id.empty()) {
        return false; // missing required configuration
    }
    
    try {
        // Construct broker address
        std::string broker_uri = "tcp://" + _config.broker_host + ":" + 
                                std::to_string(_config.broker_port);
        
        std::cout << "Connecting to MQTT broker: " << broker_uri << std::endl;
        
        // Create MQTT client
        _client = std::make_shared<mqtt::async_client>(broker_uri, _config.client_id);
        
        // Create connection options
        _conn_opts = std::make_shared<mqtt::connect_options>();
        _conn_opts->set_clean_session(_config.clean_session);
        _conn_opts->set_keep_alive_interval(_config.keepalive_seconds);
        
        // Set credentials if provided
        if (!_config.username.empty()) {
            _conn_opts->set_user_name(_config.username);
            if (!_config.password.empty()) {
                _conn_opts->set_password(_config.password);
            }
        }
        
        // Set TLS/SSL if configured
        if (_config.use_tls) {
            mqtt::ssl_options ssl_opts;
            if (!_config.tls_ca_file.empty()) {
                ssl_opts.set_trust_store(_config.tls_ca_file);
            }
            _conn_opts->set_ssl(ssl_opts);
        }
        
        // Set callback for connection events
        auto callback = std::make_shared<MQTTCallback>(this);
        _client->set_callback(*callback);
        
        // Connect to broker
        auto token = _client->connect(*_conn_opts);
        
        try {
            // Wait for connection (timeout 5 seconds)
            token->wait_for(std::chrono::seconds(5));
            
            if (!token->is_complete() || token->get_exception()) {
                std::cerr << "Failed to connect to MQTT broker" << std::endl;
                _client->disconnect();
                _client = nullptr;
                return false;
            }
        } catch (const mqtt::exception& exc) {
            std::cerr << "MQTT connection error: " << exc.what() << std::endl;
            _client = nullptr;
            return false;
        }
        
        // Subscribe to configured topics
        subscribeToTopics();
        
        std::cout << "MQTT Bridge initialized successfully" << std::endl;
        _initialized = true;
        return true;
        
    } catch (const mqtt::exception& exc) {
        std::cerr << "MQTT initialization error: " << exc.what() << std::endl;
        _client = nullptr;
        _conn_opts = nullptr;
        return false;
    }
}

void MQTTBridge::subscribeToTopics() {
    if (!_client || !_client->is_connected()) {
        return;
    }
    
    for (const auto& route : _routes) {
        if (route.direction == HashRoute::Direction::SUBSCRIBE ||
            route.direction == HashRoute::Direction::BIDIRECTIONAL) {
            try {
                _client->subscribe(route.topic, 1); // QoS 1
                std::cout << "Subscribed to MQTT topic: " << route.topic << std::endl;
            } catch (const mqtt::exception& exc) {
                std::cerr << "Failed to subscribe to topic " << route.topic 
                         << ": " << exc.what() << std::endl;
            }
        }
    }
}

void MQTTBridge::end() {
    if (!_initialized) {
        return;
    }
    
    // Disconnect MQTT client
    if (_client && _client->is_connected()) {
        try {
            _client->disconnect();
        } catch (const mqtt::exception& exc) {
            std::cerr << "Error disconnecting from MQTT: " << exc.what() << std::endl;
        }
    }
    
    _client = nullptr;
    _conn_opts = nullptr;
    
    // Clear any remaining messages
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        
        while (!_outbound_queue.empty()) {
            auto msg = _outbound_queue.front();
            _outbound_queue.pop();
            delete msg;
        }
        
        while (!_inbound_queue.empty()) {
            auto msg = _inbound_queue.front();
            _inbound_queue.pop();
            delete msg;
        }
    }
    
    std::cout << "MQTT Bridge shut down" << std::endl;
    _initialized = false;
}

void MQTTBridge::loop() {
    if (!_initialized) {
        return;
    }
    
    // Handle MQTT client connection management
    if (_client) {
        try {
            // Check if connected, attempt to reconnect if not
            if (!_client->is_connected()) {
                std::cout << "MQTT connection lost, attempting to reconnect..." << std::endl;
                try {
                    auto token = _client->connect(*_conn_opts);
                    token->wait_for(std::chrono::seconds(5));
                    
                    if (token->is_complete() && !token->get_exception()) {
                        std::cout << "MQTT reconnected successfully" << std::endl;
                        subscribeToTopics();
                    }
                } catch (const mqtt::exception& exc) {
                    std::cerr << "MQTT reconnection error: " << exc.what() << std::endl;
                }
            }
        } catch (const mqtt::exception& exc) {
            std::cerr << "Error in MQTT loop: " << exc.what() << std::endl;
        }
    }
    
    // Process and forward outbound messages to MQTT
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        
        while (!_outbound_queue.empty()) {
            auto msg = _outbound_queue.front();
            
            // Find route for this hash
            const HashRoute* route = findRoute(msg->hash_size, 
                reinterpret_cast<uint8_t*>(&msg->destination_hash));
            
            if (route && (route->direction == HashRoute::Direction::PUBLISH ||
                         route->direction == HashRoute::Direction::BIDIRECTIONAL)) {
                
                // Publish to MQTT
                if (_client && _client->is_connected()) {
                    try {
                        mqtt::message_ptr mqtt_msg = mqtt::make_message(
                            route->topic,
                            msg->payload,
                            msg->payload_len,
                            1, // QoS 1
                            false // retain
                        );
                        
                        _client->publish(mqtt_msg);
                        std::cout << "Published message to MQTT topic: " << route->topic << std::endl;
                    } catch (const mqtt::exception& exc) {
                        std::cerr << "Error publishing to MQTT: " << exc.what() << std::endl;
                    }
                }
            }
            
            _outbound_queue.pop();
            delete msg;
        }
    }
    
    // Periodically cleanup expired messages (every 60 seconds)
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (now - _last_cleanup > 60) {
        cleanupExpiredMessages();
        _last_cleanup = now;
    }
}

bool MQTTBridge::addRoute(HashSize hash_size, const uint8_t* hash,
                         const std::string& topic,
                         HashRoute::Direction direction) {
    if (!hash || (hash_size != HashSize::BYTES_1 && 
                  hash_size != HashSize::BYTES_2 && 
                  hash_size != HashSize::BYTES_3)) {
        return false;
    }
    
    if (topic.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(_route_mutex);
    
    // Check if route already exists for this hash
    for (auto& route : _routes) {
        if (route.matches(hash_size, hash)) {
            // Update existing route
            route.topic = topic;
            route.direction = direction;
            
            // If bridge is initialized and this is a subscribe route, subscribe
            if (_initialized && _client && _client->is_connected()) {
                if (direction == HashRoute::Direction::SUBSCRIBE ||
                    direction == HashRoute::Direction::BIDIRECTIONAL) {
                    try {
                        _client->subscribe(topic, 1);
                        std::cout << "Subscribed to new MQTT topic: " << topic << std::endl;
                    } catch (const mqtt::exception& exc) {
                        std::cerr << "Failed to subscribe: " << exc.what() << std::endl;
                    }
                }
            }
            
            return true;
        }
    }
    
    // Add new route
    HashRoute route;
    route.hash_size = hash_size;
    route.direction = direction;
    route.topic = topic;
    
    size_t hash_len = static_cast<size_t>(hash_size);
    if (hash_len == 1) {
        route.hash.hash_1b = hash[0];
    } else if (hash_len == 2) {
        route.hash.hash_2b = (hash[0] << 8) | hash[1];
    } else { // hash_len == 3
        route.hash.hash_3b[0] = hash[0];
        route.hash.hash_3b[1] = hash[1];
        route.hash.hash_3b[2] = hash[2];
    }
    
    _routes.push_back(route);
    
    // If bridge is initialized and this is a subscribe route, subscribe immediately
    if (_initialized && _client && _client->is_connected()) {
        if (direction == HashRoute::Direction::SUBSCRIBE ||
            direction == HashRoute::Direction::BIDIRECTIONAL) {
            try {
                _client->subscribe(topic, 1);
                std::cout << "Subscribed to new MQTT topic: " << topic << std::endl;
            } catch (const mqtt::exception& exc) {
                std::cerr << "Failed to subscribe: " << exc.what() << std::endl;
            }
        }
    }
    
    return true;
}

bool MQTTBridge::enqueueOutbound(HashSize hash_size, const uint8_t* hash,
                                 const uint8_t* payload, size_t payload_len) {
    if (!hash || !payload || payload_len == 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(_queue_mutex);
    
    if (_outbound_queue.size() >= _config.outbound_queue_size) {
        return false; // queue full
    }
    
    // Create new message
    auto msg = new MQTTMessage();
    msg->hash_size = hash_size;
    msg->is_outbound = true;
    msg->timestamp = static_cast<uint32_t>(time(nullptr));
    
    // Set hash
    if (!msg->setHash(hash, static_cast<size_t>(hash_size))) {
        delete msg;
        return false;
    }
    
    // Copy payload
    msg->payload = new uint8_t[payload_len];
    if (!msg->payload) {
        delete msg;
        return false;
    }
    memcpy(msg->payload, payload, payload_len);
    msg->payload_len = payload_len;
    
    _outbound_queue.push(msg);
    return true;
}

bool MQTTBridge::dequeueOutbound(MQTTMessage*& out_msg) {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    
    if (_outbound_queue.empty()) {
        return false;
    }
    
    out_msg = _outbound_queue.front();
    _outbound_queue.pop();
    return true;
}

bool MQTTBridge::enqueueInbound(HashSize hash_size, const uint8_t* hash,
                                const uint8_t* payload, size_t payload_len,
                                const std::string& source_topic) {
    if (!hash || !payload || payload_len == 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(_queue_mutex);
    
    if (_inbound_queue.size() >= _config.inbound_queue_size) {
        return false; // queue full
    }
    
    // Create new message
    auto msg = new MQTTMessage();
    msg->hash_size = hash_size;
    msg->is_outbound = false;
    msg->timestamp = static_cast<uint32_t>(time(nullptr));
    msg->source_topic = source_topic;
    
    // Set hash
    if (!msg->setHash(hash, static_cast<size_t>(hash_size))) {
        delete msg;
        return false;
    }
    
    // Copy payload
    msg->payload = new uint8_t[payload_len];
    if (!msg->payload) {
        delete msg;
        return false;
    }
    memcpy(msg->payload, payload, payload_len);
    msg->payload_len = payload_len;
    
    _inbound_queue.push(msg);
    return true;
}

bool MQTTBridge::dequeueInbound(MQTTMessage*& out_msg) {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    
    if (_inbound_queue.empty()) {
        return false;
    }
    
    out_msg = _inbound_queue.front();
    _inbound_queue.pop();
    return true;
}

const HashRoute* MQTTBridge::findRoute(HashSize hash_size, const uint8_t* hash) const {
    if (!hash) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(_route_mutex);
    
    for (const auto& route : _routes) {
        if (route.matches(hash_size, hash)) {
            return &route;
        }
    }
    
    return nullptr;
}

void MQTTBridge::cleanupExpiredMessages() {
    uint32_t now = static_cast<uint32_t>(time(nullptr));
    uint32_t ttl = _config.message_ttl_seconds;
    
    std::lock_guard<std::mutex> lock(_queue_mutex);
    
    // Clean outbound queue
    std::queue<MQTTMessage*> cleaned_outbound;
    while (!_outbound_queue.empty()) {
        auto msg = _outbound_queue.front();
        _outbound_queue.pop();
        
        if (now - msg->timestamp < ttl) {
            cleaned_outbound.push(msg);
        } else {
            delete msg; // expired
            std::cout << "Cleaned up expired outbound message" << std::endl;
        }
    }
    _outbound_queue = cleaned_outbound;
    
    // Clean inbound queue
    std::queue<MQTTMessage*> cleaned_inbound;
    while (!_inbound_queue.empty()) {
        auto msg = _inbound_queue.front();
        _inbound_queue.pop();
        
        if (now - msg->timestamp < ttl) {
            cleaned_inbound.push(msg);
        } else {
            delete msg; // expired
            std::cout << "Cleaned up expired inbound message" << std::endl;
        }
    }
    _inbound_queue = cleaned_inbound;
}

size_t MQTTBridge::getOutboundCount() const {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    return _outbound_queue.size();
}

size_t MQTTBridge::getInboundCount() const {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    return _inbound_queue.size();
}

size_t MQTTBridge::getRouteCount() const {
    std::lock_guard<std::mutex> lock(_route_mutex);
    return _routes.size();
}

bool MQTTBridge::isConnected() const {
    if (!_client) {
        return false;
    }
    return _client->is_connected();
}

} // namespace mqtt
