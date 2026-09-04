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
        beginTest ("LiveMix accounts: ids, password hashes, the account file, the paths on the share");
        {
            expect (WebDavBackup::validateAccountId ("alice").isEmpty());
            expect (WebDavBackup::validateAccountId (juce::String::fromUTF8 (" 가을_01 ")).isEmpty());
            expect (WebDavBackup::validateAccountId ("a").isNotEmpty());                      // too short
            expect (WebDavBackup::validateAccountId ("abcdefghijklmnopqrstu").isNotEmpty());  // too long
            expect (WebDavBackup::validateAccountId ("a b").isNotEmpty());                    // a space
            expect (WebDavBackup::validateAccountId ("a/b").isNotEmpty());                    // a path character
            expect (WebDavBackup::validateAccountId ("accounts").isNotEmpty());               // the accounts folder's name
            expect (WebDavBackup::validateAccountId ("-x").isNotEmpty());

            const auto salt = WebDavBackup::newSalt();
            expect (salt.length() >= 32 && salt != WebDavBackup::newSalt());
            const auto hash = WebDavBackup::hashPassword ("secret", salt);
            expectEquals (hash.length(), 64);
            expect (hash.containsOnly ("0123456789abcdef"));
            expectEquals (hash, WebDavBackup::hashPassword ("secret", salt));       // the same again
            expect (hash != WebDavBackup::hashPassword ("Secret", salt));          // the password matters
            expect (hash != WebDavBackup::hashPassword ("secret", salt + "x"));    // the salt matters

            const auto json = WebDavBackup::accountJson ("alice", salt, hash, "STUDIO-PC", juce::Time (2026, 8, 4, 15, 30, 0, 0, true));
            juce::String salt2, hash2;
            expect (WebDavBackup::parseAccount (json, salt2, hash2));
            expectEquals (salt2, salt);
            expectEquals (hash2, hash);
            expect (! WebDavBackup::parseAccount ("{\"id\": \"alice\"}", salt2, hash2));   // no hash
            expect (! WebDavBackup::parseAccount ("not json", salt2, hash2));
            expect (! WebDavBackup::parseAccount ("[]", salt2, hash2));

            expectEquals (WebDavBackup::parseAdminList ("alice\n# a comment\n\n  bob  \n").joinIntoString (","), juce::String ("alice,bob"));
            expect (WebDavBackup::parseAdminList ("").isEmpty());

            expectEquals (WebDavBackup::accountsFolder ("/backups/"), juce::String ("/backups/accounts"));
            expectEquals (WebDavBackup::accountPath ("backups", " alice "), juce::String ("/backups/accounts/alice.json"));
            expectEquals (WebDavBackup::adminListPath ("/backups"), juce::String ("/backups/accounts/admins.txt"));
            expectEquals (WebDavBackup::accountFolder ("/backups", "alice"), juce::String ("/backups/alice"));
            expectEquals (WebDavBackup::backupPathFor ("/backups", "alice", "STUDIO:PC", juce::Time (2026, 8, 4, 15, 30, 0, 0, true)),
                          juce::String ("/backups/alice/STUDIO_PC_2026-09-04_153000.livemix"));

            // a backup path is exactly <share>/<owner>/<file>.livemix - nothing a server could resolve elsewhere
            juce::String owner;
            expect (WebDavBackup::parseBackupPath ("/backups", "/backups/alice/STUDIO_PC_2026-09-04_153000.livemix", owner) && owner == "alice");
            expect (! WebDavBackup::parseBackupPath ("/backups", "/backups/alice/../bob/x.livemix", owner));     // a dot segment
            expect (! WebDavBackup::parseBackupPath ("/backups", "/backups/alice//x.livemix", owner));           // an empty segment
            expect (! WebDavBackup::parseBackupPath ("/backups", "/backups/alice/sub/x.livemix", owner));        // too deep
            expect (! WebDavBackup::parseBackupPath ("/backups", "/backups/alice/x.txt", owner));                // not a session
            expect (! WebDavBackup::parseBackupPath ("/backups", "/backups/accounts/x.livemix", owner));         // the accounts folder
            expect (! WebDavBackup::parseBackupPath ("/backups", "/other/alice/x.livemix", owner));              // another share
            expect (! WebDavBackup::parseBackupPath ("/backups", "/backupsX/alice/x.livemix", owner));           // a share that is a prefix
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
            expect (WebDavBackup::validateBaseUrl ("https://[::ffff:192.168.001.1]:5006").isNotEmpty());   // a leading zero is not an octet

            expectEquals (WebDavBackup::credentialKeyFor ("https://Parkdoomin.synology.me:5006/", " gom "), juce::String ("LiveMix/WebDAV/parkdoomin.synology.me:5006/gom"));
            expect (WebDavBackup::credentialKeyFor ("https://a.example:5006", "gom") != WebDavBackup::credentialKeyFor ("https://b.example:5006", "gom"));
        }

        beginTest ("the WebDAV date form parses to UTC");
        {
            const auto t = WebDavBackup::parseHttpDate ("Fri, 17 Jul 2026 02:32:05 GMT");
            expectEquals (t.toISO8601 (true), juce::Time (2026, 6, 17, 2, 32, 5, 0, false).toISO8601 (true));
            expectEquals (WebDavBackup::parseHttpDate ("17 Jul 2026 02:32:05").toISO8601 (true), t.toISO8601 (true));   // without the weekday / zone
            expect (WebDavBackup::parseHttpDate ("2026-07-17T02:32:05Z") == juce::Time());   // not that form
            expect (WebDavBackup::parseHttpDate ("") == juce::Time());
            expect (WebDavBackup::parseHttpDate ("Fri, 40 Jul 2026 02:32:05 GMT") == juce::Time());
        }

        beginTest ("a multistatus answer lists folders and files under any namespace prefix, the asked folder itself left out");
        {
            // Synology (Apache mod_dav): D: on the frame, lp1: on the properties, percent-encoded Korean, the folder itself first
            const juce::String synology = juce::String::fromUTF8 (
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<D:multistatus xmlns:D=\"DAV:\">\n"
                "<D:response xmlns:lp2=\"http://apache.org/dav/props/\" xmlns:lp1=\"DAV:\">\n"
                "<D:href>/home/LiveMix%20%EB%B0%B1%EC%97%85/</D:href>\n"
                "<D:propstat><D:prop><lp1:resourcetype><D:collection/></lp1:resourcetype>"
                "<lp1:getlastmodified>Fri, 17 Jul 2026 02:32:05 GMT</lp1:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n"
                "</D:response>\n"
                "<D:response xmlns:lp1=\"DAV:\"><D:href>/home/LiveMix%20%EB%B0%B1%EC%97%85/STUDIO-PC/</D:href>\n"
                "<D:propstat><D:prop><lp1:resourcetype><D:collection/></lp1:resourcetype>"
                "<lp1:getlastmodified>Thu, 04 Sep 2026 06:31:18 GMT</lp1:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n"
                "</D:response>\n"
                "<D:response xmlns:lp1=\"DAV:\"><D:href>https://nas.example:5006/home/LiveMix%20%EB%B0%B1%EC%97%85/STUDIO-PC/%EA%B0%80%EC%9D%84_2026-09-04_153000.livemix</D:href>\n"
                "<D:propstat><D:prop><lp1:resourcetype/><lp1:getcontentlength>2468</lp1:getcontentlength>"
                "<lp1:getlastmodified>Thu, 04 Sep 2026 06:31:18 GMT</lp1:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\n"
                "</D:response>\n"
                "</D:multistatus>\n");

            const auto items = WebDavBackup::parseMultistatus (synology, "/home/LiveMix%20%EB%B0%B1%EC%97%85/");
            expectEquals ((int) items.size(), 2);

            if (items.size() == 2)
            {
                expectEquals (items[0].path, juce::String::fromUTF8 ("/home/LiveMix 백업/STUDIO-PC"));
                expect (items[0].collection);
                expectEquals (items[0].name(), juce::String ("STUDIO-PC"));
                expectEquals (items[1].path, juce::String::fromUTF8 ("/home/LiveMix 백업/STUDIO-PC/가을_2026-09-04_153000.livemix"));   // an absolute href: the path only
                expect (! items[1].collection);
                expectEquals (items[1].name(), juce::String::fromUTF8 ("가을_2026-09-04_153000.livemix"));
                expectEquals (items[1].size, (juce::int64) 2468);
                expectEquals (items[1].modified.toISO8601 (true), juce::Time (2026, 8, 4, 6, 31, 18, 0, false).toISO8601 (true));
            }

            // no prefix at all (a default namespace), the asked path given without the trailing slash
            const juce::String plain = "<multistatus xmlns=\"DAV:\"><response><href>/homes/</href><propstat><prop><resourcetype><collection/></resourcetype></prop></propstat></response>"
                                       "<response><href>/homes/fkvmfls/</href><propstat><prop><resourcetype><collection/></resourcetype></prop></propstat></response>"
                                       "<response><href>/homes/note.lnk</href><propstat><prop><resourcetype/><getcontentlength>12</getcontentlength></prop></propstat></response></multistatus>";
            const auto homes = WebDavBackup::parseMultistatus (plain, "/homes");
            expectEquals ((int) homes.size(), 2);

            if (homes.size() == 2)
            {
                expect (homes[0].collection && homes[0].name() == "fkvmfls");
                expect (! homes[1].collection && homes[1].name() == "note.lnk" && homes[1].size == 12);
            }

            bool parsed = true;
            expect (WebDavBackup::parseMultistatus ("<html>not dav</html>", "/x", &parsed).empty());
            expect (! parsed);   // an unreadable answer is not an empty folder
            parsed = true;
            expect (WebDavBackup::parseMultistatus ("", "/x", &parsed).empty());
            expect (! parsed);
            parsed = false;
            expect (WebDavBackup::parseMultistatus ("<D:multistatus xmlns:D=\"DAV:\"></D:multistatus>", "/x", &parsed).empty());
            expect (parsed);   // an empty folder reads fine

            // a literal '+' in a name is a '+' (JUCE's decoder would make it a space), a real space comes encoded
            const auto plus = WebDavBackup::parseMultistatus ("<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>/home/b/take+1%20final.livemix</D:href>"
                                                             "<D:propstat><D:prop><D:resourcetype/></D:prop></D:propstat></D:response></D:multistatus>", "/home/b");
            expectEquals ((int) plus.size(), 1);

            if (plus.size() == 1)
                expectEquals (plus[0].name(), juce::String ("take+1 final.livemix"));
            expect (WebDavBackup::credentialKeyFor ("https://a.example:5006", "gom") != WebDavBackup::credentialKeyFor ("https://a.example:5006", "lanna"));
        }
    }
};

static WebDavBackupTests webDavBackupTests;

} // namespace gocue::tests
