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
        // RTSP cameras typically output H264, just depay and forward
        desc = "rtspsrc location=" + config_.uri +
               " latency=0 protocols=tcp"
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

bool CameraPipeline::start() {
    if (running_.load()) {
        spdlog::warn("[{}] Pipeline already running", config_.id);
        return true;
    }

    std::string desc = build_pipeline_description();
    spdlog::info("[{}] Starting pipeline: {}", config_.id, desc);

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
        spdlog::error("[{}] Failed to start pipeline", config_.id);
        gst_object_unref(appsink_);
        gst_object_unref(pipeline_);
        appsink_  = nullptr;
        pipeline_ = nullptr;
        return false;
    }

    running_.store(true);
    spdlog::info("[{}] Pipeline started successfully", config_.id);
    return true;
}

void CameraPipeline::stop() {
    if (!running_.load()) return;

    spdlog::info("[{}] Stopping pipeline...", config_.id);
    running_.store(false);

    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (appsink_) {
            gst_object_unref(appsink_);
            appsink_ = nullptr;
        }
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    spdlog::info("[{}] Pipeline stopped", config_.id);
}

void CameraPipeline::on_frame(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    callbacks_.push_back(std::move(callback));
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

    // Distribute to all registered callbacks
    {
        std::lock_guard<std::mutex> lock(self->cb_mutex_);
        for (const auto& cb : self->callbacks_) {
            cb(frame);
        }
    }

    return GST_FLOW_OK;
}

} // namespace ist
