#include "h264_packetizer.h"
#include <spdlog/spdlog.h>
#include <cstring>

namespace ist {

std::vector<std::pair<const std::byte*, size_t>> H264Packetizer::split_nal_units(
    const std::byte* data, size_t size) {

    std::vector<std::pair<const std::byte*, size_t>> nals;
    if (!data || size < 4) return nals;

    // Find NAL unit boundaries (00 00 00 01 or 00 00 01)
    size_t i = 0;
    size_t nal_start = 0;
    bool found_first = false;

    while (i < size) {
        // Check for 4-byte start code: 00 00 00 01
        if (i + 3 < size &&
            data[i]   == std::byte{0x00} &&
            data[i+1] == std::byte{0x00} &&
            data[i+2] == std::byte{0x00} &&
            data[i+3] == std::byte{0x01}) {
            if (found_first && i > nal_start) {
                nals.emplace_back(data + nal_start, i - nal_start);
            }
            nal_start = i + 4;
            found_first = true;
            i += 4;
            continue;
        }
        // Check for 3-byte start code: 00 00 01
        if (i + 2 < size &&
            data[i]   == std::byte{0x00} &&
            data[i+1] == std::byte{0x00} &&
            data[i+2] == std::byte{0x01}) {
            if (found_first && i > nal_start) {
                nals.emplace_back(data + nal_start, i - nal_start);
            }
            nal_start = i + 3;
            found_first = true;
            i += 3;
            continue;
        }
        i++;
    }

    // Last NAL unit
    if (found_first && nal_start < size) {
        nals.emplace_back(data + nal_start, size - nal_start);
    }

    return nals;
}

void H264Packetizer::send_h264_to_track(
    std::shared_ptr<rtc::Track> track,
    const std::byte* data,
    size_t size,
    uint64_t timestamp_ns,
    uint64_t elapsed_ns) {

    if (!track || !track->isOpen()) return;
    if (!data || size == 0) return;

    // libdatachannel's H264RtpPacketizer expects the entire access unit
    // in byte-stream format (with start codes). It will handle NAL splitting,
    // FU-A fragmentation, and RTP packetization internally.

    // Calculate RTP timestamp (90kHz clock)
    auto rtp_timestamp = static_cast<uint32_t>(elapsed_ns / 1000 * 90 / 1000);

    try {
        // Send the raw H264 byte-stream; the track's packetizer handles the rest
        track->send(reinterpret_cast<const std::byte*>(data), size);
    } catch (const std::exception& e) {
        spdlog::warn("Failed to send H264 data to track: {}", e.what());
    }
}

} // namespace ist
