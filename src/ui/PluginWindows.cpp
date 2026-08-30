#include "ui/PluginWindows.h"

#include "ui/UiUtils.h"

namespace gocue
{

PluginEditorWindow::PluginEditorWindow (juce::AudioPluginInstance& p, const juce::String& title,
                                        std::function<void (PluginEditorWindow&)> closeCallback)
    : DocumentWindow (title, Palette::panel, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      plugin (p),
      onClose (std::move (closeCallback))
{
    setUsingNativeTitleBar (true);

    juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (plugin);

    const bool canResize = editor->isResizable();
    setContentOwned (editor, true);
    setResizable (canResize, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    toFront (true);
}

PluginEditorWindow::~PluginEditorWindow()
{
    clearContentComponent();   // deletes the editor, which detaches itself from the processor
}

void PluginEditorWindow::closeButtonPressed()
{
    if (onClose)
        onClose (*this);
}

//==============================================================================
PluginWindowManager::~PluginWindowManager()
{
    closeAll();
}

void PluginWindowManager::open (juce::AudioPluginInstance& plugin, const juce::String& title)
{
    for (auto& w : windows)
    {
        if (&w->getPlugin() == &plugin)
        {
            w->setName (title);
            w->toFront (true);
            return;
        }
    }

    windows.push_back (std::make_unique<PluginEditorWindow> (plugin, title,
                                                             [this] (PluginEditorWindow& w) { closeWindow (w); }));
}

void PluginWindowManager::closeFor (juce::AudioPluginInstance* plugin)
{
    for (auto it = windows.begin(); it != windows.end();)
    {
        if (&(*it)->getPlugin() == plugin)
            it = windows.erase (it);
        else
            ++it;
    }
}

void PluginWindowManager::closeAll()
{
    windows.clear();
}

void PluginWindowManager::pluginAboutToBeRemoved (PluginChain&, juce::AudioPluginInstance& plugin)
{
    closeFor (&plugin);
}

void PluginWindowManager::chainChanged (PluginChain& chain)
{
    if (onChainChanged)
        onChainChanged (chain);
}

void PluginWindowManager::closeWindow (PluginEditorWindow& window)
{
    for (auto it = windows.begin(); it != windows.end(); ++it)
    {
        if (it->get() == &window)
        {
            windows.erase (it);
            return;
        }
    }
}

} // namespace gocue
