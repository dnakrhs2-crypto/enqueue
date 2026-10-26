#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// R0 contract only. Existing collab::Header/Kind and the v1 codec stay unchanged.
// See docs/superpowers/specs/2026-10-26-livemix-webguest-record-contracts.md.
namespace gocue::livemix::collab::contracts
{
using ParticipantId = std::uint32_t;
using StreamEpoch = std::uint32_t;
using Revision = std::uint32_t;
using ConnectionGeneration = std::uint32_t;

inline constexpr std::size_t maxParticipants = 3, remoteSlots = 2, maxWebGuests = 1;
inline constexpr std::size_t nicknameCodePoints = 40, nicknameUtf8Bytes = 160, appVersionBytes = 32;
inline constexpr std::uint32_t packetMagic = 0x42434d4c;
inline constexpr std::uint8_t packetVersion = 2;
inline constexpr std::size_t v1HeaderBytes = 36, v2HeaderBytes = 48, maxPacketBytes = 1200;
inline constexpr std::uint16_t packetFrames = 120;
inline constexpr std::size_t stereoPcm24Bytes = 720, monoPcm16Bytes = 240;
inline constexpr std::uint32_t defaultWireRate = 48000;
inline constexpr ParticipantId noParticipant = 0, hostParticipant = 1;
inline constexpr std::uint32_t controlRetryMs = 200;
inline constexpr std::uint32_t pendingRequestSeconds = 60, reservationSeconds = 15;
inline constexpr std::uint32_t eventWaitSeconds = 20, eventTimeoutMs = 30000;
inline constexpr std::size_t maxPendingWebRequests = 8, maxSdpBytes = 65536;
inline constexpr std::size_t maxCandidateBytes = 2048, maxCandidatesPerSide = 64;
inline constexpr std::size_t maxWebBodyBytes = 131072, maxEventsPerReply = 32;

namespace offset
{
inline constexpr std::size_t magic = 0, version = 4, kind = 5, flags = 6;
inline constexpr std::size_t session = 8, origin = 16, seq = 20, firstFrame = 24;
inline constexpr std::size_t frames = 32, payloadBytes = 34;
inline constexpr std::size_t linkSender = 36, streamEpoch = 40, rosterRevision = 44;
}

// Values 2, 3 and 7 remain v1-only hello, welcome and probe respectively.
enum class Kind : std::uint8_t
{
    audio = 1, ping = 4, pong = 5, bye = 6, punch = 8,
    offer = 9, offerAck = 10, roster = 11, rosterAck = 12, streamRestart = 13
};
// Exactly one audio format, or none for control packets; not combinable formats.
enum class Flags : std::uint16_t { none = 0, stereoPcm24 = 1, monoPcm16 = 2 };
enum class Capability : std::uint32_t
{
    packetV2 = 1u << 0, rosterAck = 1u << 1, star3 = 1u << 2,
    pcm16Mono = 1u << 3, webGuest = 1u << 4
};
struct CapabilityName { Capability bit; const char* name; };
inline constexpr std::array<CapabilityName, 5> capabilityNames {{
    { Capability::packetV2, "packet_v2" }, { Capability::rosterAck, "roster_ack" },
    { Capability::star3, "star_3" }, { Capability::pcm16Mono, "pcm16_mono" },
    { Capability::webGuest, "web_guest" }
}};
inline constexpr std::uint32_t requiredNativeV2Capabilities = 0x07, requiredWebCapabilities = 0x1f;
inline constexpr std::uint32_t allCapabilities = 0x1f;
enum class RoomMode : std::uint8_t { legacy2 = 0, v2 = 1 };
enum class ParticipantKind : std::uint8_t { none = 0, native = 1, web = 2 };
enum class ConnectionState : std::uint8_t { empty = 0, connecting = 1, connected = 2, disconnected = 3 };

// Logical fields, NOT a packed wire object. Encode/decode each LE field explicitly.
struct HeaderV2
{
    Kind kind = Kind::audio;
    Flags flags = Flags::none;
    std::uint64_t session = 0;
    ParticipantId origin = noParticipant;
    std::uint32_t seq = 0;
    std::uint64_t firstFrame = 0;
    std::uint16_t frames = 0, payloadBytes = 0;
    ParticipantId linkSender = noParticipant;
    StreamEpoch streamEpoch = 0;
    Revision rosterRevision = 0;
};

// A callback-local planar view, valid until the next renderSources/prepare call.
// A null channel is silence. Mono views expose the same samples on left/right.
struct AudioSourceView
{
    const float* left = nullptr;
    const float* right = nullptr;
    ParticipantId participantId = noParticipant;
    StreamEpoch streamEpoch = 0;
    ParticipantKind kind = ParticipantKind::none;
    std::uint8_t channels = 2;
    bool connected = false;
};
struct RenderedSources
{
    std::array<AudioSourceView, remoteSlots> remote {};
    int frames = 0;
};

// Transport context contains no HTTP credentials. Established by web/accept.
struct WebConnection
{
    std::uint64_t session = 0;
    ParticipantId hostId = hostParticipant, guestId = noParticipant;
    ConnectionGeneration generation = 0;
    StreamEpoch hostEpoch = 1, guestEpoch = 1;
    Revision rosterRevision = 0;
    std::uint32_t wireRate = defaultWireRate;
};

inline constexpr char webOrigin[] = "https://xn--jb0byyo90f.com";
inline constexpr char joinPath[] = "/livemix/join/";
inline constexpr char apiBase[] = "https://xn--2j1bo5s81gjmag88b.com/api/livemix/collab/";
inline constexpr char dataChannelLabel[] = "livemix-audio-v2";
inline constexpr char dataChannelProtocol[] = "livemix-pcm-v2";
inline constexpr std::array<const char*, 2> iceServers {{
    "stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478"
}};
inline constexpr bool dataChannelOrdered = false;
inline constexpr unsigned dataChannelMaxRetransmits = 0;

static_assert(offset::rosterRevision + sizeof(Revision) == v2HeaderBytes);
static_assert(v2HeaderBytes + stereoPcm24Bytes == 768);
static_assert(v2HeaderBytes + monoPcm16Bytes == 288);
}
