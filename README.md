# IST WebRTC Camera Server

**Remotely Operated Forklift — Real-time Camera Streaming**

Program C++ untuk streaming 4 kamera (RTSP/USB) dari forklift ke control room melalui WebRTC. Didesain untuk latency rendah dan sinkronisasi antar kamera pada jaringan lokal (Moxa industrial WiFi).

## Architecture

```
┌─────────────── Forklift ───────────────────────┐
│                                                 │
│  RTSP Cam ──► GStreamer Pipeline ──┐            │
│  RTSP Cam ──► GStreamer Pipeline ──┤            │
│  USB  Cam ──► GStreamer Pipeline ──┼─► H264 ──► WebRTC Server ──► Control Room
│  USB  Cam ──► GStreamer Pipeline ──┘   Pktzr    (WS Signaling)    (Dashboard)
│                                                 │
└─────────────────────────────────────────────────┘
```

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
cameras:
  - id: "cam_front"
    name: "Front Camera"
    type: "rtsp" # rtsp | usb | test
    uri: "rtsp://192.168.1.100:554/stream"
    width: 1280
    height: 720
    fps: 30
    bitrate: 2000
```

Tipe kamera:

- **rtsp** — RTSP IP camera (sudah H264 encoded, hanya di-depay)
- **usb** — USB/V4L2 camera (perlu encoding, menggunakan x264)
- **test** — GStreamer test pattern (untuk development tanpa kamera fisik)

## Run

```bash
# Dengan config default
./build/webrtc-server

# Dengan config custom
./build/webrtc-server --config /path/to/config.yaml

# Verbose logging
./build/webrtc-server -v
```

## Test Dashboard

Buka browser di control room:

```
http://<forklift-ip>:8554
```

Klik **Connect** untuk mulai streaming. Dashboard menampilkan 4 feed kamera secara real-time.

> **Note:** File `web/index.html` perlu di-serve secara terpisah (nginx/http server). Server hanya menyediakan WebSocket signaling di port 8554.

## Docker

```bash
docker build -t ist-webrtc-server .
docker run -d --name ist-camera \
    --network host \
    -v $(pwd)/config.yaml:/opt/webrtc-server/config.yaml \
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

# Cek status
sudo systemctl status webrtc-server
journalctl -u webrtc-server -f
```

## Project Structure

```
webrtc-server/
├── CMakeLists.txt           # Build system
├── config.yaml              # Camera & server configuration
├── Dockerfile               # Container build
├── src/
│   ├── main.cpp             # Entry point, lifecycle management
│   ├── config.h/cpp         # YAML configuration loader
│   ├── camera_pipeline.h/cpp # GStreamer camera capture (RTSP/USB/test)
│   ├── h264_packetizer.h/cpp # H264 NAL → WebRTC RTP bridge
│   ├── signaling_server.h/cpp# WebSocket signaling server
│   └── peer_manager.h/cpp   # WebRTC PeerConnection management
├── web/
│   └── index.html           # Test dashboard (4-camera grid)
└── deploy/
    └── webrtc-server.service # Systemd service file
```

## Signaling Protocol

| Direction | Type          | Payload                    |
| --------- | ------------- | -------------------------- |
| S→C       | `camera_list` | Camera info array          |
| S→C       | `offer`       | SDP offer (4 video tracks) |
| C→S       | `answer`      | SDP answer                 |
| S↔C       | `candidate`   | ICE candidate              |
| S→C       | `error`       | Error message              |

## License

IST Internal - All rights reserved.
