#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>

namespace gocue::livemix::collab
{
inline constexpr int defaultWireRate = 48000, framesPerPacket = 120, channels = 2, bytesPerSample = 3;
inline constexpr bool isValidWireRate (uint32_t rate) { return rate == 44100 || rate == 48000; }
inline constexpr double packetDurationMs (uint32_t rate) { return framesPerPacket * 1000.0 / rate; }
inline constexpr int headerBytes = 36, maxDatagram = 1200, audioBytes = framesPerPacket * channels * bytesPerSample;
inline constexpr uint32_t magic = 0x42434d4c;
inline constexpr uint8_t version = 1;
enum class Kind : uint8_t { audio = 1, hello, welcome, ping, pong, bye, probe };

// Wire offsets (little endian): magic u32 @0, version u8 @4, kind u8 @5,
// flags u16 @6, session u64 @8, sender u32 @16, seq u32 @20,
// firstFrame u64 @24, frames u16 @32, payloadBytes u16 @34. Never memcpy a C++ struct.
struct Header
{
    Kind kind = Kind::audio;
    uint16_t flags = 0;
    uint64_t session = 0;
    uint32_t sender = 0, seq = 0;
    uint64_t firstFrame = 0;
    uint16_t frames = 0, payloadBytes = 0;
};

// hello/welcome: u8 nicknameBytes @0, UTF-8 nickname @1 (0..64 bytes),
// u32 inputLatencyUs, u32 outputLatencyUs, u8 versionBytes, UTF-8 appVersion
// (0..32 bytes), u32 deviceRateHz, u32 wireRateHz (44100 or 48000).
// deviceRateHz accounts for the sender ASRC. The host's welcome sets the room wire rate;
// the guest's hello reports its initial preference, then the accepted room rate.
// ping/pong: u64 monotonic nanoseconds @0 (pong echoes ping unchanged).
// bye: empty. probe: 16 opaque nonce bytes; all header fields except kind/length zero.
struct PeerInfo
{
    juce::String nickname, appVersion = "0.15.0";
    double inputLatencyMs = 0.0, outputLatencyMs = 0.0;
    uint32_t deviceRate = defaultWireRate, wireRate = defaultWireRate;
};
using Nonce = std::array<uint8_t, 16>;

size_t writePacket (uint8_t* destination, size_t capacity, const Header&, const void* payload) noexcept;
bool readPacket (const void* data, size_t bytes, Header&, const uint8_t*& payload) noexcept;
void packPcm24 (uint8_t* destination, const float* left, const float* right, int frames) noexcept;
void unpackPcm24 (const uint8_t* source, float* interleaved, int frames) noexcept;
size_t writePeerInfo (uint8_t* destination, const PeerInfo&);
bool readPeerInfo (const uint8_t* source, size_t bytes, PeerInfo&);
void writeTimestamp (uint8_t* destination, uint64_t ns) noexcept;
uint64_t readTimestamp (const uint8_t* source) noexcept;
}
