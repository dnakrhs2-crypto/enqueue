#pragma once
#include "PluginScan.h"

namespace gocue::livemix
{
/** JUCE 8.0.15's progress dialog sets a private stop flag, but doesn't signal the current
    ThreadPoolJob until that file returns. Forward modal dismissal to our interruptible scanner. */
class PluginScanListComponent final : public juce::PluginListComponent, private juce::Timer
{
public:
    PluginScanListComponent (juce::AudioPluginFormatManager& formats, juce::KnownPluginList& list,
                             const juce::File& crashMarker, PluginScanCoordinator& c)
        : PluginListComponent (formats, list, crashMarker, nullptr, false), coordinator (c)
    {
        setScanDialogText (progressTitle, juce::String::fromUTF8 ("플러그인을 찾는 중입니다."));
        setNumberOfThreadsForScanning (1);
    }
    ~PluginScanListComponent() override
    {
        stopTimer();
        coordinator.cancel(); // before the base destructor joins the scanning job
    }
    void scanFor (juce::AudioPluginFormat& format, const juce::StringArray& files = {})
    {
        ++generation;
        watched = nullptr;
        PluginListComponent::scanFor (format, files);
        watchProgress();
        startTimer (20);
    }

private:
    void timerCallback() override
    {
        if (! isScanning()) { stopTimer(); return; }
        watchProgress();
    }
    void watchProgress()
    {
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
        {
            auto* component = desktop.getComponent (i);
            if (component == watched || component->getName() != progressTitle || ! component->isCurrentlyModal()) continue;
            watched = component;
            juce::Component::SafePointer<PluginScanListComponent> safe (this);
            juce::ModalComponentManager::getInstance()->attachCallback (component,
                juce::ModalCallbackFunction::create ([safe, scan = generation] (int)
                {
                    if (safe != nullptr && safe->generation == scan && safe->isScanning()) safe->coordinator.cancel();
                }));
        }
    }
    const juce::String progressTitle = juce::String::fromUTF8 ("LiveMix 플러그인 스캔");
    PluginScanCoordinator& coordinator;
    juce::Component::SafePointer<juce::Component> watched;
    unsigned generation = 0;
};
} // namespace gocue::livemix
