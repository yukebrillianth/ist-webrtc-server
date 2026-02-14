#pragma once

#include "config.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>

namespace ist {

// H264 frame data passed from GStreamer to WebRTC layer
struct H264Frame {
    std::vector<std::byte> data;       // NAL unit data
    uint64_t               timestamp;  // PTS in nanoseconds
    bool                   is_keyframe;
};

// Callback with ID for lifecycle management
using FrameCallback = std::function<void(const H264Frame&)>;
using CallbackId    = uint64_t;

class CameraPipeline {
public:
    explicit CameraPipeline(const CameraConfig& config);
    ~CameraPipeline();

    // Non-copyable
    CameraPipeline(const CameraPipeline&) = delete;
    CameraPipeline& operator=(const CameraPipeline&) = delete;

    // Start/stop pipeline
    bool start();
    void stop();

    // Register callback for H264 frames, returns ID for unregistering
    CallbackId on_frame(FrameCallback callback);

    // Unregister a specific callback by ID
    void remove_callback(CallbackId id);

    // Remove all callbacks (used on shutdown)
    void clear_callbacks();

    // Status & health
    bool is_running() const { return running_.load(); }
    const CameraConfig& config() const { return config_; }
    const std::string& id() const { return config_.id; }

    // Health metrics
    uint64_t frame_count() const { return frame_count_.load(); }
    double seconds_since_last_frame() const;
    int restart_count() const { return restart_count_.load(); }

private:
    // GStreamer callbacks
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);

    // Build pipeline string
    std::string build_pipeline_description() const;

    // Pipeline lifecycle (internal)
    bool launch_pipeline();
    void destroy_pipeline();

    // GStreamer bus monitoring thread
    void bus_monitor_thread();

    // Auto-restart with exponential backoff
    void schedule_restart();

    CameraConfig            config_;
    GstElement*             pipeline_  = nullptr;
    GstElement*             appsink_   = nullptr;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       shutdown_{false};  // true = permanent stop, no restart

    // Frame callbacks with IDs
    mutable std::mutex      cb_mutex_;
    struct CallbackEntry {
        CallbackId    id;
        FrameCallback fn;
    };
    std::vector<CallbackEntry> callbacks_;
    std::atomic<CallbackId>    next_cb_id_{1};

    // Health metrics
    std::atomic<uint64_t>   frame_count_{0};
    std::atomic<std::chrono::steady_clock::time_point> last_frame_time_{std::chrono::steady_clock::now()};

    // Auto-recovery
    std::atomic<int>        restart_count_{0};
    std::thread             bus_thread_;
    int                     backoff_seconds_ = 1;
    static constexpr int    kMaxBackoff = 30;
};

} // namespace ist
