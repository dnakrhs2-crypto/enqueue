#include "ui/AudioSettingsDialog.h"

#include "ui/UiUtils.h"

namespace gocue::AudioSettingsDialog
{

namespace
{
    juce::Component::SafePointer<juce::DialogWindow> openDialog;
}

void show (juce::AudioDeviceManager& deviceManager, juce::Component* centreAround)
{
    if (openDialog != nullptr)
    {
        openDialog->toFront (true);
        return;
    }

    auto* selector = new juce::AudioDeviceSelectorComponent (deviceManager,
                                                             0, 0,     // no inputs
                                                             2, 2,     // exactly one stereo output pair
                                                             false,    // midi inputs
                                                             false,    // midi output
                                                             true,     // stereo pairs
                                                             false);   // show advanced options directly
    selector->setSize (520, 440);

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
