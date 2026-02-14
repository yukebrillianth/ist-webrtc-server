#include "config.h"
#include "camera_pipeline.h"
#include "signaling_server.h"
#include "peer_manager.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <gst/gst.h>

#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <iostream>
#include <string>
#include <getopt.h>

static std::atomic<bool> g_running{true};
static std::atomic<int>  g_signal_count{0};

static void signal_handler(int sig) {
    int count = g_signal_count.fetch_add(1) + 1;
    if (count == 1) {
        spdlog::info("Received signal {}, shutting down gracefully...", sig);
        g_running.store(false);
    } else {
        // Second signal = force exit
        spdlog::warn("Forced exit (signal {} received {} times)", sig, count);
        _exit(1);
    }
}

static void print_usage(const char* program) {
    std::cerr << "IST WebRTC Camera Server v1.0.0\n"
              << "Remotely Operated Forklift - Camera Streaming\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  -c, --config <path>     Config file path (default: config.yaml)\n"
              << "  -l, --log-dir <path>    Log directory (default: ./logs)\n"
              << "  -v, --verbose           Enable verbose logging\n"
              << "  -h, --help              Show this help\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    // Default values
    std::string config_path = "config.yaml";
    std::string log_dir = "./logs";
    bool verbose = false;

    // Parse command line arguments
    static struct option long_options[] = {
        {"config",  required_argument, nullptr, 'c'},
        {"log-dir", required_argument, nullptr, 'l'},
        {"verbose", no_argument,       nullptr, 'v'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr,   0,                 nullptr,  0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:vh", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'l': log_dir = optarg;     break;
        case 'v': verbose = true;       break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    // Setup logging — console + rotating file
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_dir + "/webrtc-server.log",
        10 * 1024 * 1024,  // 10 MB per file
        3                  // keep 3 rotated files
    );

    auto logger = std::make_shared<spdlog::logger>(
        "main", spdlog::sinks_init_list{console_sink, file_sink}
    );
    spdlog::set_default_logger(logger);
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    spdlog::flush_on(spdlog::level::warn);  // auto-flush on warnings and above

    spdlog::info("==========================================");
    spdlog::info("  IST WebRTC Camera Server v1.0.0");
    spdlog::info("  Remotely Operated Forklift");
    spdlog::info("==========================================");
    spdlog::info("Log directory: {}", log_dir);

    // Signal handler
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Initialize GStreamer
    gst_init(&argc, &argv);
    spdlog::info("GStreamer initialized: {}", gst_version_string());

    try {
        // Load configuration
        spdlog::info("Loading configuration from: {}", config_path);
        auto config = ist::load_config(config_path);
        spdlog::info("Configuration loaded: {} cameras, port {}, max {} clients",
                     config.cameras.size(), config.server.port, config.webrtc.max_clients);

        for (const auto& cam : config.cameras) {
            std::string type_str;
            switch (cam.type) {
            case ist::CameraType::RTSP: type_str = "RTSP"; break;
            case ist::CameraType::USB:  type_str = "USB";  break;
            case ist::CameraType::TEST: type_str = "TEST"; break;
            }
            spdlog::info("  Camera [{}] '{}' type={} uri={} {}x{}@{}fps",
                         cam.id, cam.name, type_str, cam.uri,
                         cam.width, cam.height, cam.fps);
        }

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

        // Main loop — health monitoring & watchdog
        auto last_status_log = std::chrono::steady_clock::now();
        constexpr double kStallThresholdSeconds = 10.0;

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            auto now = std::chrono::steady_clock::now();
            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_status_log).count();

            // Periodic status + watchdog every 30 seconds
            if (elapsed_s >= 30) {
                last_status_log = now;

                int active = 0, stalled = 0;
                for (const auto& cam : cameras) {
                    if (cam->is_running()) {
                        active++;
                        double since_last = cam->seconds_since_last_frame();
                        if (since_last > kStallThresholdSeconds) {
                            stalled++;
                            spdlog::warn("[{}] STALLED — no frames for {:.1f}s (total: {}, restarts: {})",
                                         cam->id(), since_last,
                                         cam->frame_count(), cam->restart_count());
                        }
                    } else {
                        // Camera not running — recovery should be handling this
                        spdlog::warn("[{}] Not running (restarts: {})",
                                     cam->id(), cam->restart_count());
                    }
                }

                spdlog::info("[Health] Cameras: {}/{} active, {} stalled | Clients: {} | Uptime: {}s",
                             active, cameras.size(), stalled,
                             peer_manager.peer_count(), elapsed_s);

                // Per-camera frame stats
                for (const auto& cam : cameras) {
                    spdlog::debug("[{}] frames={}, last_frame={:.1f}s ago, restarts={}",
                                  cam->id(), cam->frame_count(),
                                  cam->seconds_since_last_frame(),
                                  cam->restart_count());
                }
            }
        }

        // Graceful shutdown with timeout
        spdlog::info("Shutting down...");

        // Run shutdown in a thread with timeout
        std::atomic<bool> shutdown_done{false};
        std::thread shutdown_thread([&]() {
            // Stop cameras first
            for (auto& camera : cameras) {
                camera->stop();
            }
            // Stop signaling (disconnects clients)
            signaling.stop();
            shutdown_done.store(true);
        });

        // Wait up to 5 seconds for graceful shutdown
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!shutdown_done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (shutdown_done.load()) {
            shutdown_thread.join();
            spdlog::info("Graceful shutdown completed");
        } else {
            spdlog::warn("Shutdown timed out after 5s, forcing exit");
            shutdown_thread.detach();
        }

    } catch (const std::exception& e) {
        spdlog::error("Fatal error: {}", e.what());
        gst_deinit();
        return 1;
    }

    gst_deinit();
    spdlog::info("Server stopped cleanly. Goodbye!");
    return 0;
}
