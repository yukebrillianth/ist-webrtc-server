#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace ist {

enum class CameraType {
    RTSP,
    USB,
    TEST  // GStreamer videotestsrc for development
};

struct CameraConfig {
    std::string id;
    std::string name;
    CameraType  type;
    std::string uri;
    int         width    = 1280;
    int         height   = 720;
    int         fps      = 30;
    int         bitrate  = 2000; // kbps
};

struct ServerConfig {
    uint16_t    port = 8554;
    std::string bind = "0.0.0.0";
};

struct WebRTCConfig {
    std::string stun_server;
    int         max_clients = 3;
};

struct AppConfig {
    ServerConfig              server;
    std::vector<CameraConfig> cameras;
    WebRTCConfig              webrtc;
};

// Load configuration from YAML file
AppConfig load_config(const std::string& path);

} // namespace ist
