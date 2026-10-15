#include "LiveMixCollabTestHelpers.h"

namespace gocue::tests
{
using namespace collabtest;
class LiveMixCollabSenderTests : public juce::UnitTest
{
public:
    LiveMixCollabSenderTests() : UnitTest ("LiveMix Collab sender and rates", "LiveMix") {}
    void runTest() override
    {
        beginTest ("arbitrary callback sizes, silence and monotonic firstFrame across device restart");
        {
            CollabSender sender; sender.prepare (48000, 256);
            uint64_t expectedFrame = 0; int packets = 0; bool content = true, numbering = true;
            auto sink = [&] (const Header& h, const uint8_t* payload)
            {
                numbering = numbering && h.firstFrame == expectedFrame; expectedFrame += 120; ++packets;
                std::array<float, 240> decoded; unpackPcm24 (payload, decoded.data(), 120);
                content = content && std::all_of (decoded.begin(), decoded.end(), [] (float v) { return v == 0; });
            };
            for (int n : { 17, 239, 64, 400 }) { sender.push (nullptr, nullptr, n); sender.drain (sink); }
            expectEquals (packets, 6); expect (content && numbering);
            sender.prepare (44100, 441);
            sender.push (nullptr, nullptr, 441); sender.drain (sink);
            expect (packets >= 9 && numbering); expect (sender.getOverruns() == 0);
        }

        beginTest ("44.1 kHz capture -> 48 kHz packets -> 96 kHz playout: pitch and images");
        {
            CollabSender sender; sender.prepare (44100, 441);
            CollabPlayout play; play.prepare (96000, 960);
            std::array<float, 441> input;
            std::array<float, 960> left {}, right {};
            std::vector<float> signal;
            uint64_t sample = 0; bool numbering = true; uint64_t frame = 0;
            for (int block = 0; block < 800; ++block)
            {
                for (auto& v : input) v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000 * sample++ / 44100.0);
                sender.push (input.data(), input.data(), (int) input.size());
                sender.drain ([&] (const Header& header, const uint8_t* payload)
                {
                    auto h = header; h.sender = 1;
                    numbering = numbering && h.firstFrame == frame; frame += 120;
                    play.push (h, payload, (uint64_t) block * 10000000ULL);
                });
                play.pull (left.data(), right.data(), 960);
                if (block >= 600) signal.insert (signal.end(), left.begin(), left.end());
            }
            double firstCross = -1, lastCross = 0; int crossings = 0;
            for (size_t i = 1; i < signal.size(); ++i)
                if (signal[i - 1] <= 0 && signal[i] > 0)
                {
                    const double crossing = (double) i - signal[i] / (signal[i] - signal[i - 1]);
                    if (firstCross < 0) firstCross = crossing;
                    lastCross = crossing; ++crossings;
                }
            const double frequency = (crossings - 1) * 96000.0 / (lastCross - firstCross);
            const double fundamental = amplitude (signal, 96000, frequency);
            const double image = std::max (amplitude (signal, 96000, 44100 - frequency), amplitude (signal, 96000, 48000 - frequency));
            logMessage ("frequency=" + juce::String (frequency, 6) + " Hz, images=" + juce::String (20 * std::log10 (image / fundamental), 2) + " dBc");
            expect (numbering); expectWithinAbsoluteError (frequency, 1000.0, 0.5);
            expectLessThan (image / fundamental, 0.001); expectGreaterThan (fundamental, 0.49);
            expect (play.getStats().underruns == 0);
        }

        beginTest ("left-only pulses remain left-only across packetization and ASRC");
        {
            CollabSender sender; sender.prepare (44100, 441);
            CollabPlayout play; play.prepare (96000, 960);
            std::array<float, 441> input {}; input[200] = 0.5f;
            std::array<float, 960> left {}, right {};
            double energy = 0, rightEnergy = 0;
            for (int block = 0; block < 100; ++block)
            {
                sender.push (input.data(), nullptr, 441);
                sender.drain ([&] (const Header& h, const uint8_t* p) { play.push (h, p, (uint64_t) block * 10000000ULL); });
                play.pull (left.data(), right.data(), 960);
                for (int i = 0; i < 960; ++i) { energy += left[(size_t) i] * left[(size_t) i]; rightEnergy += right[(size_t) i] * right[(size_t) i]; }
            }
            expectGreaterThan (energy, 1.0); expectEquals (rightEnergy, 0.0);
        }

        for (const auto rates : { std::pair<int, int> { 48000, 44100 }, { 44100, 48000 }, { 44100, 44100 } })
        {
            beginTest ("44.1 kHz room, five simulated minutes: " + juce::String (rates.first) + " -> " + juce::String (rates.second));
            const int inputFrames = rates.first / 100, outputFrames = rates.second / 100;
            CollabSender sender; sender.prepare (rates.first, inputFrames, 44100);
            CollabPlayout play; play.prepare (rates.second, outputFrames, 44100);
            std::vector<float> input ((size_t) inputFrames), left ((size_t) outputFrames), right ((size_t) outputFrames), signal;
            uint64_t sample = 0, nextFrame = 0;
            bool numbering = true, stereo = true;
            for (int block = 0; block < 30000; ++block)
            {
                for (auto& v : input) v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000 * sample++ / rates.first);
                sender.push (input.data(), input.data(), inputFrames);
                sender.drain ([&] (const Header& h, const uint8_t* payload)
                {
                    numbering = numbering && h.firstFrame == nextFrame && h.frames == 120;
                    nextFrame += framesPerPacket;
                    play.push (h, payload, (uint64_t) block * 10000000ULL);
                });
                play.pull (left.data(), right.data(), outputFrames);
                stereo = stereo && left == right;
                if (block >= 29800) signal.insert (signal.end(), left.begin(), left.end());
            }
            double firstCross = -1, lastCross = 0; int crossings = 0;
            for (size_t i = 1; i < signal.size(); ++i)
                if (signal[i - 1] <= 0 && signal[i] > 0)
                {
                    const double crossing = (double) i - signal[i] / (signal[i] - signal[i - 1]);
                    if (firstCross < 0) firstCross = crossing;
                    lastCross = crossing; ++crossings;
                }
            const double frequency = (crossings - 1) * (double) rates.second / (lastCross - firstCross);
            logMessage ("frequency=" + juce::String (frequency, 6) + " Hz, underruns=" + juce::String (play.getStats().underruns));
            expectWithinAbsoluteError (frequency, 1000.0, 0.5);
            expectGreaterThan (amplitude (signal, rates.second, frequency), 0.49);
            expect (numbering && stereo); expect (sender.getOverruns() == 0);
            expect (play.getStats().underruns == 0 && play.getStats().lost == 0);
            expectWithinAbsoluteError ((double) nextFrame / 44100, 300.0, 0.01);
        }
    }
};
static LiveMixCollabSenderTests liveMixCollabSenderTests;
}
