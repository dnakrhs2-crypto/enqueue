#include "ui/PluginDialogs.h"

#include "ui/PluginChainComponent.h"
#include "ui/UiUtils.h"

namespace gocue::PluginDialogs
{

namespace
{
    juce::Component::SafePointer<juce::DialogWindow> masterDialog;

    class MasterInsertsContent : public juce::Component
    {
    public:
        MasterInsertsContent (AudioEngine& engine, PluginWindowManager& windows, std::function<void()> onOpenPluginManager,
                              PerformEdit performEdit)
            : strip (engine, windows)
        {
            title.setText (ko ("마스터 버스 인서트 - 모든 큐가 믹스된 뒤 마지막으로 통과합니다"), juce::dontSendNotification);
            title.setColour (juce::Label::textColourId, Palette::dimText);
            title.setFont (Palette::font (Palette::bodySize));
            addAndMakeVisible (title);

            strip.onOpenPluginManager = std::move (onOpenPluginManager);
            strip.performEdit = std::move (performEdit);
            strip.setChain (&engine.getMasterChain(), ko ("마스터"));
            addAndMakeVisible (strip);

            setSize (760, 140);
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
            Palette::drawDialog (g, getLocalBounds());
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
        options.dialogBackgroundColour = Palette::background;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = resizable;
        return options.launchAsync();
    }
}

void showMasterInserts (AudioEngine& engine, PluginWindowManager& windows,
                        std::function<void()> onOpenPluginManager, PerformEdit performEdit,
                        juce::Component* centreAround)
{
    if (masterDialog != nullptr)
    {
        masterDialog->toFront (true);
        return;
    }

    auto* content = new MasterInsertsContent (engine, windows, std::move (onOpenPluginManager), std::move (performEdit));
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
}

} // namespace gocue::PluginDialogs
