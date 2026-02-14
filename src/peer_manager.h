/**
 * @file    peer_manager.h
 * @brief   WebRTC peer connection lifecycle manager
 * @author  Yuke Brilliant Hestiavin <yukebrilliant@gmail.com>
 * @date    2026
 *
 * @copyright Copyright (c) 2026 PT Indonesia Smelting Technology (IST)
 *            All rights reserved. Internal use only.
 *
 * Manages the lifecycle of WebRTC PeerConnection instances for each
 * connected control room client. Handles SDP negotiation, ICE candidate
 * exchange, video track setup with H.264 RTP packetization, and proper
 * resource cleanup on disconnection to prevent memory leaks.
 */

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

/**
 * @brief Per-client WebRTC session state
 *
 * Holds all resources associated with a single control room client,
 * including the PeerConnection, video tracks, and registered frame
 * callback IDs for proper cleanup on disconnection.
 */
struct PeerContext {
    std::string                                client_id;     ///< Unique client identifier
    std::shared_ptr<rtc::PeerConnection>       peer;          ///< WebRTC peer connection
    std::shared_ptr<rtc::WebSocket>            ws;            ///< Signaling WebSocket
    std::unordered_map<std::string, std::shared_ptr<rtc::Track>> tracks;  ///< camera_id → track
    std::chrono::steady_clock::time_point      start_time;    ///< Session start time
    bool                                       ready = false; ///< True after SDP answer received

    /// Registered frame callback IDs for cleanup (camera_index, callback_id)
    std::vector<std::pair<size_t, CallbackId>> callback_ids;
};

/**
 * @brief Manages WebRTC PeerConnection lifecycle for all clients
 *
 * Creates a PeerConnection per client with one SendOnly video track per
 * camera. Handles the full WebRTC negotiation flow (offer → answer → ICE)
 * and properly cleans up frame callbacks when clients disconnect.
 *
 * Thread Safety:
 *   - All public methods are thread-safe (protected by peers_mutex_)
 *   - Frame callbacks are invoked from GStreamer pipeline threads
 */
class PeerManager {
public:
    PeerManager(const AppConfig& config,
                std::vector<std::unique_ptr<CameraPipeline>>& cameras);
    ~PeerManager();

    // Non-copyable, non-movable
    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    /**
     * @brief Create a new PeerConnection for a client
     *
     * Sets up video tracks for all cameras, configures RTP packetization,
     * registers frame callbacks, and initiates SDP offer generation.
     *
     * @param client_id  Unique client identifier
     * @param ws         Client's signaling WebSocket connection
     */
    void create_peer(const std::string& client_id, std::shared_ptr<rtc::WebSocket> ws);

    /**
     * @brief Remove a peer and clean up all associated resources
     *
     * Unregisters all frame callbacks for this peer from camera pipelines
     * to prevent callback accumulation, then closes the PeerConnection.
     *
     * @param client_id  Client to remove
     */
    void remove_peer(const std::string& client_id);

    /**
     * @brief Handle an incoming signaling message from a client
     * @param client_id  Source client identifier
     * @param msg        Parsed JSON message (type: answer, candidate, request_stream)
     */
    void handle_message(const std::string& client_id, const json& msg);

    /** @brief Get the number of active peer connections */
    size_t peer_count() const;

private:
    /// Set up video tracks with H264RtpPacketizer for each camera
    void setup_tracks(PeerContext& ctx);

    /// Generate and send SDP offer to the client
    void create_offer(std::shared_ptr<PeerContext> ctx);

    AppConfig                                     config_;
    std::vector<std::unique_ptr<CameraPipeline>>& cameras_;

    mutable std::mutex                            peers_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerContext>> peers_;
};

} // namespace ist
