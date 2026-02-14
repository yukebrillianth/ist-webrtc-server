<p align="center">
  <img height="60" alt="IST Logo" src="https://github.com/user-attachments/assets/aa8510eb-fbbf-479d-bf4d-0bc31e30d549" />
  &nbsp;&nbsp;&nbsp;&nbsp;
  <img height="60" alt="MRI Logo" src="https://github.com/user-attachments/assets/ac6bdb22-16b2-4b04-933a-4e31308e6525" />
  &nbsp;&nbsp;&nbsp;&nbsp;
  <img height="60" alt="ITS Logo" src="https://github.com/user-attachments/assets/8cf68a07-12ba-44c3-84b9-517e6502d9cc" />
</p>

# IST WebRTC Camera Server

**Remotely Operated Forklift — Real-time Camera Streaming**

Program C++ untuk streaming 4 kamera (RTSP/USB) dari forklift ke control room melalui WebRTC. Didesain untuk latency rendah, sinkronisasi antar kamera, dan **operasi 24/7 di lingkungan industri** dengan auto-recovery dan health monitoring.

## Architecture

```
┌─────────────── Forklift ───────────────────────┐
│                                                 │
│  RTSP Cam ──► GStreamer Pipeline ──┐            │
│  RTSP Cam ──► GStreamer Pipeline ──┤            │
│  USB  Cam ──► GStreamer Pipeline ──┼─► H264 ──► WebRTC Server ──► Control Room
│  USB  Cam ──► GStreamer Pipeline ──┘   RTP      (WS Signaling)    (Dashboard)
│                                                 │
│  Bus Monitor ◄── Auto-Recovery (backoff 1s→30s) │
│  Watchdog ──► Stall Detection (10s threshold)   │
│  File Logger ──► Rotating Logs (10MB × 3)       │
│                                                 │
└─────────────────────────────────────────────────┘
```

## Features

- **Multi-camera streaming** — Up to 4 cameras (RTSP, USB, test pattern) over single PeerConnection
- **Low latency** — Zero-copy H264 passthrough for RTSP, `zerolatency` x264 for USB
- **Auto-recovery** — Pipeline auto-restart with exponential backoff (1s → 2s → 4s → ... → 30s cap)
- **GStreamer bus monitoring** — Handles ERROR, WARNING, and EOS events automatically
- **Health watchdog** — Detects stalled cameras (no frames > 10s), logs health every 30s
- **Rotating file logs** — 10MB × 3 files, auto-flush on warnings
- **Clean callback lifecycle** — No memory leaks on client reconnect/disconnect
- **Graceful shutdown** — Handles SIGINT/SIGTERM cleanly

## Dependencies

- **C++17** compiler (GCC 9+ / Clang 10+)
- **CMake** 3.16+
- **GStreamer** 1.20+ dengan plugins: good, bad, ugly, libav
- **OpenSSL** 1.1+
- **libnice** (ICE library)

### Install Dependencies (Ubuntu/Debian)

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake git pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libssl-dev libnice-dev
```

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## Configuration

Edit `config.yaml`:

```yaml
server:
  port: 8554
  bind: "0.0.0.0"

cameras:
  - id: "cam_front"
    name: "Front Camera"
    type: "rtsp" # rtsp | usb | test
    uri: "rtsp://192.168.1.100:554/stream"
    width: 1280
    height: 720
    fps: 30
    bitrate: 2000 # pengaturan bitrate ini hanya untuk kamera usb ajah

  - id: "cam_left"
    name: "Left Camera"
    type: "test" # test pattern for development
    uri: "/dev/video0"
    width: 1280
    height: 720
    fps: 30
    bitrate: 2000

webrtc:
  stun_server: "" # kosong = local only (recommended untuk jaringan lokal)
  max_clients: 3
```

Tipe kamera:

| Type   | Description            | Encoding                       |
| ------ | ---------------------- | ------------------------------ |
| `rtsp` | IP camera via RTSP     | Passthrough (H264 dari kamera) |
| `usb`  | USB/V4L2 camera        | x264 zerolatency encoding      |
| `test` | GStreamer test pattern | x264 + clock overlay           |

## Run

```bash
# Standard
./build/webrtc-server --config config.yaml

# Dengan verbose logging + custom log directory
./build/webrtc-server --config config.yaml --log-dir /var/log/webrtc-server -v

# Opsi lengkap
./build/webrtc-server --help
```

### CLI Options

| Option                 | Default       | Description          |
| ---------------------- | ------------- | -------------------- |
| `-c, --config <path>`  | `config.yaml` | Config file path     |
| `-l, --log-dir <path>` | `./logs`      | Log output directory |
| `-v, --verbose`        | off           | Enable debug logging |
| `-h, --help`           | -             | Show help            |

### Log Output

Logs disimpan di `<log-dir>/webrtc-server.log` dengan rotating (10MB × 3 backup files). Log juga ditampilkan di console.

Contoh health monitoring log:

```
[2026-02-15 05:15:00.123] [info] [Health] Cameras: 4/4 active, 0 stalled | Clients: 2 | Uptime: 3600s
[2026-02-15 05:15:00.124] [warn] [cam_front] STALLED — no frames for 12.3s (total: 54321, restarts: 1)
[2026-02-15 05:15:05.500] [warn] [cam_front] Scheduling restart (attempt 2, backoff 1s)
[2026-02-15 05:15:06.600] [info] [cam_front] Pipeline restarted successfully (attempt 2)
```

## Test Dashboard

Buka `web/index.html` di browser control room. File ini perlu di-serve via Nginx atau HTTP server terpisah.

Dashboard menampilkan:

- 4 feed kamera real-time
- Stats overlay: FPS, bitrate, jitter, packet loss, latency (RTT)
- Connection status indikator

> **Note:** Server hanya menyediakan WebSocket signaling di port 8554 — bukan HTTP server.

## Docker

```bash
docker build -t ist-webrtc-server .
docker run -d --name ist-camera \
    --network host \
    -v $(pwd)/config.yaml:/opt/webrtc-server/config.yaml \
    -v /var/log/webrtc-server:/opt/webrtc-server/logs \
    ist-webrtc-server
```

`--network host` diperlukan agar WebRTC bisa akses camera device dan jaringan lokal.

## Systemd Service

```bash
# Copy files
sudo cp build/webrtc-server /opt/webrtc-server/
sudo cp config.yaml /opt/webrtc-server/
sudo cp deploy/webrtc-server.service /etc/systemd/system/

# Enable dan start
sudo systemctl daemon-reload
sudo systemctl enable webrtc-server
sudo systemctl start webrtc-server

# Monitoring
sudo systemctl status webrtc-server
journalctl -u webrtc-server -f
tail -f /var/log/webrtc-server/webrtc-server.log
```

## Project Structure

```
webrtc-server/
├── CMakeLists.txt             # Build system (FetchContent dependencies)
├── config.yaml                # Camera & server configuration
├── Dockerfile                 # Multi-stage container build
├── src/
│   ├── main.cpp               # Entry point, watchdog, file logging
│   ├── config.h/cpp           # YAML configuration loader
│   ├── camera_pipeline.h/cpp  # GStreamer capture + auto-recovery + bus monitor
│   ├── signaling_server.h/cpp # WebSocket signaling server
│   └── peer_manager.h/cpp     # WebRTC PeerConnection + callback lifecycle
├── web/
│   └── index.html             # Test dashboard (4-camera grid + stats)
└── deploy/
    └── webrtc-server.service  # Systemd service file
```

## Signaling Protocol

WebSocket JSON protocol di port 8554:

| Direction | Type          | Payload                    |
| --------- | ------------- | -------------------------- |
| S→C       | `camera_list` | Camera info array          |
| S→C       | `offer`       | SDP offer (4 video tracks) |
| C→S       | `answer`      | SDP answer                 |
| S↔C       | `candidate`   | ICE candidate              |
| S→C       | `error`       | Error message              |

## Robustness / Auto-Recovery

Server dirancang untuk **24/7 industrial operation**:

| Scenario                   | Behavior                                 |
| -------------------------- | ---------------------------------------- |
| RTSP camera disconnect     | Auto-reconnect (backoff 1s → 30s)        |
| GStreamer pipeline error   | Detect via bus monitor → auto-restart    |
| Camera stall (no frames)   | Watchdog alert every 30s                 |
| Client disconnect          | Cleanup peer + unregister callbacks      |
| Multiple clients reconnect | No callback leak, proper lifecycle       |
| SIGTERM/SIGINT             | Graceful shutdown (stop cams → close WS) |

## License

IST Internal - All rights reserved.
