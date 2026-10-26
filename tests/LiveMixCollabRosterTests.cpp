#include "CollabRoster.h"
#include <algorithm>
#include <limits>

namespace gocue::tests
{
using namespace livemix::collab;
namespace c = livemix::collab::contracts;
class LiveMixCollabRosterTests : public juce::UnitTest
{
public:
    LiveMixCollabRosterTests() : UnitTest ("LiveMix Collab roster", "LiveMix") {}
    void runTest() override
    {
        beginTest ("v2 exact 48 byte LE header, formats and strict packet bounds");
        c::HeaderV2 h; h.session = 0x1020304050607080ULL; h.origin = 0x11223344; h.seq = 0x55667788;
        h.firstFrame = 120 * (1ULL << 32) + 120; h.frames = 120; h.payloadBytes = 720;
        h.flags = c::Flags::stereoPcm24; h.linkSender = 1; h.streamEpoch = 0x01020304; h.rosterRevision = 0x0a0b0c0d;
        std::array<uint8_t, 1201> packet {}; std::array<uint8_t, 720> pcm {}; pcm.fill (0xa5);
        const std::array<uint8_t, 48> golden {{
            0x4c,0x4d,0x43,0x42,2,1,1,0,0x80,0x70,0x60,0x50,0x40,0x30,0x20,0x10,
            0x44,0x33,0x22,0x11,0x88,0x77,0x66,0x55,0x78,0,0,0,0x78,0,0,0,
            120,0,0xd0,2,1,0,0,0,4,3,2,1,0x0d,0x0c,0x0b,0x0a }};
        expectEquals ((int) writePacketV2 (packet.data(), packet.size(), h, pcm.data()), 768);
        expect (std::equal (golden.begin(), golden.end(), packet.begin()));
        c::HeaderV2 decoded; const uint8_t* payload = nullptr;
        expect (readPacketV2 (packet.data(), 768, decoded, payload));
        expect (decoded.firstFrame == h.firstFrame && decoded.session == h.session && decoded.streamEpoch == h.streamEpoch);
        expect (payload == packet.data() + 48 && std::equal (pcm.begin(), pcm.end(), payload));
        for (size_t bytes : { size_t (47), size_t (767), size_t (769), size_t (1201) })
            expect (! readPacketV2 (packet.data(), bytes, decoded, payload));
        for (size_t at : { size_t (0), size_t (4), size_t (5), size_t (6), size_t (32), size_t (34) })
        {
            auto bad = packet; bad[at] = 0xff; expect (! readPacketV2 (bad.data(), 768, decoded, payload));
        }
        for (int flags : { 0, 3, 4, 0x8001 })
        {
            auto bad = h; bad.flags = (c::Flags) flags; expect (writePacketV2 (packet.data(), packet.size(), bad, pcm.data()) == 0);
        }
        auto bad = h; ++bad.firstFrame; expect (! validHeaderV2 (bad));
        bad = h; bad.streamEpoch = 0; expect (! validHeaderV2 (bad));
        h.flags = c::Flags::monoPcm16; h.payloadBytes = 240;
        expectEquals ((int) writePacketV2 (packet.data(), packet.size(), h, pcm.data()), 288);
        expect (readPacketV2 (packet.data(), 288, decoded, payload));
        expect (! writePacketV2 (packet.data(), 287, h, pcm.data()));

        beginTest ("mono PCM16 finite clamp round and signed decode");
        const float input[] { -2, 2, -1, 0.5f, -0.5f, 0.5f / 32768, -0.5f / 32768,
                              std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() };
        float output[9] {};
        packPcm16 (pcm.data(), input, 9); unpackPcm16 (pcm.data(), output, 9);
        const float expected[] { -1, 32767.0f / 32768, -1, 0.5f, -0.5f, 1.0f / 32768, -1.0f / 32768, 0, 0 };
        for (int n = 0; n < 9; ++n) expectEquals (output[n], expected[n]);
        expect (pcm[0] == 0 && pcm[1] == 0x80 && pcm[2] == 0xff && pcm[3] == 0x7f);
        packPcm16 (pcm.data(), nullptr, 120); expect (std::all_of (pcm.begin(), pcm.begin() + 240, [] (auto b) { return b == 0; }));

        beginTest ("offer, ACK, restart, ping and empty control payloads");
        OfferV2 offer { 0xffff, 44100, 2, 0x11223344 }, copy;
        expectEquals ((int) writeOfferV2 (pcm.data(), pcm.size(), offer), 16);
        expect (pcm[0] == 31 && pcm[4] == 0x44 && pcm[5] == 0xac && pcm[12] == 0x44 && pcm[15] == 0x11);
        expect (readOfferV2 (pcm.data(), 16, copy));
        expect (copy.capabilities == 31 && copy.connectionGeneration == offer.connectionGeneration && copy.peerParticipantId == 2);
        expect (! readOfferV2 (pcm.data(), 15, copy)); expect (! readOfferV2 (pcm.data(), 17, copy));
        StreamRestartV2 restart { 96000, 1200, 3400 }, restartCopy;
        expectEquals ((int) writeStreamRestartV2 (pcm.data(), pcm.size(), restart), 12);
        expect (readStreamRestartV2 (pcm.data(), 12, restartCopy));
        expect (restartCopy.deviceRate == 96000 && restartCopy.inputLatencyUs == 1200 && restartCopy.outputLatencyUs == 3400);
        expect (! readStreamRestartV2 (pcm.data(), 11, restartCopy));
        for (auto kind : { c::Kind::ping, c::Kind::pong, c::Kind::bye, c::Kind::punch, c::Kind::offer,
                           c::Kind::offerAck, c::Kind::rosterAck, c::Kind::streamRestart })
        {
            c::HeaderV2 control; control.kind = kind; control.session = 17; control.origin = control.linkSender = 2; control.rosterRevision = 1;
            control.payloadBytes = kind == c::Kind::ping || kind == c::Kind::pong ? 8
                : kind == c::Kind::offer || kind == c::Kind::offerAck ? 16 : kind == c::Kind::streamRestart ? 12 : 0;
            control.streamEpoch = kind == c::Kind::streamRestart ? 2 : 0;
            auto bytes = writePacketV2 (packet.data(), packet.size(), control, pcm.data());
            expect (bytes > 0 && readPacketV2 (packet.data(), bytes, decoded, payload));
            ++control.payloadBytes; expect (! validHeaderV2 (control)); --control.payloadBytes;
            control.origin = 3; expect (! validHeaderV2 (control)); control.origin = 2;
            control.firstFrame = 120; expect (! validHeaderV2 (control));
        }
        for (int kind : { 2, 3, 7, 14 })
        {
            c::HeaderV2 control; control.session = 17; control.origin = control.linkSender = 1;
            control.rosterRevision = 1; control.kind = (c::Kind) kind; expect (! validHeaderV2 (control));
        }
        c::HeaderV2 pendingBye; pendingBye.kind = c::Kind::bye; pendingBye.session = 17;
        pendingBye.origin = pendingBye.linkSender = 3; // cancellation before the first roster
        expect (validHeaderV2 (pendingBye));

        beginTest ("roster round trip, max UTF-8 scalar and payload sizes");
        RosterSnapshot r; r.session = 17; r.revision = 1; r.memberCount = 3;
        for (size_t n = 0; n < 3; ++n)
        {
            auto& m = r.members[n]; m.participantId = (uint32_t) n + 1; m.capabilities = 31;
            m.nickname = juce::String::repeatedString (juce::String::fromUTF8 ("😀"), 40);
            m.appVersion = juce::String::repeatedString ("v", 32); m.state = c::ConnectionState::connected;
        }
        r.members[2].kind = c::ParticipantKind::web;
        auto size = writeRoster (pcm.data(), pcm.size(), r); expectEquals ((int) size, 674);
        RosterSnapshot result;
        expect (readRoster (pcm.data(), size, 17, 1, 2, result));
        expect (result.members[2].kind == c::ParticipantKind::web && result.members[1].nickname == r.members[1].nickname);
        for (auto& m : r.members) { m.nickname = "A"; m.appVersion = "0.17.0"; }
        size = writeRoster (pcm.data(), pcm.size(), r); expectEquals ((int) size, 119);
        expect (pcm[4] == 3 && pcm[5] == 1 && pcm[8] == 1 && pcm[45] == 2 && pcm[82] == 3);
        expect (! readRoster (pcm.data(), size, 17, 1, 4, result));

        beginTest ("reject incomplete, surplus, invalid UTF-8, IDs, epochs and two web members");
        for (size_t bytes : { size_t (0), size_t (7), size - 1, size + 1 })
            expect (! readRoster (pcm.data(), bytes, 17, 1, 2, result));
        const auto rejectByte = [&] (size_t at, uint8_t value)
        {
            auto broken = pcm; broken[at] = value;
            expect (! readRoster (broken.data(), size, 17, 1, 2, result));
        };
        rejectByte (6, 1); rejectByte (4, 4); rejectByte (5, 2); rejectByte (8, 0); rejectByte (8, 2);
        rejectByte (45, 1); rejectByte (49, 0); rejectByte (69, 2); // duplicate, zero epoch, second web
        rejectByte (33, 3); rejectByte (37, 1); rejectByte (34, 0); rejectByte (36, 33);
        rejectByte (38, 0); rejectByte (38, 0xc3); rejectByte (38, 0x80); rejectByte (38, 0x20);
        auto invalid = r; invalid.members[1].nickname = juce::String::repeatedString ("x", 41);
        expect (writeRoster (packet.data(), packet.size(), invalid) == 0);
        invalid = r; invalid.members[0].capabilities = 7; expect (! validateRoster (invalid, 2));
        invalid = r; invalid.mode = c::RoomMode::legacy2; expect (! validateRoster (invalid, 2));

        beginTest ("same revision ACK without reinstall, lower ignored, higher complete replacement");
        CollabRoster installed;
        expect (installed.apply (r, 2) == CollabRoster::Result::replaced);
        auto next = r; next.members[1].nickname = "new name";
        expect (installed.apply (next, 2) == CollabRoster::Result::unchanged);
        expectEquals (installed.snapshot().members[1].nickname, juce::String ("A"));
        next.revision = 2; next.memberCount = 2; next.members[1].streamEpoch = 2;
        expect (installed.apply (next, 2) == CollabRoster::Result::replaced);
        expect (installed.snapshot().memberCount == 2 && installed.snapshot().members[1].streamEpoch == 2);
        expect (installed.apply (r, 2) == CollabRoster::Result::ignored);
        next.revision = 3; next.members[1].streamEpoch = 1;
        expect (installed.apply (next, 2) == CollabRoster::Result::rejected);
        expectEquals (installed.snapshot().revision, 2u);
    }
};
static LiveMixCollabRosterTests liveMixCollabRosterTests;
}
