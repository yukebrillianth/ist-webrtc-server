/**
 * @file    config.h
 * @brief   Application configuration structures and loader declaration
 * @author  Yuke Brilliant Hestiavin <yukebrilliant@gmail.com>
 * @date    2026
 *
 * @copyright Copyright (c) 2026 PT Indonesia Smelting Technology (IST)
 *            All rights reserved. Internal use only.
 *
 * Defines the data structures used to configure the WebRTC camera server,
 * including server binding, camera parameters, and WebRTC settings.
 * Configuration is loaded from a YAML file at startup.
 */

#pragma once

#include <string>
#include <vector>

namespace ist
{

    /**
     * @brief Supported camera source types
     */
    enum class CameraType
    {
        RTSP, ///< IP camera via RTSP protocol (H.264 passthrough)
        USB,  ///< USB/V4L2 camera (requires software encoding)
        TEST  ///< GStreamer test pattern (development/diagnostics)
    };

    /**
     * @brief Video encoder backend selection
     */
    enum class EncoderType
    {
        SOFTWARE, ///< x264enc (CPU-based, slower but compatible)
        VAAPI     ///< vaapih264enc (Intel Quick Sync via VA-API, requires hardware support)
    };

    /**
     * @brief Configuration for a single camera source
     */
    struct CameraConfig
    {
        std::string id;      ///< Unique camera identifier (e.g., "cam_front")
        std::string name;    ///< Human-readable display name
        CameraType type;     ///< Source type (RTSP, USB, or TEST)
        std::string uri;     ///< RTSP URI or V4L2 device path
        int width;           ///< Capture width in pixels
        int height;          ///< Capture height in pixels
        int fps;             ///< Target frame rate
        int bitrate;         ///< Target bitrate in kbps (USB/TEST encoding only)
        EncoderType encoder; ///< Encoder backend (SOFTWARE or VAAPI, USB/TEST only)
    };

    /**
     * @brief Server networking configuration
     */
    struct ServerConfig
    {
        int port;         ///< WebSocket signaling port
        std::string bind; ///< Bind address (e.g., "0.0.0.0")
    };

    /**
     * @brief WebRTC-specific configuration
     */
    struct WebRTCConfig
    {
        std::string stun_server; ///< STUN server URI (empty = local network only)
        int max_clients;         ///< Maximum concurrent WebRTC clients
        int mtu;
    };

    /**
     * @brief Top-level application configuration
     */
    struct AppConfig
    {
        std::string version;               ///< Application version string
        ServerConfig server;               ///< Server networking settings
        std::vector<CameraConfig> cameras; ///< Camera source definitions
        WebRTCConfig webrtc;               ///< WebRTC parameters
    };

    /**
     * @brief  Load application configuration from a YAML file
     * @param  path  Filesystem path to the YAML configuration file
     * @return Parsed AppConfig structure
     * @throws std::runtime_error if the file cannot be read or parsed
     */
    AppConfig load_config(const std::string &path);

} // namespace ist
