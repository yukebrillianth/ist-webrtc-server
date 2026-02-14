/**
 * @file    peer_manager.cpp
 * @brief   WebRTC peer connection lifecycle manager implementation
 * @author  Yuke Brilliant Hestiavin <yukebrilliant@gmail.com>
 * @date    2026
 *
 * @copyright Copyright (c) 2026 PT Indonesia Smelting Technology (IST)
 *            All rights reserved. Internal use only.
 */

#include "peer_manager.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace ist {

PeerManager::PeerManager(const AppConfig& config,
                         std::vector<std::unique_ptr<CameraPipeline>>& cameras)
    : config_(config), cameras_(cameras) {
}

PeerManager::~PeerManager() {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (auto& [id, ctx] : peers_) {
        // Unregister all frame callbacks
        for (auto& [cam_idx, cb_id] : ctx->callback_ids) {
            if (cam_idx < cameras_.size()) {
                cameras_[cam_idx]->remove_callback(cb_id);
            }
        }
        if (ctx->peer) {
            ctx->peer->close();
        }
    }
    peers_.clear();
}

void PeerManager::create_peer(const std::string& client_id, std::shared_ptr<rtc::WebSocket> ws) {
    spdlog::info("[{}] Creating peer connection", client_id);

    auto ctx = std::make_shared<PeerContext>();
    ctx->client_id  = client_id;
    ctx->ws         = ws;
    ctx->start_time = std::chrono::steady_clock::now();

    // Configure PeerConnection
    rtc::Configuration rtc_config;
    if (!config_.webrtc.stun_server.empty()) {
        rtc_config.iceServers.emplace_back(config_.webrtc.stun_server);
    }
    rtc_config.disableAutoNegotiation = true;

    ctx->peer = std::make_shared<rtc::PeerConnection>(rtc_config);

    // ICE state change
    ctx->peer->onStateChange([client_id](rtc::PeerConnection::State state) {
        spdlog::info("[{}] PeerConnection state: {}", client_id, static_cast<int>(state));
    });

    ctx->peer->onGatheringStateChange([client_id, ws_weak = std::weak_ptr(ws)]
                                       (rtc::PeerConnection::GatheringState state) {
        spdlog::debug("[{}] Gathering state: {}", client_id, static_cast<int>(state));
        
        // Send end-of-candidates when gathering complete
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            if (auto ws = ws_weak.lock()) {
                json msg;
                msg["type"] = "candidate";
                msg["candidate"] = nullptr;  // null = end of candidates
                try {
                    ws->send(msg.dump());
                } catch (...) {}
            }
        }
    });

    // ICE candidate
    ctx->peer->onLocalCandidate([client_id, ws_weak = std::weak_ptr(ws)]
                                 (rtc::Candidate candidate) {
        if (auto ws = ws_weak.lock()) {
            json msg;
            msg["type"]      = "candidate";
            msg["candidate"] = std::string(candidate);
            msg["sdpMid"]    = candidate.mid();
            try {
                ws->send(msg.dump());
            } catch (const std::exception& e) {
                spdlog::error("[{}] Failed to send candidate: {}", client_id, e.what());
            }
        }
    });

    // Handle incoming messages from this client's WebSocket
    ws->onMessage([this, client_id](auto data) {
        if (auto* msg_str = std::get_if<std::string>(&data)) {
            try {
                json msg = json::parse(*msg_str);
                handle_message(client_id, msg);
            } catch (const json::parse_error& e) {
                spdlog::error("[{}] JSON parse error: {}", client_id, e.what());
            }
        }
    });

    // Track open callback
    ctx->peer->onTrack([client_id](std::shared_ptr<rtc::Track> track) {
        spdlog::info("[{}] Track opened: {}", client_id, track->mid());
    });

    // Store peer context
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers_[client_id] = ctx;
    }

    // Set up video tracks and create offer
    setup_tracks(*ctx);
    create_offer(ctx);
}

void PeerManager::setup_tracks(PeerContext& ctx) {
    spdlog::info("[{}] Setting up {} video tracks", ctx.client_id, cameras_.size());

    for (size_t i = 0; i < cameras_.size(); i++) {
        auto& camera = cameras_[i];
        const auto& cam_config = camera->config();

        uint32_t ssrc = static_cast<uint32_t>(1000 + i);
        uint8_t payloadType = static_cast<uint8_t>(96 + i);

        // Create video track description
        rtc::Description::Video media(cam_config.id, rtc::Description::Direction::SendOnly);
        media.addH264Codec(payloadType);
        media.addSSRC(ssrc, cam_config.id);

        auto track = ctx.peer->addTrack(media);

        // Set up RTP packetization config + H264 packetizer
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc,
            cam_config.id,
            payloadType,
            rtc::H264RtpPacketizer::defaultClockRate       // 90000 Hz
        );

        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::LongStartSequence,
            rtpConfig
        );

        track->setMediaHandler(packetizer);

        ctx.tracks[cam_config.id] = track;

        spdlog::info("[{}] Added track for camera '{}' (mid={}, ssrc={}, pt={})",
                     ctx.client_id, cam_config.id, track->mid(), ssrc, payloadType);

        // Register frame callback with ID tracking for cleanup
        auto track_weak = std::weak_ptr(track);
        auto rtp_config_weak = std::weak_ptr(rtpConfig);
        auto start_time = ctx.start_time;

        CallbackId cb_id = camera->on_frame(
            [track_weak, rtp_config_weak, start_time,
             cam_id = cam_config.id](const H264Frame& frame) {
                auto track = track_weak.lock();
                if (!track || !track->isOpen()) return;

                auto rtpConfig = rtp_config_weak.lock();
                if (!rtpConfig) return;

                // Calculate RTP timestamp (90kHz clock from elapsed time)
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
                rtpConfig->timestamp = static_cast<uint32_t>(elapsed_us * 90 / 1000);

                try {
                    track->send(
                        reinterpret_cast<const std::byte*>(frame.data.data()),
                        frame.data.size()
                    );
                } catch (const std::exception& e) {
                    spdlog::warn("[{}] Failed to send frame: {}", cam_id, e.what());
                }
            }
        );

        // Store callback ID for cleanup when peer disconnects
        ctx.callback_ids.push_back({i, cb_id});
    }
}

void PeerManager::create_offer(std::shared_ptr<PeerContext> ctx) {
    spdlog::info("[{}] Creating SDP offer", ctx->client_id);

    // CRITICAL: set onLocalDescription callback BEFORE setLocalDescription
    ctx->peer->onLocalDescription([ctx](rtc::Description desc) {
        json msg;
        msg["type"] = "offer";
        msg["sdp"]  = std::string(desc);
        
        spdlog::info("[{}] Sending SDP offer ({} bytes)",
                     ctx->client_id, msg["sdp"].get<std::string>().size());
        try {
            if (ctx->ws && ctx->ws->isOpen()) {
                ctx->ws->send(msg.dump());
            } else {
                spdlog::error("[{}] WebSocket not open when sending offer", ctx->client_id);
            }
        } catch (const std::exception& e) {
            spdlog::error("[{}] Failed to send offer: {}", ctx->client_id, e.what());
        }
    });

    // Now trigger offer generation
    ctx->peer->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerManager::handle_message(const std::string& client_id, const json& msg) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(client_id);
    if (it == peers_.end()) {
        spdlog::warn("[{}] Peer not found for message", client_id);
        return;
    }

    auto& ctx = it->second;
    std::string type = msg.value("type", "");

    if (type == "answer") {
        std::string sdp = msg.value("sdp", "");
        if (!sdp.empty()) {
            spdlog::info("[{}] Received SDP answer", client_id);
            try {
                rtc::Description answer(sdp, rtc::Description::Type::Answer);
                ctx->peer->setRemoteDescription(answer);
                ctx->ready = true;
            } catch (const std::exception& e) {
                spdlog::error("[{}] Failed to set answer: {}", client_id, e.what());
            }
        }
    } else if (type == "candidate") {
        if (msg.contains("candidate") && !msg["candidate"].is_null()) {
            std::string candidate = msg["candidate"].get<std::string>();
            std::string mid = msg.value("sdpMid", "");
            spdlog::debug("[{}] Adding ICE candidate", client_id);
            try {
                ctx->peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
            } catch (const std::exception& e) {
                spdlog::error("[{}] Failed to add candidate: {}", client_id, e.what());
            }
        }
    } else if (type == "request_stream") {
        spdlog::info("[{}] Client requesting stream (peer already created)", client_id);
    }
}

void PeerManager::remove_peer(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(client_id);
    if (it != peers_.end()) {
        spdlog::info("[{}] Removing peer (cleaning up {} callbacks)",
                     client_id, it->second->callback_ids.size());

        // Unregister all frame callbacks for this peer
        for (auto& [cam_idx, cb_id] : it->second->callback_ids) {
            if (cam_idx < cameras_.size()) {
                cameras_[cam_idx]->remove_callback(cb_id);
            }
        }

        if (it->second->peer) {
            it->second->peer->close();
        }
        peers_.erase(it);
    }
}

size_t PeerManager::peer_count() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_.size();
}

} // namespace ist
