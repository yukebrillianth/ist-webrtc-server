#pragma once

#include "config.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>

namespace ist {

using json = nlohmann::json;

// Callback for when a client requests a stream
using ClientConnectCallback = std::function<void(const std::string& client_id,
                                                  std::shared_ptr<rtc::WebSocket> ws)>;
using ClientDisconnectCallback = std::function<void(const std::string& client_id)>;

class SignalingServer {
public:
    explicit SignalingServer(const AppConfig& config);
    ~SignalingServer();

    // Non-copyable
    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    // Start/stop the WebSocket server
    bool start();
    void stop();

    // Set callbacks
    void on_client_connect(ClientConnectCallback cb)       { on_connect_ = std::move(cb); }
    void on_client_disconnect(ClientDisconnectCallback cb)  { on_disconnect_ = std::move(cb); }

    // Send a message to a specific client
    void send_to_client(const std::string& client_id, const json& msg);

    // Broadcast to all connected clients
    void broadcast(const json& msg);

    // Get number of connected clients
    size_t client_count() const;

private:
    void handle_message(const std::string& client_id, const std::string& message);
    void remove_client(const std::string& client_id);
    std::string generate_client_id();

    AppConfig                config_;
    std::shared_ptr<rtc::WebSocketServer> server_;

    mutable std::mutex       clients_mutex_;
    std::unordered_map<std::string, std::shared_ptr<rtc::WebSocket>> clients_;

    ClientConnectCallback    on_connect_;
    ClientDisconnectCallback on_disconnect_;

    int client_counter_ = 0;
};

} // namespace ist
