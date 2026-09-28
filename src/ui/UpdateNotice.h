#pragma once

#include "app/CoupangShortcut.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::UpdateNotice
{
inline std::unique_ptr<juce::AlertWindow> create (juce::LookAndFeel& lookAndFeel,
                                                juce::String message, bool offerShortcut)
{
    if (offerShortcut)
        message += "\n\n" + CoupangShortcut::guidance + "\n" + CoupangShortcut::disclosure;
    // Preserve the application's style/router and keep both Enter and Esc on OK.
    std::unique_ptr<juce::AlertWindow> alert (lookAndFeel.createAlertWindow (
        juce::String::fromUTF8 ("업데이트 완료"), message, juce::String::fromUTF8 ("확인"), {}, {},
        juce::MessageBoxIconType::InfoIcon, 1, nullptr));
    alert->setAlwaysOnTop (juce::WindowUtils::areThereAnyAlwaysOnTopWindows());   // as AlertWindow::showAsync does
    if (offerShortcut)
    {
        alert->addButton (CoupangShortcut::buttonText, 2);
        alert->getButton (1)->setWantsKeyboardFocus (false); // no Tab/Space activation either
    }
    return alert;
}

inline void show (juce::LookAndFeel& lookAndFeel, const juce::String& message, bool offerShortcut)
{
    if (! offerShortcut)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            juce::String::fromUTF8 ("업데이트 완료"), message, juce::String::fromUTF8 ("확인"));
        return;
    }
    auto alert = create (lookAndFeel, message, true);
    alert.release()->enterModalState (true, juce::ModalCallbackFunction::create ([] (int result)
    {
        if (result != 2)
            return;
        const auto created = CoupangShortcut::createOn (CoupangShortcut::userDesktop(), CoupangShortcut::installedIcon());
        if (created.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                juce::String::fromUTF8 ("쿠팡 바로가기 추가"),
                juce::String::fromUTF8 ("바탕화면에 쿠팡 바로가기를 만들지 못했습니다.\n") + created.getErrorMessage(),
                juce::String::fromUTF8 ("확인"));
    }), true);
}
}
