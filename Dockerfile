# ==== Build Stage ====
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libssl-dev libnice-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY CMakeLists.txt .
COPY src/ src/

RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && cmake --build . -j$(nproc)

# ==== Runtime Stage ====
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
    libnice10 libssl3 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -r -s /bin/false ist

WORKDIR /opt/webrtc-server
COPY --from=builder /build/build/webrtc-server .
COPY config.yaml .
COPY web/ web/

RUN chown -R ist:ist /opt/webrtc-server

USER ist

EXPOSE 8554

ENTRYPOINT ["./webrtc-server"]
CMD ["--config", "config.yaml"]
