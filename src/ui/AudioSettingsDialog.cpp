#include "ui/AudioSettingsDialog.h"

#include "audio/AudioEngine.h"

#include "ui/UiUtils.h"

namespace gocue::AudioSettingsDialog
{

namespace
{
    juce::Component::SafePointer<juce::DialogWindow> openDialog;

    /** Hosts the JUCE selector and rebuilds it when the device type changes, so the output list offers up to 64
        channels on ASIO and only the first pair everywhere else (the engine enforces the same limit). */
    class SelectorHost : public juce::Component,
                         private juce::ChangeListener
    {
    public:
        explicit SelectorHost (juce::AudioDeviceManager& dm) : deviceManager (dm)
        {
            hint.setJustificationType (juce::Justification::centredLeft);
            hint.setColour (juce::Label::textColourId, Palette::dimText);
            hint.setFont (juce::Font (juce::FontOptions (12.0f)));
            addAndMakeVisible (hint);
            rebuild();
            deviceManager.addChangeListener (this);
            setSize (520, 480);
        }

        ~SelectorHost() override
        {
            deviceManager.removeChangeListener (this);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            hint.setBounds (area.removeFromBottom (24).reduced (8, 0));

            if (selector != nullptr)
                selector->setBounds (area);
        }

    private:
        static bool allowsMultichannel (juce::AudioDeviceManager& dm)
        {
            if (auto* type = dm.getCurrentDeviceTypeObject())
                return type->getTypeName().containsIgnoreCase ("ASIO");

            return false;
        }

        void rebuild()
        {
            const bool multichannel = allowsMultichannel (deviceManager);
            builtForMultichannel = multichannel;
            // On ASIO the output list offers stereo pairs up to 64 channels. Elsewhere the engine pins the outputs to
            // 1-2, so the list is hidden: JUCE only shows it while the minimum is below the device's channel count,
            // and a minimum equal to the maximum keeps the selector's own bookkeeping consistent.
            const int minOut = multichannel ? 2 : AudioEngine::maxDeviceOutputs;
            selector = std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager,
                                                                            0, AudioEngine::maxDeviceInputs,   // inputs for mic cues
                                                                            minOut, AudioEngine::maxDeviceOutputs,
                                                                            false,    // midi inputs
                                                                            false,    // midi output
                                                                            true,     // stereo pairs
                                                                            false);   // show advanced options directly
            addAndMakeVisible (*selector);
            hint.setText (multichannel ? ko ("ASIO: 출력 채널을 최대 64개까지 열 수 있습니다.")
                                       : ko ("다채널 출력은 ASIO에서만 됩니다. 이 모드에서는 출력 1-2만 사용합니다."),
                          juce::dontSendNotification);
            resized();
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            if (allowsMultichannel (deviceManager) != builtForMultichannel)
                rebuild();   // the type changed: the channel list gets the right limit
        }

        juce::AudioDeviceManager& deviceManager;
        std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;
        juce::Label hint;
        bool builtForMultichannel = false;
    };
}

void show (juce::AudioDeviceManager& deviceManager, juce::Component* centreAround)
{
    if (openDialog != nullptr)
    {
        openDialog->toFront (true);
        return;
    }

    auto* selector = new SelectorHost (deviceManager);

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = ko ("오디오 출력 설정 (ASIO / WASAPI)");
    options.content.setOwned (selector);
    options.componentToCentreAround = centreAround;
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    openDialog = options.launchAsync();
}

void closeIfOpen()
{
    if (openDialog != nullptr)
        delete openDialog.getComponent();
}

} // namespace gocue::AudioSettingsDialog
