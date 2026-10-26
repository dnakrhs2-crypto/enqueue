#include "ObsMetadata.h"
#include "lm_obs_metadata.h"
#include "lm_obs_protocol.h"

#include <sddl.h>
#include <vector>

namespace gocue::livemix
{
namespace
{
    // Match ObsSender's access for ordinary/elevated LiveMix and OBS processes.
    struct MappingSecurity
    {
        MappingSecurity()
        {
            HANDLE token = nullptr;
            if (! OpenProcessToken (GetCurrentProcess(), TOKEN_QUERY, &token)) return;
            DWORD bytes = 0;
            GetTokenInformation (token, TokenUser, nullptr, 0, &bytes);
            std::vector<unsigned char> storage (bytes);
            LPWSTR sid = nullptr;
            if (bytes > 0 && GetTokenInformation (token, TokenUser, storage.data(), bytes, &bytes)
                && ConvertSidToStringSidW (reinterpret_cast<TOKEN_USER*> (storage.data())->User.Sid, &sid))
            {
                const auto sddl = juce::String (L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;") + juce::String (sid) + ")S:(ML;;NW;;;ME)";
                ConvertStringSecurityDescriptorToSecurityDescriptorW (sddl.toWideCharPointer(), SDDL_REVISION_1, &descriptor, nullptr);
                LocalFree (sid);
            }
            CloseHandle (token);
        }
        ~MappingSecurity() { if (descriptor != nullptr) LocalFree (descriptor); }
        PSECURITY_DESCRIPTOR descriptor = nullptr;
    };
}

ObsMetadata::~ObsMetadata() { close(); }

bool ObsMetadata::open (const juce::String& resolvedPeerRing, unsigned slot)
{
    close();
    if (resolvedPeerRing.isEmpty() || slot < 1 || slot > 2) return false;
    MappingSecurity security;
    if (security.descriptor == nullptr) return false;
    SECURITY_ATTRIBUTES attributes { sizeof (attributes), security.descriptor, FALSE };
    const auto name = resolvedPeerRing + juce::String (LM_OBS_METADATA_SUFFIX);
    mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                  LM_OBS_METADATA_BYTES, name.toWideCharPointer());
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    if (mapping != nullptr)
        view = static_cast<lm_obs_metadata*> (MapViewOfFile (mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                                            0, 0, LM_OBS_METADATA_BYTES));
    if (view == nullptr)
    {
        close();
        return false;
    }
    if (! existed) std::memset (view, 0, sizeof (*view));
    slotNumber = slot;
    clear();
    return true;
}

void ObsMetadata::store (const lm_obs_metadata& next)
{
    // Retained mappings may survive an app restart. Publish their reinitialization
    // under the same sequence protocol instead of resetting a visible sequence.
    const auto odd = (static_cast<uint64_t> (lm_obs_load_acquire (&view->sequence)) + 1u) | uint64_t (1);
    lm_obs_store_release (&view->sequence, static_cast<int64_t> (odd));
    std::memcpy (view, &next, offsetof (lm_obs_metadata, sequence));
    constexpr auto payload = offsetof (lm_obs_metadata, session);
    std::memcpy (reinterpret_cast<char*> (view) + payload, reinterpret_cast<const char*> (&next) + payload,
                 sizeof (next) - payload);
    lm_obs_store_release (&view->sequence, static_cast<int64_t> (odd + 1u));
}

void ObsMetadata::publish (uint64_t session, collab::contracts::ParticipantId participant,
                           collab::contracts::StreamEpoch epoch, collab::contracts::ParticipantKind kind,
                           collab::contracts::ConnectionState state, const juce::String& nickname)
{
    if (view == nullptr) return;
    lm_obs_metadata next {};
    next.magic = LM_OBS_METADATA_MAGIC;
    next.version = LM_OBS_METADATA_VERSION;
    next.mapping_bytes = LM_OBS_METADATA_BYTES;
    next.slot = slotNumber;
    next.session = session;
    next.participant_id = participant;
    next.stream_epoch = epoch;
    next.kind = static_cast<uint32_t> (kind);
    next.state = static_cast<uint32_t> (state);
    next.writer_pid = GetCurrentProcessId();
    const auto* utf8 = nickname.toRawUTF8();
    auto bytes = std::min (nickname.getNumBytesAsUTF8(), size_t (LM_OBS_METADATA_NICK_BYTES));
    while (bytes > 0 && (static_cast<unsigned char> (utf8[bytes]) & 0xc0u) == 0x80u) --bytes;
    next.nickname_bytes = static_cast<uint32_t> (bytes);
    std::memcpy (next.nickname_utf8, utf8, bytes); // zero-initialized tail includes the NUL
    store (next);
}

void ObsMetadata::clear()
{
    publish (0, 0, 0, collab::contracts::ParticipantKind::none, collab::contracts::ConnectionState::empty, {});
}

void ObsMetadata::close()
{
    if (view != nullptr)
    {
        clear();
        UnmapViewOfFile (view);
        view = nullptr;
    }
    if (mapping != nullptr) CloseHandle (mapping);
    mapping = nullptr;
    slotNumber = 0;
}
}
