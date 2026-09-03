#include "WebDavBackup.h"

#include <juce_core/juce_core.h>

namespace gocue::tests
{

using namespace gocue::livemix;

class WebDavBackupTests : public juce::UnitTest
{
public:
    WebDavBackupTests() : juce::UnitTest ("LiveMix backup path", "LiveMix") {}

    void runTest() override
    {
        beginTest ("the remote path is folder/PC/creator_date.livemix with file-safe names");
        {
            const juce::Time when (2026, 8, 4, 1, 2, 3, 0, true);   // 2026-09-04 01:02:03
            const auto path = WebDavBackup::remotePathFor (juce::String::fromUTF8 ("/LiveMix 백업/"), "STUDIO-PC", juce::String::fromUTF8 ("곰: 테스트?"), when);
            expectEquals (path, juce::String::fromUTF8 ("/LiveMix 백업/STUDIO-PC/곰_ 테스트__2026-09-04_010203.livemix"));

            const auto noSlash = WebDavBackup::remotePathFor ("backups", "pc", "a", when);
            expect (noSlash.startsWith ("/backups/pc/a_"));
            expectEquals (WebDavBackup::sanitiseName ("  "), juce::String ("session"));
            expectEquals (WebDavBackup::sanitiseName ("a/b\\c|d"), juce::String ("a_b_c_d"));
        }
    }
};

static WebDavBackupTests webDavBackupTests;

} // namespace gocue::tests
