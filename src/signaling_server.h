/**
 * @file    signaling_server.h
 * @brief   WebSocket signaling server for WebRTC negotiation
 * @author  Yuke Brilliant Hestiavin <yukebrilliant@gmail.com>
 * @date    2026
 *
 * @copyright Copyright (c) 2026 PT Indonesia Smelting Technology (IST)
 *            All rights reserved. Internal use only.
 *
 * Provides a WebSocket server for exchanging WebRTC signaling messages
 * (SDP offers/answers, ICE candidates) between the camera server and
 * remote control room clients. Implements client lifecycle management
 * including connection limits and graceful disconnection.
 */

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

/// Callback invoked when a new client WebSocket connection is established
using ClientConnectCallback = std::function<void(const std::string& client_id,
                                                  std::shared_ptr<rtc::WebSocket> ws)>;

/// Callback invoked when a client disconnects (clean close or error)
using ClientDisconnectCallback = std::function<void(const std::string& client_id)>;

/**
 * @brief WebSocket signaling server for WebRTC session negotiation
 *
 * Manages the WebSocket server lifecycle and routes signaling messages
 * between control room clients and the PeerManager. Enforces a maximum
 * client limit to prevent resource exhaustion.
 *
 * Thread Safety:
 *   - All public methods are thread-safe
 *   - Callbacks are invoked from libdatachannel's internal threads
 */
class SignalingServer {
public:
    explicit SignalingServer(const AppConfig& config);
    ~SignalingServer();

    // Non-copyable, non-movable
    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    /**
     * @brief  Initialize and start the WebSocket server
     * @return true on success, false if the port is unavailable
     */
    bool start();

    /** @brief Gracefully close all client connections and stop the server */
    void stop();

    /** @brief Register callback for new client connections */
    void on_client_connect(ClientConnectCallback cb)       { on_connect_ = std::move(cb); }

    /** @brief Register callback for client disconnections */
    void on_client_disconnect(ClientDisconnectCallback cb)  { on_disconnect_ = std::move(cb); }

    /** @brief Send a JSON message to a specific connected client */
    void send_to_client(const std::string& client_id, const json& msg);

    /** @brief Broadcast a JSON message to all connected clients */
    void broadcast(const json& msg);

    /** @brief Get the current number of connected clients */
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

    int client_counter_ = 0;  ///< Monotonic counter for unique client IDs
};

} // namespace ist
