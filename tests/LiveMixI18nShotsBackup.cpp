#include "LiveMixI18nShots.h"
#include "ui/BackupDialog.h"
#include "BackupServer.h"
#include "PluginPreset.h"
#include "ui/Widgets.h"

// BackupContent is file-local, and the production login callback is the only way to fill its list.
// Compile that exact implementation in a test-only namespace. No copied UI, networking or production hook.
// Include its headers first so the namespace macro changes only this .cpp, not any shared class definition.
namespace gocue::livemix_i18n_shots_backup
{
using namespace gocue::livemix;
namespace Palette = gocue::livemix::Palette;
// The release server configuration is private to LiveMixCore. Give this test-only copy an offline configuration
// so its real registration button is available; no login, submit, upload or download action is invoked.
namespace BackupServer
{
bool isConfigured() { return true; }
gocue::livemix::WebDavBackup::Target target (const juce::String& id, const juce::String& password)
{
    gocue::livemix::WebDavBackup::Target value;
    value.baseUrl = "https://example.invalid"; value.share = "/capture";
    value.accountId = id; value.accountPassword = password;
    return value;
}
}
namespace BackupDialog
{
using Callbacks = gocue::livemix::BackupDialog::Callbacks;
using gocue::livemix::BackupDialog::fileColumnText;
void show (MixDocument&, LiveMixSettings&, WebDavBackup&, juce::Component*, Callbacks);
void closeIfOpen();
}
}
#define livemix livemix_i18n_shots_backup
#include "../livemix/src/ui/BackupDialog.cpp"
#undef livemix

namespace gocue::tests::i18nshots
{
namespace
{
using Content = gocue::livemix_i18n_shots_backup::BackupContent;
struct Entries { using Type = void (Content::*) (std::vector<livemix::WebDavBackup::Entry>, bool); friend Type member (Entries); };
struct Status { using Type = void (Content::*) (const juce::String&, bool); friend Type member (Status); };
struct Account { using Type = juce::TextEditor Content::*; friend Type member (Account); };
struct Password { using Type = juce::TextEditor Content::*; friend Type member (Password); };
template<class Tag, typename Tag::Type value> struct Access { friend typename Tag::Type member (Tag) { return value; } };
template struct Access<Entries, &Content::showEntries>;
template struct Access<Status, &Content::setStatus>;
template struct Access<Account, &Content::idEditor>;
template struct Access<Password, &Content::passwordEditor>;
}

void captureBackup (Capture& shots, livemix::MixDocument& document, livemix::LiveMixSettings& settings)
{
    namespace implementation = gocue::livemix_i18n_shots_backup;
    livemix::WebDavBackup backup;
    implementation::BackupDialog::show (document, settings, backup, nullptr, {});
    auto* window = implementation::openWindow.getComponent();
    shots.test.expect (window != nullptr);
    if (window == nullptr) return;
    auto* content = dynamic_cast<Content*> (window->getContentComponent());
    shots.test.expect (content != nullptr);
    if (content != nullptr)
    {
        shots.window (*window, "backup_signed_out");
        auto* create = dynamic_cast<juce::TextButton*> (findComponent (*content, [] (auto& c)
        {
            auto* button = dynamic_cast<juce::TextButton*> (&c);
            return button != nullptr && button->getButtonText() == livemix::tr ("계정 만들기");
        }));
        shots.test.expect (create != nullptr);
        if (create != nullptr) create->onClick();
        dispatch();
        if (auto* registration = dialogNamed (livemix::tr ("계정 만들기")))
        {
            shots.window (*registration, "backup_create_account");
            registration->exitModalState (0);
            dispatch();
        }
        else shots.test.expect (false, "Account registration window did not open");

        std::vector<livemix::WebDavBackup::Entry> entries;
        for (int i = 0; i < 6; ++i)
        {
            livemix::WebDavBackup::Entry entry;
            entry.owner = "demo"; entry.pc = "Demo-PC";
            entry.name = (i == 0 ? livemix::defaultSessionName() : "Voice " + juce::String (i))
                + (i % 2 == 0 ? ".livemix" : ".livemixpreset");
            entry.path = "/demo/" + entry.name;
            entry.modified = juce::Time (2026, 10, 11, 14, 30);
            entry.size = 12800 + i * 500; entry.isPreset = i % 2 != 0;
            entries.push_back (entry);
        }
        (content->*member (Account {})).setText ("demo", false);
        (content->*member (Password {})).setText ("demo-password", false);
        (content->*member (Entries {})) (entries, false);
        (content->*member (Status {})) (livemix::trf ("백업 {0}개", (int) entries.size()), false);
        if (auto* table = dynamic_cast<juce::TableListBox*> (findComponent (*content, [] (auto& c) { return dynamic_cast<juce::TableListBox*> (&c) != nullptr; })))
            table->selectRow (0);
        shots.window (*window, "backup_list");
        (content->*member (Entries {})) (entries, true);
        shots.window (*window, "backup_list_all_accounts");
    }
    implementation::BackupDialog::closeIfOpen();
    dispatch();
}
}
