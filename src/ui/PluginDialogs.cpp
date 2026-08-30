#include "ui/PluginDialogs.h"

#include "ui/PluginChainComponent.h"
#include "ui/UiUtils.h"

namespace gocue::PluginDialogs
{

namespace
{
    juce::Component::SafePointer<juce::DialogWindow> managerDialog;
    juce::Component::SafePointer<juce::DialogWindow> masterDialog;

    class MasterInsertsContent : public juce::Component
    {
    public:
        MasterInsertsContent (AudioEngine& engine, PluginWindowManager& windows, std::function<void()> onOpenPluginManager)
            : strip (engine, windows)
        {
            title.setText (ko ("마스터 버스 인서트 - 모든 큐가 믹스된 뒤 마지막으로 통과합니다"), juce::dontSendNotification);
            title.setColour (juce::Label::textColourId, Palette::dimText);
            title.setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (title);

            strip.onOpenPluginManager = std::move (onOpenPluginManager);
            strip.setChain (&engine.getMasterChain(), ko ("마스터"));
            addAndMakeVisible (strip);

            setSize (760, 120);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12, 10);
            title.setBounds (area.removeFromTop (20));
            area.removeFromTop (8);
            strip.setBounds (area);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Palette::panel);
        }

        void chainChanged (PluginChain* chain)
        {
            strip.chainChanged (chain);
        }

    private:
        juce::Label title;
        PluginChainComponent strip;
    };

    juce::Component::SafePointer<MasterInsertsContent> masterContent;

    juce::DialogWindow* launch (juce::Component* content, const juce::String& title, juce::Component* centreAround, bool resizable)
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = title;
        options.content.setOwned (content);
        options.componentToCentreAround = centreAround;
        options.dialogBackgroundColour = Palette::panel;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = resizable;
        return options.launchAsync();
    }
}

void showPluginManager (AudioEngine& engine, AppSettings& settings, juce::Component* centreAround)
{
    if (managerDialog != nullptr)
    {
        managerDialog->toFront (true);
        return;
    }

    auto& host = engine.getPluginHost();
    auto* list = new juce::PluginListComponent (host.getFormatManager(), host.getKnownPlugins(),
                                                settings.getDeadMansPedalFile(), settings.getPropertiesFile(), false);
    list->setSize (780, 480);

    managerDialog = launch (list, ko ("VST3 플러그인 관리 - [Options]에서 스캔"), centreAround, true);
}

void showMasterInserts (AudioEngine& engine, PluginWindowManager& windows,
                        std::function<void()> onOpenPluginManager, juce::Component* centreAround)
{
    if (masterDialog != nullptr)
    {
        masterDialog->toFront (true);
        return;
    }

    auto* content = new MasterInsertsContent (engine, windows, std::move (onOpenPluginManager));
    masterContent = content;
    masterDialog = launch (content, ko ("마스터 버스 인서트"), centreAround, false);
}

void chainChanged (PluginChain* chain)
{
    if (masterContent != nullptr)
        masterContent->chainChanged (chain);
}

void closeAll()
{
    if (masterDialog != nullptr)
        delete masterDialog.getComponent();

    if (managerDialog != nullptr)
        delete managerDialog.getComponent();
}

} // namespace gocue::PluginDialogs
