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

namespace ist {

// H264 frame data passed from GStreamer to WebRTC layer
struct H264Frame {
    std::vector<std::byte> data;       // NAL unit data
    uint64_t               timestamp;  // PTS in nanoseconds
    bool                   is_keyframe;
};

// Callback type for receiving H264 frames
using FrameCallback = std::function<void(const H264Frame&)>;

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

    // Register callback for H264 frames (thread-safe)
    void on_frame(FrameCallback callback);

    // Status
    bool is_running() const { return running_.load(); }
    const CameraConfig& config() const { return config_; }
    const std::string& id() const { return config_.id; }

private:
    // GStreamer callbacks
    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data);
    static void          on_pad_added(GstElement* src, GstPad* pad, gpointer user_data);

    // Build pipeline string
    std::string build_pipeline_description() const;

    CameraConfig            config_;
    GstElement*             pipeline_  = nullptr;
    GstElement*             appsink_   = nullptr;
    std::atomic<bool>       running_{false};

    mutable std::mutex      cb_mutex_;
    std::vector<FrameCallback> callbacks_;
};

} // namespace ist
