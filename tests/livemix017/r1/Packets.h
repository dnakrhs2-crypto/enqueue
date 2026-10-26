#pragma once
#include "../../../livemix/src/CollabContracts.h"
#include <cstdint>
#include <vector>

// Independent test wire fixtures, not a replacement for R3's production codec.
namespace livemix_webguest_fixture
{
namespace c = gocue::livemix::collab::contracts;
inline c::WebConnection connection(uint32_t generation = 1)
{
    c::WebConnection value;
    value.session = 0x123456789abcdef0ULL;
    value.guestId = 2;
    value.generation = generation;
    value.rosterRevision = 1;
    return value;
}
inline void put(std::vector<uint8_t>& packet, size_t offset, uint64_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) packet[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}
inline uint32_t sequence(const uint8_t* packet)
{
    return uint32_t(packet[20]) | (uint32_t(packet[21]) << 8)
        | (uint32_t(packet[22]) << 16) | (uint32_t(packet[23]) << 24);
}
inline std::vector<uint8_t> audio(bool mono, uint32_t seq, c::WebConnection value = connection())
{
    const size_t payload = mono ? 240 : 720;
    std::vector<uint8_t> p(48 + payload);
    put(p, 0, 0x42434d4c, 4);
    p[4] = 2; p[5] = 1;
    put(p, 6, mono ? 2 : 1, 2);
    put(p, 8, value.session, 8);
    put(p, 16, mono ? value.guestId : value.hostId, 4);
    put(p, 20, seq, 4);
    put(p, 24, uint64_t(seq) * 120, 8);
    put(p, 32, 120, 2);
    put(p, 34, payload, 2);
    put(p, 36, mono ? value.guestId : value.hostId, 4);
    put(p, 40, mono ? value.guestEpoch : value.hostEpoch, 4);
    put(p, 44, value.rosterRevision, 4);
    for (uint32_t frame = 0; frame < 120; ++frame)
    {
        if (mono)
        {
            const auto sample = int32_t((seq * 997 + frame * 131) % 65536) - 32768;
            put(p, 48 + frame * 2, static_cast<uint32_t>(sample), 2);
        }
        else
        {
            const auto left = int32_t((seq * 7919 + frame * 104729) % 16777216) - 8388608;
            const auto right = 8388607 - int32_t((seq * 3571 + frame * 65537) % 16777216);
            put(p, 48 + frame * 6, static_cast<uint32_t>(left), 3);
            put(p, 51 + frame * 6, static_cast<uint32_t>(right), 3);
        }
    }
    return p;
}
inline std::vector<uint8_t> control(bool guest, c::WebConnection value = connection())
{
    auto p = audio(guest, 0, value);
    p.resize(48);
    p[5] = 12; // rosterAck: enough to exercise the independent control queue
    put(p, 6, 0, 2); put(p, 32, 0, 2); put(p, 34, 0, 2); put(p, 40, 0, 4);
    return p;
}
}
