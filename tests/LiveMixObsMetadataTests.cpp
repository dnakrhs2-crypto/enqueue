#include "ObsMetadata.h"
#include "lm_obs_metadata.h"
#include "lm_obs_protocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gocue::tests
{
using namespace gocue::livemix;
using namespace gocue::livemix::collab::contracts;

namespace
{
    juce::String ringName() { return "Local\\LiveMix.ObsMetadataTest." + juce::Uuid().toString(); }

    struct MetadataView
    {
        explicit MetadataView (const juce::String& ring)
        {
            const auto name = ring + juce::String (LM_OBS_METADATA_SUFFIX);
            handle = OpenFileMappingW (FILE_MAP_READ, FALSE, name.toWideCharPointer());
            if (handle != nullptr)
                view = static_cast<const lm_obs_metadata*> (MapViewOfFile (handle, FILE_MAP_READ, 0, 0, LM_OBS_METADATA_BYTES));
        }
        ~MetadataView() { if (view != nullptr) UnmapViewOfFile (view); if (handle != nullptr) CloseHandle (handle); }
        HANDLE handle = nullptr;
        const lm_obs_metadata* view = nullptr;
    };

    template <typename T, size_t N> bool allZero (const T (&values)[N])
    {
        return std::all_of (std::begin (values), std::end (values), [] (T value) { return value == 0; });
    }
}

class LiveMixObsMetadataTests : public juce::UnitTest
{
public:
    LiveMixObsMetadataTests() : juce::UnitTest ("LiveMix ObsMetadata", "LiveMix") {}

    void runTest() override
    {
        beginTest ("the shared C/C++ ABI is 256 bytes, aligned to 8, with frozen offsets");
        expectEquals (int (sizeof (lm_obs_metadata)), 256);
        expectEquals (int (alignof (lm_obs_metadata)), 8);
        const std::array<size_t, 15> offsets {{ offsetof (lm_obs_metadata, magic), offsetof (lm_obs_metadata, version),
            offsetof (lm_obs_metadata, mapping_bytes), offsetof (lm_obs_metadata, slot), offsetof (lm_obs_metadata, sequence),
            offsetof (lm_obs_metadata, session), offsetof (lm_obs_metadata, participant_id), offsetof (lm_obs_metadata, stream_epoch),
            offsetof (lm_obs_metadata, state), offsetof (lm_obs_metadata, kind), offsetof (lm_obs_metadata, nickname_bytes),
            offsetof (lm_obs_metadata, writer_pid), offsetof (lm_obs_metadata, nickname_utf8), offsetof (lm_obs_metadata, reserved),
            sizeof (lm_obs_metadata::nickname_utf8) }};
        const std::array<size_t, 15> expected {{ 0, 4, 8, 12, 16, 24, 32, 36, 40, 44, 48, 52, 56, 217, 161 }};
        expect (offsets == expected);

        beginTest ("both resolved peer names initialize empty mappings and publish independently");
        const auto base = ringName();
        ObsMetadata first, second;
        expect (first.open (base + ".Peer", 1));
        expect (second.open (base + ".Peer2", 2));
        MetadataView one (base + ".Peer"), two (base + ".Peer2");
        expect (one.view != nullptr && two.view != nullptr);
        if (one.view == nullptr || two.view == nullptr) return;
        for (const auto* view : { one.view, two.view })
        {
            expectEquals (view->magic, uint32_t (LM_OBS_METADATA_MAGIC));
            expectEquals (view->version, uint32_t (1));
            expectEquals (view->mapping_bytes, uint32_t (256));
            expectEquals (lm_obs_load_acquire (&view->sequence), int64_t (2));
            expectEquals (view->writer_pid, uint32_t (GetCurrentProcessId()));
            expectEmpty (*view);
        }
        expectEquals (one.view->slot, uint32_t (1));
        expectEquals (two.view->slot, uint32_t (2));
        const auto korean = juce::String::fromUTF8 ("\xea\xb0\x80\xeb\x82\x98");
        first.publish (0x123456789abcdef0ULL, 2, 3, ParticipantKind::native, ConnectionState::connecting, korean);
        second.publish (77, 8, 9, ParticipantKind::web, ConnectionState::connected, "Web");
        expectEquals (lm_obs_load_acquire (&one.view->sequence), int64_t (4));
        expectEquals (one.view->session, uint64_t (0x123456789abcdef0ULL));
        expectEquals (one.view->participant_id, uint32_t (2));
        expectEquals (one.view->stream_epoch, uint32_t (3));
        expectEquals (one.view->kind, uint32_t (LM_OBS_PARTICIPANT_NATIVE));
        expectEquals (one.view->state, uint32_t (LM_OBS_PARTICIPANT_CONNECTING));
        expectName (*one.view, korean);
        expectEquals (two.view->participant_id, uint32_t (8));
        expectEquals (two.view->kind, uint32_t (LM_OBS_PARTICIPANT_WEB));
        expectEquals (two.view->state, uint32_t (LM_OBS_PARTICIPANT_CONNECTED));
        expectName (*two.view, "Web");

        beginTest ("updates and clear advance even sequences, erase the old name and leave the other slot intact");
        first.publish (77, 2, 4, ParticipantKind::native, ConnectionState::connected, "A");
        expectEquals (lm_obs_load_acquire (&one.view->sequence), int64_t (6));
        expectEquals (one.view->state, uint32_t (LM_OBS_PARTICIPANT_CONNECTED));
        expectName (*one.view, "A");
        first.clear();
        expectEquals (lm_obs_load_acquire (&one.view->sequence), int64_t (8));
        expectEmpty (*one.view);
        expectEquals (one.view->slot, uint32_t (1));
        expectEquals (one.view->writer_pid, uint32_t (GetCurrentProcessId()));
        expectName (*two.view, "Web");
        expectEquals (lm_obs_load_acquire (&two.view->sequence), int64_t (4));

        beginTest ("UTF-8 names stop at complete scalars within 160 bytes and always include a zero tail");
        const auto ascii = juce::String::repeatedString ("a", 160);
        const auto hangul = juce::String::fromUTF8 ("\xea\xb0\x80");
        const auto accent = juce::String::fromUTF8 ("\xc3\xa9");
        const auto emoji = juce::String::fromUTF8 ("\xf0\x9f\x8e\xa4");
        const std::array<std::pair<juce::String, juce::String>, 8> names {{
            { ascii, ascii }, { ascii + "z", ascii },
            { juce::String::repeatedString (hangul, 54), juce::String::repeatedString (hangul, 53) },
            { juce::String::repeatedString (emoji, 41), juce::String::repeatedString (emoji, 40) },
            { ascii.dropLastCharacters (1) + emoji, ascii.dropLastCharacters (1) },
            { ascii.dropLastCharacters (2) + accent, ascii.dropLastCharacters (2) + accent },
            { ascii.dropLastCharacters (1) + accent, ascii.dropLastCharacters (1) }, { {}, {} }
        }};
        for (const auto& nicknameCase : names)
        {
            const auto before = lm_obs_load_acquire (&one.view->sequence);
            first.publish (77, 2, 4, ParticipantKind::native, ConnectionState::connected, nicknameCase.first);
            expectEquals (lm_obs_load_acquire (&one.view->sequence), before + int64_t (2));
            expectName (*one.view, nicknameCase.second);
        }

        beginTest ("close and destruction publish empty slots to retained read-only views; reopen reinitializes");
        first.close();
        expectEmpty (*one.view);
        const auto closedSequence = lm_obs_load_acquire (&one.view->sequence);
        first.clear(); // safe before open/after close
        first.publish (1, 2, 3, ParticipantKind::native, ConnectionState::connected, "closed");
        expectEquals (lm_obs_load_acquire (&one.view->sequence), closedSequence);
        {
            ObsMetadata reopened;
            expect (reopened.open (base + ".Peer", 1));
            expectEquals (lm_obs_load_acquire (&one.view->sequence), closedSequence + int64_t (2));
            expectEmpty (*one.view);
            reopened.publish (88, 10, 1, ParticipantKind::web, ConnectionState::connected, "New");
            expectName (*one.view, "New");
        }
        expectEmpty (*one.view);
        expectEquals (lm_obs_load_acquire (&one.view->sequence), closedSequence + int64_t (6));
        expectName (*two.view, "Web");

        beginTest ("open requires a resolved peer name and a one-based slot");
        expect (! first.open ({}, 1));
        expect (! first.open (base, 0));
        expect (! first.open (base, 3));
    }

private:
    void expectEmpty (const lm_obs_metadata& view)
    {
        expect (view.session == 0 && view.participant_id == 0 && view.stream_epoch == 0);
        expect (view.state == LM_OBS_PARTICIPANT_EMPTY && view.kind == LM_OBS_PARTICIPANT_NONE);
        expect (view.nickname_bytes == 0 && allZero (view.nickname_utf8) && allZero (view.reserved));
    }

    void expectName (const lm_obs_metadata& view, const juce::String& expected)
    {
        expectEquals (view.nickname_bytes, uint32_t (expected.getNumBytesAsUTF8()));
        expectEquals (juce::String::fromUTF8 (view.nickname_utf8), expected);
        expect (std::all_of (view.nickname_utf8 + view.nickname_bytes, std::end (view.nickname_utf8),
                            [] (char byte) { return byte == 0; }));
        expect (allZero (view.reserved));
    }
};
static LiveMixObsMetadataTests liveMixObsMetadataTests;
}
