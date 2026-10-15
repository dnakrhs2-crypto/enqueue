#pragma once
#include "CollabPlayout.h"
#include "CollabSender.h"
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
