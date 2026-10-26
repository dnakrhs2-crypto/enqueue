#pragma once
#include "CollabPlayout.h"
#include "CollabSender.h"
#include "CollabRoster.h"
#include "CollabAudio.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gocue::tests::collabtest
{
using namespace livemix::collab;
struct Packet
{
    Header header;
    std::array<uint8_t, audioBytes> payload {};
    uint64_t arrival = 0;
};
inline Packet tone (uint64_t number, float level = 0.25f, double hz = 0, bool stereo = true, uint32_t wireRate = defaultWireRate)
{
    Packet p; p.header.firstFrame = number * framesPerPacket; p.header.sender = 1;
    p.header.frames = framesPerPacket; p.header.payloadBytes = audioBytes;
    std::array<float, framesPerPacket> left, right;
    for (int n = 0; n < framesPerPacket; ++n)
    {
        left[(size_t) n] = hz == 0 ? level : level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (p.header.firstFrame + n) / wireRate);
        right[(size_t) n] = stereo ? left[(size_t) n] : 0.0f;
    }
    packPcm24 (p.payload.data(), left.data(), right.data(), framesPerPacket);
    p.arrival = (uint64_t) (p.header.firstFrame * 1.0e9 / wireRate);
    return p;
}
inline bool push (CollabPlayout& play, const Packet& p) { return play.push (p.header, p.payload.data(), p.arrival); }
inline RosterSnapshot rosterFor (std::initializer_list<uint32_t> ids, uint32_t revision = 1, uint32_t webId = 0)
{
    RosterSnapshot result; result.session = 0xfedcba9876543210ULL; result.revision = revision;
    for (auto id : ids)
    {
        auto& m = result.members[result.memberCount++]; m.participantId = id;
        m.nickname = "person" + juce::String (id); m.appVersion = "0.17.0";
        m.capabilities = contracts::allCapabilities; m.state = contracts::ConnectionState::connected;
        m.kind = id == webId ? contracts::ParticipantKind::web : contracts::ParticipantKind::native;
    }
    return result;
}
inline void receiveTone (CollabAudio& audio, uint32_t pid, uint64_t number, float level, bool web = false, uint32_t revision = 1)
{
    auto p = tone (number, level);
    contracts::HeaderV2 h; h.session = 0xfedcba9876543210ULL; h.origin = pid; h.linkSender = 1;
    h.seq = (uint32_t) number; h.firstFrame = number * 120; h.streamEpoch = 1; h.rosterRevision = revision;
    h.frames = 120; h.payloadBytes = web ? 240 : 720;
    h.flags = web ? contracts::Flags::monoPcm16 : contracts::Flags::stereoPcm24;
    if (web) { std::array<float, 120> mono; mono.fill (level); packPcm16 (p.payload.data(), mono.data(), 120); }
    audio.receiveV2 (h, p.payload.data(), p.arrival);
}
inline double amplitude (const std::vector<float>& signal, double rate, double hz)
{
    double real = 0, imag = 0, weight = 0;
    for (size_t i = 0; i < signal.size(); ++i)
    {
        const double window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (signal.size() - 1));
        const double phase = juce::MathConstants<double>::twoPi * hz * i / rate;
        real += signal[i] * window * std::cos (phase); imag += signal[i] * window * std::sin (phase); weight += window;
    }
    return 2.0 * std::hypot (real, imag) / weight;
}
}
