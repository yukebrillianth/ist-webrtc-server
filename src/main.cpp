#include "config.h"
#include "camera_pipeline.h"
#include "signaling_server.h"
#include "peer_manager.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <gst/gst.h>

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <string>
#include <getopt.h>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
    spdlog::info("Received signal {}, shutting down...", sig);
    g_running.store(false);
}

static void print_usage(const char* program) {
    std::cerr << "IST WebRTC Camera Server v1.0.0\n"
              << "Remotely Operated Forklift - Camera Streaming\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  -c, --config <path>    Config file path (default: config.yaml)\n"
              << "  -v, --verbose          Enable verbose logging\n"
              << "  -h, --help             Show this help\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    // Default values
    std::string config_path = "config.yaml";
    bool verbose = false;

    // Parse command line arguments
    static struct option long_options[] = {
        {"config",  required_argument, nullptr, 'c'},
        {"verbose", no_argument,       nullptr, 'v'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr,   0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:vh", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'v': verbose = true;       break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    // Setup logging
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    spdlog::info("==========================================");
    spdlog::info("  IST WebRTC Camera Server v1.0.0");
    spdlog::info("  Remotely Operated Forklift");
    spdlog::info("==========================================");

    // Signal handler
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize GStreamer
    gst_init(&argc, &argv);
    spdlog::info("GStreamer initialized: {}", gst_version_string());

    try {
        // Load configuration
        auto config = ist::load_config(config_path);

        // Create camera pipelines
        std::vector<std::unique_ptr<ist::CameraPipeline>> cameras;
        for (const auto& cam_cfg : config.cameras) {
            cameras.push_back(std::make_unique<ist::CameraPipeline>(cam_cfg));
        }

        // Create peer manager
        ist::PeerManager peer_manager(config, cameras);

        // Create signaling server
        ist::SignalingServer signaling(config);

        signaling.on_client_connect([&peer_manager](const std::string& client_id,
                                                      std::shared_ptr<rtc::WebSocket> ws) {
            peer_manager.create_peer(client_id, ws);
        });

        signaling.on_client_disconnect([&peer_manager](const std::string& client_id) {
            peer_manager.remove_peer(client_id);
        });

        // Start signaling server
        if (!signaling.start()) {
            spdlog::error("Failed to start signaling server");
            gst_deinit();
            return 1;
        }

        // Start camera pipelines
        int started = 0;
        for (auto& camera : cameras) {
            if (camera->start()) {
                started++;
            } else {
                spdlog::error("Failed to start camera: {}", camera->id());
            }
        }

        if (started == 0) {
            spdlog::error("No cameras started successfully");
            gst_deinit();
            return 1;
        }

        spdlog::info("------------------------------------------");
        spdlog::info("  Server is running!");
        spdlog::info("  Signaling:  ws://{}:{}", config.server.bind, config.server.port);
        spdlog::info("  Cameras:    {}/{} active", started, cameras.size());
        spdlog::info("  Max clients: {}", config.webrtc.max_clients);
        spdlog::info("------------------------------------------");

        // Main loop - health monitoring
        auto last_log = std::chrono::steady_clock::now();
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Periodic status log every 30 seconds
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 30) {
                last_log = now;

                int active_cameras = 0;
                for (const auto& cam : cameras) {
                    if (cam->is_running()) active_cameras++;
                }

                spdlog::info("[Status] Cameras: {}/{} | Clients: {}",
                             active_cameras, cameras.size(),
                             peer_manager.peer_count());
            }
        }

        // Graceful shutdown
        spdlog::info("Shutting down...");

        // Stop cameras first
        for (auto& camera : cameras) {
            camera->stop();
        }

        // Stop signaling (disconnects clients)
        signaling.stop();

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        gst_deinit();
        return 1;
    }

    gst_deinit();
    spdlog::info("Server stopped cleanly. Goodbye!");
    return 0;
}
