#include "signaling_server.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ist {

SignalingServer::SignalingServer(const AppConfig& config)
    : config_(config) {
}

SignalingServer::~SignalingServer() {
    stop();
}

std::string SignalingServer::generate_client_id() {
    return "client_" + std::to_string(++client_counter_);
}

bool SignalingServer::start() {
    spdlog::info("Starting signaling server on {}:{}", config_.server.bind, config_.server.port);

    rtc::WebSocketServerConfiguration ws_config;
    ws_config.port = config_.server.port;
    ws_config.bindAddress = config_.server.bind;
    ws_config.enableTls = false;

    try {
        server_ = std::make_shared<rtc::WebSocketServer>(ws_config);
    } catch (const std::exception& e) {
        spdlog::error("Failed to create WebSocket server: {}", e.what());
        return false;
    }

    server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
        std::string client_id = generate_client_id();
        spdlog::info("New client connected: {}", client_id);

        // Check max clients
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (static_cast<int>(clients_.size()) >= config_.webrtc.max_clients) {
                spdlog::warn("Max clients ({}) reached, rejecting {}", 
                             config_.webrtc.max_clients, client_id);
                json reject;
                reject["type"] = "error";
                reject["message"] = "Server is full, maximum " + 
                                    std::to_string(config_.webrtc.max_clients) + " clients";
                ws->send(reject.dump());
                ws->close();
                return;
            }
            clients_[client_id] = ws;
        }

        ws->onOpen([this, client_id]() {
            spdlog::info("WebSocket opened for {}", client_id);

            // Send camera list to client
            json msg;
            msg["type"] = "camera_list";
            msg["cameras"] = json::array();
            for (const auto& cam : config_.cameras) {
                json cam_info;
                cam_info["id"] = cam.id;
                cam_info["name"] = cam.name;
                cam_info["width"] = cam.width;
                cam_info["height"] = cam.height;
                cam_info["fps"] = cam.fps;
                msg["cameras"].push_back(cam_info);
            }
            send_to_client(client_id, msg);
        });

        ws->onMessage([this, client_id](auto data) {
            if (auto* msg = std::get_if<std::string>(&data)) {
                handle_message(client_id, *msg);
            }
        });

        ws->onClosed([this, client_id]() {
            spdlog::info("Client disconnected: {}", client_id);
            remove_client(client_id);
        });

        ws->onError([this, client_id](std::string error) {
            spdlog::error("WebSocket error for {}: {}", client_id, error);
            remove_client(client_id);
        });

        // Notify peer manager
        if (on_connect_) {
            on_connect_(client_id, ws);
        }
    });

    spdlog::info("Signaling server started on port {}", config_.server.port);
    return true;
}

void SignalingServer::stop() {
    spdlog::info("Stopping signaling server...");
    
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [id, ws] : clients_) {
            if (ws && ws->isOpen()) {
                ws->close();
            }
        }
        clients_.clear();
    }
    
    server_.reset();
    spdlog::info("Signaling server stopped");
}

void SignalingServer::handle_message(const std::string& client_id, const std::string& message) {
    try {
        json msg = json::parse(message);
        std::string type = msg.value("type", "");

        spdlog::debug("[{}] Received: {}", client_id, type);

        if (type == "answer") {
            // Forward SDP answer to peer manager via on_connect callback
            // The peer manager handles this directly via the WebSocket
        } else if (type == "candidate") {
            // ICE candidate also handled directly
        } else if (type == "request_stream") {
            // Client is requesting video streams - handled in on_connect
            spdlog::info("[{}] Stream requested", client_id);
        } else {
            spdlog::warn("[{}] Unknown message type: {}", client_id, type);
        }
    } catch (const json::parse_error& e) {
        spdlog::error("[{}] Failed to parse message: {}", client_id, e.what());
    }
}

void SignalingServer::send_to_client(const std::string& client_id, const json& msg) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    auto it = clients_.find(client_id);
    if (it != clients_.end() && it->second && it->second->isOpen()) {
        try {
            it->second->send(msg.dump());
        } catch (const std::exception& e) {
            spdlog::error("[{}] Failed to send message: {}", client_id, e.what());
        }
    }
}

void SignalingServer::broadcast(const json& msg) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::string data = msg.dump();
    for (auto& [id, ws] : clients_) {
        if (ws && ws->isOpen()) {
            try {
                ws->send(data);
            } catch (const std::exception& e) {
                spdlog::error("[{}] Broadcast failed: {}", id, e.what());
            }
        }
    }
}

size_t SignalingServer::client_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return clients_.size();
}

void SignalingServer::remove_client(const std::string& client_id) {
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(client_id);
    }
    if (on_disconnect_) {
        on_disconnect_(client_id);
    }
}

} // namespace ist
