#include "camera_pipeline.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace ist {

CameraPipeline::CameraPipeline(const CameraConfig& config)
    : config_(config) {
}

CameraPipeline::~CameraPipeline() {
    stop();
}

std::string CameraPipeline::build_pipeline_description() const {
    std::string desc;

    switch (config_.type) {
    case CameraType::RTSP:
        // RTSP cameras: depay H264 and pass through
        // tcp-timeout: 5s for faster disconnect detection
        desc = "rtspsrc location=" + config_.uri +
               " latency=0 protocols=tcp"
               " tcp-timeout=5000000"
               " retry=3"
               " ! rtph264depay"
               " ! h264parse config-interval=-1"
               " ! video/x-h264,stream-format=byte-stream,alignment=au"
               " ! appsink name=sink emit-signals=true sync=false"
               " max-buffers=2 drop=true";
        break;

    case CameraType::USB:
        // USB cameras need encoding
        desc = "v4l2src device=" + config_.uri +
               " ! video/x-raw,width=" + std::to_string(config_.width) +
               ",height=" + std::to_string(config_.height) +
               ",framerate=" + std::to_string(config_.fps) + "/1"
               " ! videoconvert"
               " ! x264enc tune=zerolatency bitrate=" + std::to_string(config_.bitrate) +
               " speed-preset=ultrafast"
               " key-int-max=" + std::to_string(config_.fps * 2) +
               " bframes=0 b-adapt=false"
               " sliced-threads=true threads=" + std::to_string(std::max(1u, std::thread::hardware_concurrency() / 4)) +
               " ! video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline"
               " ! h264parse config-interval=-1"
               " ! appsink name=sink emit-signals=true sync=false"
               " max-buffers=2 drop=true";
        break;

    case CameraType::TEST:
        // Test pattern for development
        desc = "videotestsrc is-live=true pattern=smpte"
               " ! video/x-raw,width=" + std::to_string(config_.width) +
               ",height=" + std::to_string(config_.height) +
               ",framerate=" + std::to_string(config_.fps) + "/1"
               " ! videoconvert"
               " ! clockoverlay font-desc=\"Sans 36\" time-format=\"%H:%M:%S\""
               " ! x264enc tune=zerolatency bitrate=" + std::to_string(config_.bitrate) +
               " speed-preset=ultrafast key-int-max=" + std::to_string(config_.fps * 2) +
               " bframes=0 b-adapt=false"
               " ! video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline"
               " ! h264parse config-interval=-1"
               " ! appsink name=sink emit-signals=true sync=false"
               " max-buffers=2 drop=true";
        break;
    }

    return desc;
}

bool CameraPipeline::launch_pipeline() {
    std::string desc = build_pipeline_description();
    spdlog::info("[{}] Launching pipeline: {}", config_.id, desc);

    GError* error = nullptr;
    pipeline_ = gst_parse_launch(desc.c_str(), &error);
    if (!pipeline_ || error) {
        spdlog::error("[{}] Failed to create pipeline: {}",
                      config_.id, error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return false;
    }

    // Get appsink element
    appsink_ = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
    if (!appsink_) {
        spdlog::error("[{}] Failed to get appsink element", config_.id);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    // Configure appsink callbacks
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = &CameraPipeline::on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink_), &callbacks, this, nullptr);

    // Start pipeline
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        spdlog::error("[{}] Failed to set pipeline to PLAYING", config_.id);
        gst_object_unref(appsink_);
        gst_object_unref(pipeline_);
        appsink_  = nullptr;
        pipeline_ = nullptr;
        return false;
    }

    spdlog::info("[{}] Pipeline launched successfully", config_.id);
    return true;
}

void CameraPipeline::destroy_pipeline() {
    if (pipeline_) {
        // Use async state change with timeout to avoid blocking on RTSP
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (ret == GST_STATE_CHANGE_ASYNC) {
            // Wait up to 3 seconds for state change
            GstState state;
            ret = gst_element_get_state(pipeline_, &state, nullptr, 3 * GST_SECOND);
            if (ret == GST_STATE_CHANGE_FAILURE || ret == GST_STATE_CHANGE_ASYNC) {
                spdlog::warn("[{}] Pipeline state change to NULL timed out, forcing", config_.id);
            }
        }
        if (appsink_) {
            gst_object_unref(appsink_);
            appsink_ = nullptr;
        }
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

bool CameraPipeline::start() {
    if (running_.load()) {
        spdlog::warn("[{}] Pipeline already running", config_.id);
        return true;
    }

    shutdown_.store(false);
    backoff_seconds_ = 1;

    if (!launch_pipeline()) {
        return false;
    }

    running_.store(true);

    // Start bus monitoring thread
    bus_thread_ = std::thread(&CameraPipeline::bus_monitor_thread, this);

    spdlog::info("[{}] Pipeline started successfully", config_.id);
    return true;
}

void CameraPipeline::stop() {
    if (!running_.load() && !bus_thread_.joinable()) return;

    spdlog::info("[{}] Stopping pipeline...", config_.id);
    shutdown_.store(true);
    running_.store(false);

    destroy_pipeline();

    // Wait for bus monitor thread to exit
    if (bus_thread_.joinable()) {
        bus_thread_.join();
    }

    spdlog::info("[{}] Pipeline stopped (total frames: {}, restarts: {})",
                 config_.id, frame_count_.load(), restart_count_.load());
}

void CameraPipeline::bus_monitor_thread() {
    spdlog::debug("[{}] Bus monitor thread started", config_.id);

    while (!shutdown_.load()) {
        if (!pipeline_) {
            // Pipeline was destroyed, wait for restart or shutdown
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        GstBus* bus = gst_element_get_bus(pipeline_);
        if (!bus) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Poll bus with 500ms timeout so we can check shutdown_ regularly
        GstMessage* msg = gst_bus_timed_pop(bus, 500 * GST_MSECOND);
        gst_object_unref(bus);

        if (!msg) continue;

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            spdlog::error("[{}] Pipeline ERROR: {} (debug: {})",
                          config_.id,
                          err ? err->message : "unknown",
                          debug ? debug : "none");
            if (err) g_error_free(err);
            if (debug) g_free(debug);

            // Schedule restart
            gst_message_unref(msg);
            schedule_restart();
            continue;  // skip the unref at bottom
        }

        case GST_MESSAGE_WARNING: {
            GError* err = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(msg, &err, &debug);
            spdlog::warn("[{}] Pipeline WARNING: {} (debug: {})",
                         config_.id,
                         err ? err->message : "unknown",
                         debug ? debug : "none");
            if (err) g_error_free(err);
            if (debug) g_free(debug);
            break;
        }

        case GST_MESSAGE_EOS:
            spdlog::warn("[{}] Pipeline received EOS", config_.id);
            gst_message_unref(msg);
            schedule_restart();
            continue;

        case GST_MESSAGE_STATE_CHANGED:
            // Only log state changes for the pipeline itself
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline_)) {
                GstState old_state, new_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, nullptr);
                spdlog::debug("[{}] Pipeline state: {} → {}",
                              config_.id,
                              gst_element_state_get_name(old_state),
                              gst_element_state_get_name(new_state));
            }
            break;

        default:
            break;
        }

        gst_message_unref(msg);
    }

    spdlog::debug("[{}] Bus monitor thread exiting", config_.id);
}

void CameraPipeline::schedule_restart() {
    if (shutdown_.load()) return;

    running_.store(false);
    destroy_pipeline();

    int attempt = restart_count_.fetch_add(1) + 1;
    spdlog::warn("[{}] Scheduling restart (attempt {}, backoff {}s)",
                 config_.id, attempt, backoff_seconds_);

    // Wait with backoff (check shutdown_ periodically)
    for (int i = 0; i < backoff_seconds_ * 10 && !shutdown_.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (shutdown_.load()) return;

    // Try to restart
    if (launch_pipeline()) {
        running_.store(true);
        backoff_seconds_ = 1;  // Reset backoff on success
        spdlog::info("[{}] Pipeline restarted successfully (attempt {})", config_.id, attempt);
    } else {
        // Exponential backoff: 1 → 2 → 4 → 8 → 16 → 30 (cap)
        backoff_seconds_ = std::min(backoff_seconds_ * 2, kMaxBackoff);
        spdlog::error("[{}] Restart failed, next attempt in {}s", config_.id, backoff_seconds_);

        // Try again (recursive via loop in bus_monitor_thread)
        schedule_restart();
    }
}

CallbackId CameraPipeline::on_frame(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    CallbackId id = next_cb_id_.fetch_add(1);
    callbacks_.push_back({id, std::move(callback)});
    spdlog::debug("[{}] Registered frame callback id={} (total: {})",
                  config_.id, id, callbacks_.size());
    return id;
}

void CameraPipeline::remove_callback(CallbackId id) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    auto it = std::remove_if(callbacks_.begin(), callbacks_.end(),
                             [id](const CallbackEntry& e) { return e.id == id; });
    if (it != callbacks_.end()) {
        callbacks_.erase(it, callbacks_.end());
        spdlog::debug("[{}] Removed frame callback id={} (remaining: {})",
                      config_.id, id, callbacks_.size());
    }
}

void CameraPipeline::clear_callbacks() {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    size_t count = callbacks_.size();
    callbacks_.clear();
    spdlog::debug("[{}] Cleared {} frame callbacks", config_.id, count);
}

double CameraPipeline::seconds_since_last_frame() const {
    auto now = std::chrono::steady_clock::now();
    auto last = last_frame_time_.load();
    return std::chrono::duration<double>(now - last).count();
}

GstFlowReturn CameraPipeline::on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* self = static_cast<CameraPipeline*>(user_data);

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Map buffer to read data
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Check for keyframe
    bool is_keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    // Create H264Frame
    H264Frame frame;
    frame.data.resize(map.size);
    std::memcpy(frame.data.data(), map.data, map.size);
    frame.timestamp   = GST_BUFFER_PTS(buffer);
    frame.is_keyframe = is_keyframe;

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    // Update health metrics
    self->frame_count_.fetch_add(1);
    self->last_frame_time_.store(std::chrono::steady_clock::now());

    // Distribute to all registered callbacks
    {
        std::lock_guard<std::mutex> lock(self->cb_mutex_);
        for (const auto& entry : self->callbacks_) {
            try {
                entry.fn(frame);
            } catch (const std::exception& e) {
                spdlog::warn("[{}] Frame callback {} threw: {}",
                             self->config_.id, entry.id, e.what());
            }
        }
    }

    return GST_FLOW_OK;
}

} // namespace ist
