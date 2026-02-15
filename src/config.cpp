/**
 * @file    config.cpp
 * @brief   YAML configuration loader implementation
 * @author  Yuke Brilliant Hestiavin <yukebrilliant@gmail.com>
 * @date    2026
 *
 * @copyright Copyright (c) 2026 PT Indonesia Smelting Technology (IST)
 *            All rights reserved. Internal use only.
 *
 * Parses the YAML configuration file and populates the AppConfig structure.
 * Validates required fields and provides sensible defaults.
 */

#include "config.h"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <algorithm>

namespace ist
{

    static CameraType parse_camera_type(const std::string &type_str)
    {
        std::string lower = type_str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "rtsp")
            return CameraType::RTSP;
        if (lower == "usb")
            return CameraType::USB;
        if (lower == "test")
            return CameraType::TEST;
        throw std::runtime_error("Unknown camera type: " + type_str);
    }

    static EncoderType parse_encoder_type(const std::string &encoder_str)
    {
        std::string lower = encoder_str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower == "software")
            return EncoderType::SOFTWARE;
        if (lower == "vaapi")
            return EncoderType::VAAPI;
        throw std::runtime_error("Unknown encoder type: " + encoder_str);
    }

    AppConfig load_config(const std::string &path)
    {
        spdlog::info("Loading configuration from: {}", path);

        YAML::Node root;
        try
        {
            root = YAML::LoadFile(path);
        }
        catch (const YAML::Exception &e)
        {
            throw std::runtime_error("Failed to load config file: " + std::string(e.what()));
        }

        AppConfig config;

        // Version config constant
        config.version = "v1.1.0";

        // Server config
        if (auto server = root["server"])
        {
            if (server["port"])
                config.server.port = server["port"].as<uint16_t>();
            if (server["bind"])
                config.server.bind = server["bind"].as<std::string>();
        }

        // Cameras
        if (auto cameras = root["cameras"])
        {
            for (const auto &cam : cameras)
            {
                CameraConfig cc;
                cc.id = cam["id"].as<std::string>();
                cc.name = cam["name"].as<std::string>();
                cc.type = parse_camera_type(cam["type"].as<std::string>());
                cc.uri = cam["uri"].as<std::string>();
                if (cam["width"])
                    cc.width = cam["width"].as<int>();
                if (cam["height"])
                    cc.height = cam["height"].as<int>();
                if (cam["fps"])
                    cc.fps = cam["fps"].as<int>();
                if (cam["bitrate"])
                    cc.bitrate = cam["bitrate"].as<int>();

                // Encoder type (default: SOFTWARE)
                cc.encoder = EncoderType::SOFTWARE;
                if (cam["encoder"])
                {
                    cc.encoder = parse_encoder_type(cam["encoder"].as<std::string>());
                }

                config.cameras.push_back(std::move(cc));
            }
        }

        if (config.cameras.empty())
        {
            throw std::runtime_error("No cameras configured");
        }

        // WebRTC config
        if (auto webrtc = root["webrtc"])
        {
            if (webrtc["stun_server"])
                config.webrtc.stun_server = webrtc["stun_server"].as<std::string>();
            if (webrtc["max_clients"])
                config.webrtc.max_clients = webrtc["max_clients"].as<int>();
            if (webrtc["mtu"])
                config.webrtc.mtu = webrtc["mtu"].as<int>();
        }

        spdlog::info("Configuration loaded: {} cameras, port {}, max {} clients",
                     config.cameras.size(), config.server.port, config.webrtc.max_clients);

        for (const auto &cam : config.cameras)
        {
            std::string type_str = (cam.type == CameraType::RTSP) ? "RTSP" : (cam.type == CameraType::USB) ? "USB"
                                                                                                           : "TEST";
            std::string encoder_str = (cam.encoder == EncoderType::SOFTWARE) ? "software" : "vaapi";
            spdlog::info("  Camera [{}] '{}' type={} encoder={} uri={} {}x{}@{}fps",
                         cam.id, cam.name, type_str, encoder_str, cam.uri,
                         cam.width, cam.height, cam.fps);
        }

        return config;
    }

} // namespace ist
