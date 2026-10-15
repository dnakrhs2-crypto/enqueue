#include "CollabProtocol.h"
#include <cmath>

namespace gocue::tests
{
using namespace livemix::collab;
class LiveMixCollabProtocolTests : public juce::UnitTest
{
public:
    LiveMixCollabProtocolTests() : UnitTest ("LiveMix Collab protocol", "LiveMix") {}
    void runTest() override
    {
        beginTest ("36-byte little endian header and PCM packet round trip");
        std::array<uint8_t, maxDatagram> packet {};
        std::array<uint8_t, audioBytes> pcm {};
        Header h; h.session = 0x1020304050607080ULL; h.sender = 0x11223344; h.seq = 0x55667788;
        h.firstFrame = 120 * 123456789ULL; h.frames = framesPerPacket; h.payloadBytes = audioBytes;
        const auto size = writePacket (packet.data(), packet.size(), h, pcm.data());
        expectEquals ((int) size, 756);
        expect (packet[0] == 'L' && packet[1] == 'M' && packet[2] == 'C' && packet[3] == 'B');
        expect (packet[8] == 0x80 && packet[15] == 0x10 && packet[16] == 0x44 && packet[20] == 0x88);
        Header read; const uint8_t* payload = nullptr;
        expect (readPacket (packet.data(), size, read, payload));
        expect (read.session == h.session && read.sender == h.sender && read.seq == h.seq && read.firstFrame == h.firstFrame);
        expect (read.frames == 120 && read.payloadBytes == 720 && payload == packet.data() + 36);

        beginTest ("bad magic, version, truncation, trailing bytes and payload size rejected");
        for (int at : { 0, 4, 5, 6, 32, 34 })
        {
            auto broken = packet; broken[(size_t) at] = 0xff;
            expect (! readPacket (broken.data(), size, read, payload));
        }
        expect (! readPacket (packet.data(), 35, read, payload));
        expect (! readPacket (packet.data(), size - 1, read, payload));
        expect (! readPacket (packet.data(), size + 1, read, payload));
        expectEquals ((int) writePacket (packet.data(), 35, h, pcm.data()), 0);
        for (int frames : { 119, 121, 240 })
        {
            auto bad = h; bad.frames = (uint16_t) frames;
            expectEquals ((int) writePacket (packet.data(), packet.size(), bad, pcm.data()), 0);
            auto broken = packet; broken[32] = (uint8_t) frames; broken[33] = 0;
            expect (! readPacket (broken.data(), size, read, payload));
        }

        beginTest ("PCM24 stereo round trip and clipping");
        std::array<float, framesPerPacket> left, right;
        std::array<float, framesPerPacket * 2> decoded;
        for (int n = 0; n < framesPerPacket; ++n) { left[(size_t) n] = (n - 60) / 60.0f; right[(size_t) n] = -left[(size_t) n] * 0.75f; }
        packPcm24 (pcm.data(), left.data(), right.data(), framesPerPacket);
        unpackPcm24 (pcm.data(), decoded.data(), framesPerPacket);
        double error = 0;
        for (int n = 0; n < framesPerPacket; ++n)
        {
            error = std::max (error, (double) std::abs (left[(size_t) n] - decoded[(size_t) n * 2]));
            error = std::max (error, (double) std::abs (right[(size_t) n] - decoded[(size_t) n * 2 + 1]));
        }
        expectLessThan (error, 1.0e-6);
        left[0] = -3; left[1] = 3; right[0] = 1;
        packPcm24 (pcm.data(), left.data(), right.data(), framesPerPacket); unpackPcm24 (pcm.data(), decoded.data(), framesPerPacket);
        expectEquals (decoded[0], -1.0f); expectEquals (decoded[1], 8388607.0f / 8388608.0f); expectEquals (decoded[2], decoded[1]);

        beginTest ("UTF-8 metadata, ping echo, empty bye and exact probe layout");
        PeerInfo info { juce::String::repeatedString (juce::String::fromUTF8 ("합방테스트"), 20), "0.15.0", 1.234, 5.678, 44100 };
        const auto count = writePeerInfo (pcm.data(), info);
        PeerInfo copy;
        expect (readPeerInfo (pcm.data(), count, copy));
        expect (copy.nickname.getNumBytesAsUTF8() <= 64 && info.nickname.startsWith (copy.nickname));
        expectEquals (copy.appVersion, info.appVersion);
        expectWithinAbsoluteError (copy.inputLatencyMs, info.inputLatencyMs, 0.0001);
        expectWithinAbsoluteError (copy.outputLatencyMs, info.outputLatencyMs, 0.0001);
        expect (copy.deviceRate == 44100);
        expect (! readPeerInfo (pcm.data(), count - 1, copy));
        for (Kind kind : { Kind::hello, Kind::welcome, Kind::ping, Kind::pong, Kind::bye, Kind::probe })
        {
            Header control; control.kind = kind;
            control.payloadBytes = (uint16_t) (kind == Kind::probe ? 16 : kind == Kind::bye ? 0
                : kind == Kind::ping || kind == Kind::pong ? 8 : count);
            const auto bytes = writePacket (packet.data(), packet.size(), control, pcm.data());
            expect (bytes > 0 && readPacket (packet.data(), bytes, read, payload));
        }
        writeTimestamp (pcm.data(), 0x1122334455667788ULL);
        expect (readTimestamp (pcm.data()) == 0x1122334455667788ULL);
        expect (packet[4] == 1 && packet[5] == 7 && packet[34] == 16);

        beginTest ("hello and welcome round trip both room rates, reject all other rates");
        for (uint32_t rate : { 44100u, 48000u })
        {
            info.wireRate = rate;
            info.appVersion = juce::String::repeatedString ("v", 32);
            const auto bytes = writePeerInfo (pcm.data(), info);
            expect (bytes <= 114);
            for (auto kind : { Kind::hello, Kind::welcome })
            {
                Header control; control.kind = kind; control.payloadBytes = (uint16_t) bytes;
                const auto total = writePacket (packet.data(), packet.size(), control, pcm.data());
                expect (readPacket (packet.data(), total, read, payload));
                expect (readPeerInfo (payload, read.payloadBytes, copy));
                expectEquals (copy.wireRate, rate); expectEquals (copy.deviceRate, 44100u);
            }
            for (uint32_t invalid : { 0u, 32000u, 96000u })
            {
                auto bad = info; bad.wireRate = invalid;
                expectEquals ((int) writePeerInfo (packet.data(), bad), 0);
                auto broken = pcm;
                for (int i = 0; i < 4; ++i) broken[bytes - 4 + i] = (uint8_t) (invalid >> (8 * i));
                expect (! readPeerInfo (broken.data(), bytes, copy));
            }
        }
    }
};
static LiveMixCollabProtocolTests liveMixCollabProtocolTests;
}
