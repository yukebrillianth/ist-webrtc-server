/**
 * @file    camera_pipeline.h
 * @brief   GStreamer camera capture pipeline with auto-recovery
 * @author  Yuke Brilliant Hestiavin <yukebrilliant@gmail.com>
 * @date    2026
 *
 * @copyright Copyright (c) 2026 PT Indonesia Smelting Technology (IST)
 *            All rights reserved. Internal use only.
 *
 * Manages a GStreamer pipeline for capturing H.264 video from RTSP, USB,
 * or test sources. Features automatic pipeline recovery with exponential
 * backoff, GStreamer bus monitoring, and frame health metrics for
 * industrial 24/7 operation.
 */

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

namespace ist
{

    /**
     * @brief H.264 encoded frame data passed from GStreamer to WebRTC layer
     */
    struct H264Frame
    {
        std::vector<std::byte> data; ///< NAL unit data (byte-stream format)
        uint64_t timestamp;          ///< Presentation timestamp in nanoseconds
        bool is_keyframe;            ///< True if this is an IDR frame
    };

    /// Callback signature for receiving encoded H.264 frames
    using FrameCallback = std::function<void(const H264Frame &)>;

    /// Unique identifier for a registered frame callback
    using CallbackId = uint64_t;

    /**
     * @brief GStreamer camera capture pipeline with automatic recovery
     *
     * Encapsulates the lifecycle of a GStreamer pipeline for a single camera
     * source. Monitors the GStreamer bus for errors and automatically restarts
     * the pipeline with exponential backoff on failure.
     *
     * Thread Safety:
     *   - on_frame(), remove_callback(), clear_callbacks() are thread-safe
     *   - start() and stop() must be called from the same thread
     *
     * @note For RTSP sources, the pipeline uses TCP transport with a 5-second
     *       timeout for faster disconnect detection.
     */
    class CameraPipeline
    {
    public:
        explicit CameraPipeline(const CameraConfig &config);
        ~CameraPipeline();

        // Non-copyable, non-movable
        CameraPipeline(const CameraPipeline &) = delete;
        CameraPipeline &operator=(const CameraPipeline &) = delete;

        /**
         * @brief  Start the GStreamer pipeline and bus monitor thread
         * @return true on success, false if pipeline creation failed
         */
        bool start();

        /**
         * @brief Stop the pipeline and release all GStreamer resources
         *
         * Sets the shutdown flag to prevent auto-recovery, then waits for
         * the bus monitor thread to exit. Uses a 3-second timeout for the
         * GStreamer state transition to avoid hangs on RTSP disconnects.
         */
        void stop();

        /**
         * @brief  Register a callback to receive H.264 frames
         * @param  callback  Function to invoke on each new frame
         * @return Unique ID for this callback (used with remove_callback)
         */
        CallbackId on_frame(FrameCallback callback);

        /**
         * @brief  Unregister a previously registered callback
         * @param  id  Callback ID returned by on_frame()
         */
        void remove_callback(CallbackId id);

        /**
         * @brief Remove all registered frame callbacks
         */
        void clear_callbacks();

        // ── Status & Health ─────────────────────────────────────────────

        bool is_running() const { return running_.load(); }
        const CameraConfig &config() const { return config_; }
        const std::string &id() const { return config_.id; }

        /** @brief Total frames captured since pipeline creation */
        uint64_t frame_count() const { return frame_count_.load(); }

        /** @brief Seconds elapsed since the last frame was received */
        double seconds_since_last_frame() const;

        /** @brief Number of times the pipeline has been auto-restarted */
        int restart_count() const { return restart_count_.load(); }

    private:
        /// GStreamer appsink callback — invoked on each new encoded sample
        static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data);

        /// Construct the GStreamer pipeline description string
        std::string build_pipeline_description() const;

        /// Create and start the GStreamer pipeline (internal)
        bool launch_pipeline();

        /// Tear down the GStreamer pipeline with timeout (internal)
        void destroy_pipeline();

        /// Bus monitoring thread entry point — handles ERROR/WARNING/EOS
        void bus_monitor_thread();

        /// Schedule a pipeline restart with exponential backoff
        void schedule_restart();

        // ── Members ─────────────────────────────────────────────────────

        CameraConfig config_;
        GstElement *pipeline_ = nullptr;
        GstElement *appsink_ = nullptr;
        std::atomic<bool> running_{false};
        std::atomic<bool> shutdown_{false}; ///< Permanent stop — inhibits recovery

        // Frame callback registry
        mutable std::mutex cb_mutex_;
        struct CallbackEntry
        {
            CallbackId id;
            FrameCallback fn;
        };
        std::vector<CallbackEntry> callbacks_;
        std::atomic<CallbackId> next_cb_id_{1};

        // Health metrics
        std::atomic<uint64_t> frame_count_{0};
        std::atomic<std::chrono::steady_clock::time_point> last_frame_time_{
            std::chrono::steady_clock::now()};

        // Auto-recovery state
        std::atomic<int> restart_count_{0};
        std::thread bus_thread_;
        int backoff_seconds_ = 1;
        static constexpr int kMaxBackoffSeconds = 30; ///< Backoff ceiling
    };

} // namespace ist
