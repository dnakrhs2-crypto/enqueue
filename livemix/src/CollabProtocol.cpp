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
        case Kind::hello: case Kind::welcome: return h.frames == 0 && h.payloadBytes >= 18 && h.payloadBytes <= 114;
        case Kind::ping: case Kind::pong: return h.frames == 0 && h.payloadBytes == 8;
        case Kind::bye: case Kind::punch: return h.frames == 0 && h.payloadBytes == 0;
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
    if (! isValidWireRate (info.wireRate)) return 0;
    const size_t nick = utf8Prefix (info.nickname, 64), ver = utf8Prefix (info.appVersion, 32);
    size_t at = 0;
    p[at++] = (uint8_t) nick; std::memcpy (p + at, info.nickname.toRawUTF8(), nick); at += nick;
    for (double ms : { info.inputLatencyMs, info.outputLatencyMs })
    {
        put (p + at, (uint32_t) std::llround (std::clamp (std::isfinite (ms) ? ms : 0.0, 0.0, 4294967.0) * 1000.0), 4); at += 4;
    }
    p[at++] = (uint8_t) ver; std::memcpy (p + at, info.appVersion.toRawUTF8(), ver); at += ver;
    put (p + at, info.deviceRate, 4); at += 4;
    put (p + at, info.wireRate, 4);
    return at + 4;
}

bool readPeerInfo (const uint8_t* p, size_t bytes, PeerInfo& info)
{
    if (p == nullptr || bytes < 18 || p[0] > 64) return false;
    const size_t nick = p[0], verAt = 1 + nick + 8;
    if (verAt >= bytes || p[verAt] > 32 || bytes != verAt + 1 + p[verAt] + 8) return false;
    if (! juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (p + 1), (int) nick)
        || ! juce::CharPointer_UTF8::isValidString (reinterpret_cast<const char*> (p + verAt + 1), p[verAt])) return false;
    info.nickname = juce::String::fromUTF8 (reinterpret_cast<const char*> (p + 1), (int) nick);
    info.inputLatencyMs = get (p + 1 + nick, 4) / 1000.0;
    info.outputLatencyMs = get (p + 5 + nick, 4) / 1000.0;
    info.appVersion = juce::String::fromUTF8 (reinterpret_cast<const char*> (p + verAt + 1), p[verAt]);
    info.deviceRate = (uint32_t) get (p + bytes - 8, 4);
    info.wireRate = (uint32_t) get (p + bytes - 4, 4);
    return info.deviceRate > 0 && isValidWireRate (info.wireRate);
}
void writeTimestamp (uint8_t* p, uint64_t ns) noexcept { put (p, ns, 8); }
uint64_t readTimestamp (const uint8_t* p) noexcept { return get (p, 8); }

bool validHeaderV2 (const contracts::HeaderV2& h) noexcept
{
    using K = contracts::Kind;
    using F = contracts::Flags;
    if (h.session == 0 || h.origin == 0 || h.linkSender == 0
        || h.payloadBytes > contracts::maxPacketBytes - contracts::v2HeaderBytes) return false;
    if (h.kind == K::audio)
        return h.frames == framesPerPacket && h.firstFrame % framesPerPacket == 0 && h.streamEpoch != 0 && h.rosterRevision != 0
            && ((h.flags == F::stereoPcm24 && h.payloadBytes == contracts::stereoPcm24Bytes)
                || (h.flags == F::monoPcm16 && h.payloadBytes == contracts::monoPcm16Bytes));
    if (h.flags != F::none || h.frames != 0 || h.firstFrame != 0 || h.origin != h.linkSender) return false;
    if (h.kind == K::streamRestart) return h.streamEpoch > 1 && h.payloadBytes == 12 && h.rosterRevision != 0;
    if (h.streamEpoch != 0) return false;
    switch (h.kind)
    {
        case K::ping: case K::pong: return h.payloadBytes == 8;
        // A peer may cancel or ping before its first roster is installed (revision 0).
        case K::bye: case K::punch: return h.payloadBytes == 0;
        case K::rosterAck: return h.payloadBytes == 0 && h.rosterRevision != 0;
        case K::offer: case K::offerAck: return h.payloadBytes == 16 && h.rosterRevision != 0;
        case K::roster: return h.payloadBytes >= 39 && h.payloadBytes <= 674 && h.rosterRevision != 0;
        default: return false;
    }
}

size_t writePacketV2 (uint8_t* p, size_t capacity, const contracts::HeaderV2& h, const void* payload) noexcept
{
    const size_t bytes = contracts::v2HeaderBytes + h.payloadBytes;
    if (p == nullptr || capacity < bytes || ! validHeaderV2 (h) || (h.payloadBytes != 0 && payload == nullptr)) return 0;
    put (p, magic, 4); p[4] = 2; p[5] = (uint8_t) h.kind;
    put (p + 6, (uint16_t) h.flags, 2); put (p + 8, h.session, 8); put (p + 16, h.origin, 4);
    put (p + 20, h.seq, 4); put (p + 24, h.firstFrame, 8); put (p + 32, h.frames, 2); put (p + 34, h.payloadBytes, 2);
    put (p + 36, h.linkSender, 4); put (p + 40, h.streamEpoch, 4); put (p + 44, h.rosterRevision, 4);
    if (h.payloadBytes != 0) std::memcpy (p + contracts::v2HeaderBytes, payload, h.payloadBytes);
    return bytes;
}

bool readPacketV2 (const void* data, size_t bytes, contracts::HeaderV2& result, const uint8_t*& payload) noexcept
{
    payload = nullptr;
    if (data == nullptr || bytes < contracts::v2HeaderBytes || bytes > contracts::maxPacketBytes) return false;
    const auto* p = static_cast<const uint8_t*> (data);
    if (get (p, 4) != magic || p[4] != 2) return false;
    contracts::HeaderV2 h;
    h.kind = (contracts::Kind) p[5]; h.flags = (contracts::Flags) get (p + 6, 2); h.session = get (p + 8, 8);
    h.origin = (uint32_t) get (p + 16, 4); h.seq = (uint32_t) get (p + 20, 4); h.firstFrame = get (p + 24, 8);
    h.frames = (uint16_t) get (p + 32, 2); h.payloadBytes = (uint16_t) get (p + 34, 2);
    h.linkSender = (uint32_t) get (p + 36, 4); h.streamEpoch = (uint32_t) get (p + 40, 4);
    h.rosterRevision = (uint32_t) get (p + 44, 4);
    if (! validHeaderV2 (h) || bytes != contracts::v2HeaderBytes + h.payloadBytes) return false;
    result = h; payload = p + contracts::v2HeaderBytes;
    return true;
}

void packPcm16 (uint8_t* p, const float* mono, int frames) noexcept
{
    for (int i = 0; i < frames; ++i, p += 2)
    {
        const double sample = mono != nullptr ? mono[i] : 0.0;
        const auto value = (int32_t) std::llround (std::clamp (std::isfinite (sample) ? sample : 0.0, -1.0, 32767.0 / 32768.0) * 32768.0);
        put (p, (uint32_t) value, 2);
    }
}
void unpackPcm16 (const uint8_t* p, float* mono, int frames) noexcept
{
    for (int i = 0; i < frames; ++i, p += 2)
    {
        const auto u = (uint32_t) get (p, 2);
        mono[i] = (float) ((int32_t) (u & 0x7fff) - (int32_t) (u & 0x8000)) / 32768.0f;
    }
}
size_t writeOfferV2 (uint8_t* p, size_t capacity, const OfferV2& offer) noexcept
{
    if (p == nullptr || capacity < 16 || ! isValidWireRate (offer.wireRate)
        || offer.peerParticipantId == 0 || offer.connectionGeneration == 0) return 0;
    put (p, offer.capabilities & contracts::allCapabilities, 4); put (p + 4, offer.wireRate, 4);
    put (p + 8, offer.peerParticipantId, 4); put (p + 12, offer.connectionGeneration, 4);
    return 16;
}
bool readOfferV2 (const uint8_t* p, size_t bytes, OfferV2& result) noexcept
{
    if (p == nullptr || bytes != 16) return false;
    OfferV2 offer { (uint32_t) get (p, 4) & contracts::allCapabilities, (uint32_t) get (p + 4, 4),
                    (uint32_t) get (p + 8, 4), (uint32_t) get (p + 12, 4) };
    if (! isValidWireRate (offer.wireRate) || offer.peerParticipantId == 0 || offer.connectionGeneration == 0) return false;
    result = offer; return true;
}
size_t writeStreamRestartV2 (uint8_t* p, size_t capacity, const StreamRestartV2& restart) noexcept
{
    if (p == nullptr || capacity < 12 || restart.deviceRate == 0) return 0;
    put (p, restart.deviceRate, 4); put (p + 4, restart.inputLatencyUs, 4); put (p + 8, restart.outputLatencyUs, 4);
    return 12;
}
bool readStreamRestartV2 (const uint8_t* p, size_t bytes, StreamRestartV2& result) noexcept
{
    if (p == nullptr || bytes != 12 || get (p, 4) == 0) return false;
    result = { (uint32_t) get (p, 4), (uint32_t) get (p + 4, 4), (uint32_t) get (p + 8, 4) };
    return true;
}
}
