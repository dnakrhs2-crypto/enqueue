#include "MainComponentTestAccess.h"
#include "ui/WorkspaceSettingsDialog.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/UiUtils.h"
#include "app/Commands.h"

namespace gocue::tests
{
namespace
{
template <typename T, typename Pred>
T* findWhere (juce::Component& root, Pred&& pred)
{
    if (auto* self = dynamic_cast<T*> (&root); self != nullptr && pred (*self)) return self;
    for (auto* child : root.getChildren())
        if (auto* found = findWhere<T> (*child, pred)) return found;
    return nullptr;
}

juce::ComboBox* standbyBox (juce::Component& root)
{
    return findWhere<juce::ComboBox> (root, [] (juce::ComboBox& box)
    {
        return box.getNumItems() == 2 && box.getItemText (0) == ko ("다음 큐 대기") && box.getItemText (1) == ko ("지정 큐 대기");
    });
}
}

/** 프로젝트 설정 > 일반 > 큐 대기 상태 (gom 11/14: under '키를 뗀 뒤에만 다시 GO'): where it sits, that the tab still
    fits the window, and that the choice is the project's goKeepsStandby both ways. */
class StandbySettingRowTests : public juce::UnitTest
{
public:
    StandbySettingRowTests() : UnitTest ("Standby setting row", "Enqueue") {}

    void runTest() override
    {
        GoCueLookAndFeel theme;
        juce::LookAndFeel::setDefaultLookAndFeel (&theme);
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("EnqueueStandbyRow", ".settings");
        {
            juce::PropertiesFile::Options options;
            options.millisecondsBeforeSaving = -1;
            juce::PropertiesFile storage (file, options);
            AppSettings settings (storage);
            AudioEngine engine { 0 };
            juce::ApplicationCommandManager commands;
            auto main = std::make_unique<MainComponent> (engine, settings, commands);
            auto& document = ReopenLastProjectTestAccess::document (*main);

            auto open = [&]() -> juce::Component*
            {
                main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::workspaceSettings));
                auto* dialog = dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent());
                if (dialog == nullptr) return nullptr;
                if (auto* tabs = findWhere<juce::TabbedComponent> (*dialog, [] (juce::TabbedComponent&) { return true; }))
                    tabs->setCurrentTabIndex (0);
                return dialog->getContentComponent();
            };

            beginTest ("the row sits under 키를 뗀 뒤에만 다시 GO and the 일반 tab still fits the window");
            auto* content = open();
            expect (content != nullptr, "project settings opened");
            auto* box = content != nullptr ? standbyBox (*content) : nullptr;
            if (content != nullptr)
            {
                auto* label = findWhere<juce::Label> (*content, [] (juce::Label& l) { return l.getText() == ko ("큐 대기 상태"); });
                auto* keyUp = findWhere<juce::ToggleButton> (*content, [] (juce::ToggleButton& b) { return b.getButtonText() == ko ("키를 뗀 뒤에만 다시 GO"); });
                auto* last = findWhere<juce::Label> (*content, [] (juce::Label& l) { return l.getText().startsWith (ko ("글씨·화면 크기(이 PC의 배율)")); });
                expect (box != nullptr && label != nullptr && keyUp != nullptr && last != nullptr, "the row, its label, the toggle above it and the tab's last row");
                if (box != nullptr && label != nullptr && keyUp != nullptr && last != nullptr)
                {
                    auto in = [content] (juce::Component& c) { return content->getLocalArea (&c, c.getLocalBounds()); };
                    expect (box->isShowing() && label->isShowing());
                    expectEquals (in (*label).getY(), in (*keyUp).getBottom() + 6, "directly under the toggle, the form's row gap");
                    expectEquals (in (*box).getY(), in (*label).getY());
                    expect (in (*box).getX() >= in (*label).getRight());
                    expectGreaterOrEqual (content->getHeight() - in (*last).getBottom(), 24, "the last row stays inside the window with room below");
                    expectEquals (box->getSelectedId(), 1, "a project starts on 다음 큐 대기");
                    expect (! document.settings.goKeepsStandby);
                }
            }

            beginTest ("choosing 지정 큐 대기 sets the project at once, and back");
            if (box != nullptr)
            {
                box->setSelectedId (2, juce::sendNotificationSync);
                expect (document.settings.goKeepsStandby, "지정 큐 대기");
                expect (document.isDirty(), "the project needs saving");
                box->setSelectedId (1, juce::sendNotificationSync);
                expect (! document.settings.goKeepsStandby, "다음 큐 대기");
            }

            beginTest ("a project on 지정 큐 대기 opens the row on it");
            WorkspaceSettingsDialog::closeIfOpen();
            auto w = document.settings;
            w.goKeepsStandby = true;
            document.setSettings (w);
            content = open();
            box = content != nullptr ? standbyBox (*content) : nullptr;
            expect (box != nullptr && box->getSelectedId() == 2);

            WorkspaceSettingsDialog::closeIfOpen();
            main.reset();
        }
        file.deleteFile();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
};

static StandbySettingRowTests standbySettingRowTests;
}
