#include "CollabRoster.h"
#include <cstring>

namespace gocue::livemix::collab
{
namespace
{
namespace c = contracts;
void put (uint8_t* p, uint32_t value, size_t count = 4)
{
    for (size_t n = 0; n < count; ++n) p[n] = (uint8_t) (value >> (n * 8));
}
uint32_t get (const uint8_t* p, size_t count = 4)
{
    uint32_t value = 0;
    for (size_t n = 0; n < count; ++n) value |= (uint32_t) p[n] << (n * 8);
    return value;
}
// Validate the complete byte span, including the last scalar. JUCE's permissive
// text conversion must not repair a truncated name or hide an embedded NUL.
bool textValid (const uint8_t* p, size_t bytes, size_t maxScalars)
{
    size_t scalars = 0;
    for (size_t at = 0; at < bytes;)
    {
        uint32_t cp = p[at++], minimum = 0;
        size_t extra = 0;
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; extra = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; extra = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; extra = 3; minimum = 0x10000; }
        else if (cp >= 0x80) return false;
        if (extra > bytes - at) return false;
        while (extra-- > 0)
        {
            const auto next = p[at++];
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)
            || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || ++scalars > maxScalars) return false;
    }
    return true;
}
bool memberTextValid (const RosterMember& m)
{
    const auto nick = m.nickname.getNumBytesAsUTF8(), ver = m.appVersion.getNumBytesAsUTF8();
    return nick > 0 && nick <= c::nicknameUtf8Bytes && ver <= c::appVersionBytes
        && textValid ((const uint8_t*) m.nickname.toRawUTF8(), nick, c::nicknameCodePoints)
        && textValid ((const uint8_t*) m.appVersion.toRawUTF8(), ver, c::appVersionBytes)
        && m.nickname == m.nickname.trim();
}
}
const RosterMember* findMember (const RosterSnapshot& r, contracts::ParticipantId id) noexcept
{
    for (size_t n = 0; n < r.memberCount && n < r.members.size(); ++n)
        if (r.members[n].participantId == id) return &r.members[n];
    return nullptr;
}
bool validateRoster (const RosterSnapshot& r, contracts::ParticipantId self, const RosterSnapshot* previous)
{
    if (r.session == 0 || r.revision == 0 || self == 0 || ! isValidWireRate (r.wireRate)
        || r.memberCount == 0 || r.memberCount > r.members.size()
        || (r.mode != c::RoomMode::legacy2 && r.mode != c::RoomMode::v2)
        || ! findMember (r, self) || r.members[0].participantId != c::hostParticipant
        || r.members[0].kind != c::ParticipantKind::native) return false;
    if (previous != nullptr && previous->revision != 0
        && (r.session != previous->session || r.wireRate != previous->wireRate)) return false;
    unsigned web = 0;
    for (size_t n = 0; n < r.memberCount; ++n)
    {
        const auto& m = r.members[n];
        if (m.participantId == 0 || (n > 0 && m.participantId <= r.members[n - 1].participantId)
            || m.streamEpoch == 0 || m.deviceRate == 0 || ! memberTextValid (m)
            || (m.state != c::ConnectionState::connecting && m.state != c::ConnectionState::connected)
            || (m.kind != c::ParticipantKind::native && m.kind != c::ParticipantKind::web)) return false;
        if (m.kind == c::ParticipantKind::web) ++web;
        if (r.mode == c::RoomMode::v2 && (m.capabilities & c::requiredNativeV2Capabilities) != c::requiredNativeV2Capabilities) return false;
        if (previous != nullptr)
            if (const auto* old = findMember (*previous, m.participantId))
                if (m.streamEpoch < old->streamEpoch || m.kind != old->kind) return false;
    }
    if (web > 1 || (r.mode == c::RoomMode::legacy2 && (web != 0 || r.memberCount > 2))) return false;
    if (web != 0)
        for (size_t n = 0; n < r.memberCount; ++n)
            if ((r.members[n].capabilities & c::requiredWebCapabilities) != c::requiredWebCapabilities) return false;
    return true;
}
size_t writeRoster (uint8_t* p, size_t capacity, const RosterSnapshot& r)
{
    if (p == nullptr || ! validateRoster (r, c::hostParticipant)) return 0;
    size_t bytes = 8;
    for (size_t n = 0; n < r.memberCount; ++n)
        bytes += 30 + r.members[n].nickname.getNumBytesAsUTF8() + r.members[n].appVersion.getNumBytesAsUTF8();
    if (capacity < bytes) return 0;
    put (p, r.wireRate); p[4] = (uint8_t) r.memberCount; p[5] = (uint8_t) r.mode; put (p + 6, 0, 2);
    size_t at = 8;
    for (size_t n = 0; n < r.memberCount; ++n)
    {
        const auto& m = r.members[n]; auto* e = p + at;
        const auto nick = m.nickname.getNumBytesAsUTF8(), ver = m.appVersion.getNumBytesAsUTF8();
        put (e, m.participantId); put (e + 4, m.streamEpoch); put (e + 8, m.capabilities & c::allCapabilities);
        put (e + 12, m.deviceRate); put (e + 16, m.inputLatencyUs); put (e + 20, m.outputLatencyUs);
        e[24] = (uint8_t) m.kind; e[25] = (uint8_t) m.state; put (e + 26, (uint32_t) nick, 2);
        e[28] = (uint8_t) ver; e[29] = 0;
        std::memcpy (e + 30, m.nickname.toRawUTF8(), nick); std::memcpy (e + 30 + nick, m.appVersion.toRawUTF8(), ver);
        at += 30 + nick + ver;
    }
    return bytes;
}
bool readRoster (const uint8_t* p, size_t bytes, uint64_t session, contracts::Revision revision,
                 contracts::ParticipantId self, RosterSnapshot& result)
{
    if (p == nullptr || bytes < 8 || bytes > 674 || get (p + 6, 2) != 0 || p[4] == 0 || p[4] > 3) return false;
    RosterSnapshot r; r.session = session; r.revision = revision; r.wireRate = get (p);
    r.memberCount = p[4]; r.mode = (c::RoomMode) p[5];
    size_t at = 8;
    for (size_t n = 0; n < r.memberCount; ++n)
    {
        if (bytes - at < 30) return false;
        const auto* e = p + at;
        const size_t nick = get (e + 26, 2), ver = e[28];
        if (e[29] != 0 || nick == 0 || nick > 160 || ver > 32 || nick + ver > bytes - at - 30
            || ! textValid (e + 30, nick, 40) || ! textValid (e + 30 + nick, ver, 32)) return false;
        auto& m = r.members[n];
        m.participantId = get (e); m.streamEpoch = get (e + 4); m.capabilities = get (e + 8) & c::allCapabilities;
        m.deviceRate = get (e + 12); m.inputLatencyUs = get (e + 16); m.outputLatencyUs = get (e + 20);
        m.kind = (c::ParticipantKind) e[24]; m.state = (c::ConnectionState) e[25];
        m.nickname = juce::String::fromUTF8 ((const char*) e + 30, (int) nick);
        m.appVersion = juce::String::fromUTF8 ((const char*) e + 30 + nick, (int) ver);
        at += 30 + nick + ver;
    }
    if (at != bytes || ! validateRoster (r, self)) return false;
    result = std::move (r); return true;
}
CollabRoster::Result CollabRoster::apply (const RosterSnapshot& r, contracts::ParticipantId self)
{
    if (r.session == installed.session && r.revision != 0 && r.revision < installed.revision) return Result::ignored;
    if (! validateRoster (r, self, installed.revision != 0 ? &installed : nullptr)) return Result::rejected;
    if (r.revision < installed.revision) return Result::ignored;
    if (r.revision == installed.revision) return Result::unchanged;
    installed = r; return Result::replaced;
}
}
