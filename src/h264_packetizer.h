#pragma once

#include <rtc/rtc.hpp>
#include <cstdint>
#include <vector>

namespace ist {

// Converts GStreamer H264 byte-stream NAL units into proper RTP packets
// and sends them via libdatachannel Track
class H264Packetizer {
public:
    H264Packetizer() = default;

    // Send H264 data to a track
    // data: byte-stream format (00 00 00 01 NAL ...)
    // timestamp_ns: PTS in nanoseconds from GStreamer
    static void send_h264_to_track(
        std::shared_ptr<rtc::Track> track,
        const std::byte* data,
        size_t size,
        uint64_t timestamp_ns,
        uint64_t elapsed_ns    // elapsed since stream start
    );

private:
    // Split byte-stream into individual NAL units
    static std::vector<std::pair<const std::byte*, size_t>> split_nal_units(
        const std::byte* data, size_t size
    );
};

} // namespace ist
