#pragma once

#include "LiveMixSettings.h"
#include "MixDocument.h"
#include "WebDavBackup.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace gocue::livemix
{

/** 온라인 백업: the window with the account's name and password, the list of backups on the server - sessions and
    plugin presets (an admin sees everyone's), "지금 세션 백업", "플러그인 프리셋 백업" and "불러오기". '계정 만들기' opens
    its own small window. Not modal: the mic buttons stay usable while a list or a transfer runs. One window at a time. */
namespace BackupDialog
{
    struct Callbacks
    {
        std::function<void (const juce::String& message, bool error)> status;   // the main window's status line
        std::function<bool()> saveBeforeUpload;                                  // the open session onto its file; false = nothing to upload
        std::function<void (const juce::File& file)> restore;                    // a downloaded backup: open it as the session
        std::function<void()> presetRestored;                                    // a preset came down into the presets folder
    };

    void show (MixDocument& document, LiveMixSettings& settings, WebDavBackup& backup, juce::Component* centreAround, Callbacks callbacks);
    void closeIfOpen();

    /** A backup's file name as the list's file column shows it: without LiveMix's own extension - the kind column
        already says session / preset, and a common 'name_PC_date_time.livemix' was cut (10/30). A dot inside a name
        stays. */
    inline juce::String fileColumnText (const juce::String& fileName)
    {
        for (const auto* extension : { ".livemixpreset", ".livemix" })
            if (fileName.endsWithIgnoreCase (extension))
                return fileName.dropLastCharacters (juce::String (extension).length());
        return fileName;
    }
}

} // namespace gocue::livemix
