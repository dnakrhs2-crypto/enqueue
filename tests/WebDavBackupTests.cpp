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

        beginTest ("the backup address must be https://server[:port]; passwords are keyed by server and user");
        {
            expect (WebDavBackup::validateBaseUrl ("https://parkdoomin.synology.me:5006").isEmpty());
            expect (WebDavBackup::validateBaseUrl (" https://nas.local:5006/ ").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("http://parkdoomin.synology.me:5005").isNotEmpty());   // no TLS: the password would travel in the clear
            expect (WebDavBackup::validateBaseUrl ("parkdoomin.synology.me:5006").isNotEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://").isNotEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://nas.local:5006/LiveMix").isNotEmpty());   // the folder goes in its own field
            expect (WebDavBackup::validateBaseUrl ("").isNotEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://nas:").isNotEmpty());              // an empty port
            expect (WebDavBackup::validateBaseUrl ("https://nas:abc").isNotEmpty());           // not a number
            expect (WebDavBackup::validateBaseUrl ("https://nas:70000").isNotEmpty());         // out of range
            expect (WebDavBackup::validateBaseUrl ("https://nas:5006:1").isNotEmpty());        // two colons
            expect (WebDavBackup::validateBaseUrl ("https://na s:5006").isNotEmpty());         // whitespace inside
            expect (WebDavBackup::validateBaseUrl ("https://nas\\x:5006").isNotEmpty());      // a backslash
            expect (WebDavBackup::validateBaseUrl ("https://192.168.0.10:5006").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[fe80::1]:5006").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[fe80::1").isNotEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[::1]:5006").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[2001:db8::ff00:42:8329]").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[1:2:3:4:5:6:7:8]:5006").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[::ffff:192.168.0.1]:5006").isEmpty());
            expect (WebDavBackup::validateBaseUrl ("https://[::::]:5006").isNotEmpty());          // not an address
            expect (WebDavBackup::validateBaseUrl ("https://[1.2.3.4]:5006").isNotEmpty());        // IPv4 does not go in brackets
            expect (WebDavBackup::validateBaseUrl ("https://[1:2:3:4:5:6:7:8:9]").isNotEmpty());   // too many groups
            expect (WebDavBackup::validateBaseUrl ("https://[12345::1]").isNotEmpty());            // a group too long
            expect (WebDavBackup::validateBaseUrl ("https://[g::1]").isNotEmpty());                // not hex
            expect (WebDavBackup::validateBaseUrl ("https://[1::2::3]").isNotEmpty());             // two compressions

            expectEquals (WebDavBackup::credentialKeyFor ("https://Parkdoomin.synology.me:5006/", " gom "), juce::String ("LiveMix/WebDAV/parkdoomin.synology.me:5006/gom"));
            expect (WebDavBackup::credentialKeyFor ("https://a.example:5006", "gom") != WebDavBackup::credentialKeyFor ("https://b.example:5006", "gom"));
            expect (WebDavBackup::credentialKeyFor ("https://a.example:5006", "gom") != WebDavBackup::credentialKeyFor ("https://a.example:5006", "lanna"));
        }
    }
};

static WebDavBackupTests webDavBackupTests;

} // namespace gocue::tests
