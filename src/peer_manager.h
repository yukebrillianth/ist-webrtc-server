#pragma once

#include "config.h"
#include "camera_pipeline.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <chrono>

namespace ist {

using json = nlohmann::json;

struct PeerContext {
    std::string                                client_id;
    std::shared_ptr<rtc::PeerConnection>       peer;
    std::shared_ptr<rtc::WebSocket>            ws;
    std::unordered_map<std::string, std::shared_ptr<rtc::Track>> tracks;  // camera_id -> track
    std::chrono::steady_clock::time_point      start_time;
    bool                                       ready = false;

    // Callback IDs for cleanup on disconnect
    std::vector<std::pair<size_t, CallbackId>> callback_ids;  // camera_index -> callback_id
};

class PeerManager {
public:
    PeerManager(const AppConfig& config,
                std::vector<std::unique_ptr<CameraPipeline>>& cameras);
    ~PeerManager();

    // Non-copyable
    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    // Create a new peer connection for a client
    void create_peer(const std::string& client_id, std::shared_ptr<rtc::WebSocket> ws);

    // Remove a peer (cleans up callbacks)
    void remove_peer(const std::string& client_id);

    // Handle incoming WebSocket messages (answer, candidate)
    void handle_message(const std::string& client_id, const json& msg);

    // Get active peer count
    size_t peer_count() const;

private:
    void setup_tracks(PeerContext& ctx);
    void create_offer(std::shared_ptr<PeerContext> ctx);

    AppConfig                                     config_;
    std::vector<std::unique_ptr<CameraPipeline>>& cameras_;

    mutable std::mutex                            peers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerContext>> peers_;
};

} // namespace ist
