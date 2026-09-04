#pragma once

#include "LiveMixSettings.h"
#include "MixDocument.h"
#include "WebDavBackup.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::livemix
{

/** 온라인 백업: the window with the account's name and password, the list of backups on the server (an account that
    may read every home sees everyone's), "지금 세션 백업" and "불러오기". Not modal: the mic buttons stay usable while
    a list or a transfer runs. One window at a time. */
namespace BackupDialog
{
    struct Callbacks
    {
        std::function<void (const juce::String& message, bool error)> status;   // the main window's status line
        std::function<bool()> saveBeforeUpload;                                  // the open session onto its file; false = nothing to upload
        std::function<void (const juce::File& file)> restore;                    // a downloaded backup: open it as the session
    };

    void show (MixDocument& document, LiveMixSettings& settings, WebDavBackup& backup, juce::Component* centreAround, Callbacks callbacks);
    void closeIfOpen();
}

} // namespace gocue::livemix
