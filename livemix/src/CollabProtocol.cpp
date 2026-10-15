#include "CollabProtocol.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gocue::livemix::collab
{
namespace
{
void put (uint8_t* p, uint64_t value, int bytes) noexcept
{
    for (int i = 0; i < bytes; ++i) p[i] = (uint8_t) (value >> (i * 8));
}
uint64_t get (const uint8_t* p, int bytes) noexcept
{
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value |= (uint64_t) p[i] << (i * 8);
    return value;
}
bool valid (const Header& h) noexcept
{
    if (h.flags != 0 || h.payloadBytes > maxDatagram - headerBytes) return false;
    switch (h.kind)
    {
        case Kind::audio: return h.frames == framesPerPacket && h.payloadBytes == audioBytes && h.firstFrame % framesPerPacket == 0;
        case Kind::hello: case Kind::welcome: return h.frames == 0 && h.payloadBytes >= 14 && h.payloadBytes <= 110;
        case Kind::ping: case Kind::pong: return h.frames == 0 && h.payloadBytes == 8;
        case Kind::bye: return h.frames == 0 && h.payloadBytes == 0;
        case Kind::probe: return h.payloadBytes == 16 && h.session == 0 && h.sender == 0 && h.seq == 0 && h.firstFrame == 0 && h.frames == 0;
        default: return false;
    }
}
size_t utf8Prefix (const juce::String& text, size_t limit)
{
    const auto* p = reinterpret_cast<const uint8_t*> (text.toRawUTF8());
    auto n = std::min (text.getNumBytesAsUTF8(), limit);
    while (n > 0 && (p[n] & 0xc0) == 0x80) --n;
    return n;
}
}

size_t writePacket (uint8_t* p, size_t capacity, const Header& h, const void* payload) noexcept
{
    const size_t size = headerBytes + h.payloadBytes;
    if (p == nullptr || capacity < size || ! valid (h) || (h.payloadBytes > 0 && payload == nullptr)) return 0;
    put (p, magic, 4); p[4] = version; p[5] = (uint8_t) h.kind;
    put (p + 6, h.flags, 2); put (p + 8, h.session, 8); put (p + 16, h.sender, 4);
    put (p + 20, h.seq, 4); put (p + 24, h.firstFrame, 8); put (p + 32, h.frames, 2); put (p + 34, h.payloadBytes, 2);
    if (h.payloadBytes != 0) std::memcpy (p + headerBytes, payload, h.payloadBytes);
    return size;
}

bool readPacket (const void* data, size_t bytes, Header& result, const uint8_t*& payload) noexcept
{
    payload = nullptr;
    if (data == nullptr || bytes < headerBytes || bytes > maxDatagram) return false;
    const auto* p = static_cast<const uint8_t*> (data);
    if (get (p, 4) != magic || p[4] != version) return false;
    Header h;
    h.kind = (Kind) p[5]; h.flags = (uint16_t) get (p + 6, 2); h.session = get (p + 8, 8);
    h.sender = (uint32_t) get (p + 16, 4); h.seq = (uint32_t) get (p + 20, 4); h.firstFrame = get (p + 24, 8);
    h.frames = (uint16_t) get (p + 32, 2); h.payloadBytes = (uint16_t) get (p + 34, 2);
    if (! valid (h) || bytes != headerBytes + (size_t) h.payloadBytes) return false;
    result = h; payload = p + headerBytes;
    return true;
}

void packPcm24 (uint8_t* p, const float* left, const float* right, int frames) noexcept
{
    for (int i = 0; i < frames; ++i)
        for (const float* channel : { left, right })
        {
            const double sample = channel != nullptr ? channel[i] : 0.0;
            const auto value = (int32_t) std::llround (std::clamp (std::isfinite (sample) ? sample : 0.0, -1.0, 8388607.0 / 8388608.0) * 8388608.0);
            put (p, (uint32_t) value, 3); p += 3;
        }
}

void unpackPcm24 (const uint8_t* p, float* out, int frames) noexcept
{
    for (int i = 0; i < frames * channels; ++i, p += 3)
    {
        const auto u = (uint32_t) get (p, 3);
        const int32_t signedValue = (int32_t) (u & 0x7fffff) - (int32_t) (u & 0x800000);
        out[i] = (float) signedValue / 8388608.0f;
    }
}

size_t writePeerInfo (uint8_t* p, const PeerInfo& info)
{
    const size_t nick = utf8Prefix (info.nickname, 64), ver = utf8Prefix (info.appVersion, 32);
    size_t at = 0;
    p[at++] = (uint8_t) nick; std::memcpy (p + at, info.nickname.toRawUTF8(), nick); at += nick;
    for (double ms : { info.inputLatencyMs, info.outputLatencyMs })
    {
        put (p + at, (uint32_t) std::llround (std::clamp (std::isfinite (ms) ? ms : 0.0, 0.0, 4294967.0) * 1000.0), 4); at += 4;
    }
    p[at++] = (uint8_t) ver; std::memcpy (p + at, info.appVersion.toRawUTF8(), ver); at += ver;
    put (p + at, info.deviceRate, 4);
    return at + 4;
}

bool readPeerInfo (const uint8_t* p, size_t bytes, PeerInfo& info)
{
    if (p == nullptr || bytes < 14 || p[0] > 64) return false;
    const size_t nick = p[0], verAt = 1 + nick + 8;
    if (verAt >= bytes || p[verAt] > 32 || bytes != verAt + 1 + p[verAt] + 4) return false;
    if (! juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (p + 1), (int) nick)
        || ! juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (p + verAt + 1), p[verAt])) return false;
    info.nickname = juce::String::fromUTF8 (reinterpret_cast<const char*> (p + 1), (int) nick);
    info.inputLatencyMs = get (p + 1 + nick, 4) / 1000.0;
    info.outputLatencyMs = get (p + 5 + nick, 4) / 1000.0;
    info.appVersion = juce::String::fromUTF8 (reinterpret_cast<const char*> (p + verAt + 1), p[verAt]);
    info.deviceRate = (uint32_t) get (p + bytes - 4, 4);
    return info.deviceRate > 0;
}
void writeTimestamp (uint8_t* p, uint64_t ns) noexcept { put (p, ns, 8); }
uint64_t readTimestamp (const uint8_t* p) noexcept { return get (p, 8); }
}
