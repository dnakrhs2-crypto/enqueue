#include "ui/PluginChainComponent.h"

#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    constexpr int slotWidth = 172;
    constexpr int slotGap = 6;
    constexpr int slotHeight = 54;
}

//==============================================================================
class PluginChainComponent::SlotView : public juce::Component
{
public:
    SlotView (PluginChainComponent& o, int slotIndex, int slotCount, const PluginChain::Slot& slot)
        : owner (o), index (slotIndex), missing (slot.isMissing())
    {
        juce::String title = slot.plugin != nullptr ? slot.plugin->getName() : slot.state.name;

        if (title.isEmpty())
            title = ko ("(이름 없음)");

        if (missing)
            title = ko ("[없음] ") + title;

        name.setText (juce::String (slotIndex + 1) + ko ("번 ") + title, juce::dontSendNotification);
        name.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        name.setColour (juce::Label::textColourId, missing ? Palette::missing : Palette::text);
        name.setMinimumHorizontalScale (0.8f);
        name.setTooltip (slot.state.fileOrIdentifier);
        addAndMakeVisible (name);

        // order = processing order: the sound leaves slot 1 and enters slot 2
        earlier.setButtonText ("<");
        earlier.setTooltip (ko ("앞으로 (먼저 처리)"));
        earlier.setEnabled (slotIndex > 0);
        earlier.setWantsKeyboardFocus (false);
        earlier.onClick = [this] { owner.moveSlot (index, index - 1); };
        addAndMakeVisible (earlier);

        later.setButtonText (">");
        later.setTooltip (ko ("뒤로 (나중에 처리)"));
        later.setEnabled (slotIndex < slotCount - 1);
        later.setWantsKeyboardFocus (false);
        later.onClick = [this] { owner.moveSlot (index, index + 1); };
        addAndMakeVisible (later);

        bypass.setButtonText ("B");
        bypass.setTooltip (ko ("바이패스"));
        bypass.setClickingTogglesState (true);
        bypass.setToggleState (slot.bypassed.load(), juce::dontSendNotification);
        bypass.setColour (juce::TextButton::buttonOnColourId, Palette::fadingOut);
        bypass.setWantsKeyboardFocus (false);
        bypass.onClick = [this] { owner.toggleBypass (index); };
        addAndMakeVisible (bypass);

        edit.setButtonText (ko ("편집"));
        edit.setEnabled (! missing);
        edit.setWantsKeyboardFocus (false);
        edit.onClick = [this] { owner.openEditor (index); };
        addAndMakeVisible (edit);

        remove.setButtonText ("x");
        remove.setTooltip (ko ("삭제"));
        remove.setColour (juce::TextButton::buttonColourId, Palette::stopButton);
        remove.setWantsKeyboardFocus (false);
        remove.onClick = [this] { owner.removeSlot (index); };
        addAndMakeVisible (remove);

        setSize (slotWidth, slotHeight);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4);
        auto top = area.removeFromTop (20);
        later.setBounds (top.removeFromRight (22));
        top.removeFromRight (2);
        earlier.setBounds (top.removeFromRight (22));
        top.removeFromRight (4);
        name.setBounds (top);
        area.removeFromTop (2);
        bypass.setBounds (area.removeFromLeft (28));
        area.removeFromLeft (4);
        remove.setBounds (area.removeFromRight (28));
        area.removeFromRight (4);
        edit.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (missing ? Palette::missing.withAlpha (0.12f) : Palette::rowEven);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (Palette::outline);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! missing)
            owner.openEditor (index);
    }

private:
    PluginChainComponent& owner;
    const int index;
    const bool missing;
    juce::Label name;
    juce::TextButton earlier, later, bypass, edit, remove;
};

//==============================================================================
PluginChainComponent::PluginChainComponent (AudioEngine& e, PluginWindowManager& w)
    : engine (e), windows (w)
{
    viewport.setViewedComponent (&strip, false);
    viewport.setScrollBarsShown (false, true, false, true);
    viewport.setScrollBarThickness (8);
    addAndMakeVisible (viewport);

    addButton.setButtonText (ko ("+ 플러그인"));
    addButton.setWantsKeyboardFocus (false);
    addButton.onClick = [this] { showAddMenu(); };
    addAndMakeVisible (addButton);

    manageButton.setButtonText (ko ("관리..."));
    manageButton.setTooltip (ko ("VST3 플러그인 스캔 / 목록 관리"));
    manageButton.setWantsKeyboardFocus (false);
    manageButton.onClick = [this] { if (onOpenPluginManager) onOpenPluginManager(); };
    addAndMakeVisible (manageButton);

    emptyLabel.setColour (juce::Label::textColourId, Palette::dimText);
    emptyLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    emptyLabel.setInterceptsMouseClicks (false, false);   // it overlays the strip; never swallow slot clicks
    addAndMakeVisible (emptyLabel);

    setChain (nullptr, {});
}

PluginChainComponent::~PluginChainComponent()
{
    cancelPendingUpdate();
    viewport.setViewedComponent (nullptr, false);
}

void PluginChainComponent::setChain (PluginChain* newChain, const juce::String& newOwnerName)
{
    chain = newChain;
    ownerName = newOwnerName;
    refresh();
}

void PluginChainComponent::chainChanged (PluginChain* changed)
{
    if (changed != nullptr && changed == chain)
        triggerAsyncUpdate();   // deferred: the change may originate from a button inside a slot view
}

void PluginChainComponent::refresh()
{
    slotViews.clear();
    strip.removeAllChildren();

    const bool enabled = chain != nullptr;
    addButton.setEnabled (enabled);

    if (chain != nullptr)
    {
        const int numSlots = chain->getNumSlots();

        for (int i = 0; i < numSlots; ++i)
        {
            auto view = std::make_unique<SlotView> (*this, i, numSlots, chain->getSlot (i));
            view->setTopLeftPosition (i * (slotWidth + slotGap), 0);
            strip.addAndMakeVisible (*view);
            slotViews.push_back (std::move (view));
        }

        strip.setSize (juce::jmax (1, numSlots * (slotWidth + slotGap)), slotHeight);
        emptyLabel.setText (numSlots == 0 ? ko ("인서트 없음 - [+ 플러그인]으로 VST3를 추가하세요") : juce::String(),
                            juce::dontSendNotification);
        emptyLabel.setVisible (numSlots == 0);
    }
    else
    {
        strip.setSize (1, slotHeight);
        emptyLabel.setText (ko ("선택된 큐 없음"), juce::dontSendNotification);
        emptyLabel.setVisible (true);
    }

    resized();
    repaint();
}

void PluginChainComponent::showAddMenu()
{
    if (chain == nullptr)
        return;

    const auto types = engine.getPluginHost().getEffectTypes();
    juce::PopupMenu menu;

    if (types.isEmpty())
    {
        menu.addItem (1, ko ("스캔된 VST3 플러그인이 없습니다 - 플러그인 관리에서 스캔..."));
    }
    else
    {
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortAlphabetically);
        menu.addSeparator();
        menu.addItem (1, ko ("플러그인 관리 (스캔)..."));
    }

    juce::Component::SafePointer<PluginChainComponent> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton),
                        [safeThis, types] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        if (result == 1)
        {
            if (safeThis->onOpenPluginManager)
                safeThis->onOpenPluginManager();

            return;
        }

        const int index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

        if (index >= 0)
            safeThis->addPlugin (types[index]);
    });
}

void PluginChainComponent::addPlugin (const juce::PluginDescription& description)
{
    if (chain == nullptr)
        return;

    juce::String error;
    auto instance = engine.getPluginHost().createInstance (description, engine.getSampleRate(), engine.getBlockSize(), error);

    if (instance == nullptr)
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (ko ("플러그인을 불러오지 못했습니다"))
                                          .withMessage (description.name + "\n" + error)
                                          .withButton (ko ("확인"))
                                          .withAssociatedComponent (this),
                                      [] (int) {});
        return;
    }

    // std::function needs a copyable callable, so the instance travels in a shared holder and is moved out once.
    auto* target = chain;
    auto holder = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>> (std::move (instance));
    runEdit (ko ("플러그인 추가"), [target, holder] { target->addPlugin (std::move (*holder)); });
    refresh();
    openEditor (chain->getNumSlots() - 1);
}

void PluginChainComponent::runEdit (const juce::String& name, const std::function<void()>& edit)
{
    if (performEdit)
        performEdit (name, edit);
    else
        edit();
}

void PluginChainComponent::openEditor (int index)
{
    if (chain == nullptr || index < 0 || index >= chain->getNumSlots())
        return;

    auto& slot = chain->getSlot (index);

    if (slot.plugin != nullptr)
        windows.open (*slot.plugin, ownerName + " - " + slot.plugin->getName());
}

void PluginChainComponent::removeSlot (int index)
{
    // Deferred: the click arrives from a button inside the slot view we are about to destroy.
    juce::Component::SafePointer<PluginChainComponent> safeThis (this);
    auto* expectedChain = chain;
    const int expectedCount = chain != nullptr ? chain->getNumSlots() : 0;

    juce::MessageManager::callAsync ([safeThis, expectedChain, expectedCount, index]
    {
        if (safeThis == nullptr || safeThis->chain == nullptr || safeThis->chain != expectedChain)
            return;

        if (safeThis->chain->getNumSlots() != expectedCount)   // the chain changed underneath us: do not guess
        {
            safeThis->refresh();
            return;
        }

        auto* target = safeThis->chain;
        safeThis->runEdit (ko ("플러그인 삭제"), [target, index] { target->removePlugin (index); });
        safeThis->refresh();
    });
}

void PluginChainComponent::moveSlot (int from, int to)
{
    // Deferred: the click arrives from a button inside a slot view that refresh() destroys.
    juce::Component::SafePointer<PluginChainComponent> safeThis (this);
    auto* expectedChain = chain;
    const int expectedCount = chain != nullptr ? chain->getNumSlots() : 0;

    juce::MessageManager::callAsync ([safeThis, expectedChain, expectedCount, from, to]
    {
        if (safeThis == nullptr || safeThis->chain == nullptr || safeThis->chain != expectedChain)
            return;

        if (safeThis->chain->getNumSlots() != expectedCount || to < 0 || to >= expectedCount)
        {
            safeThis->refresh();   // the chain changed underneath us: do not guess
            return;
        }

        auto* target = safeThis->chain;
        safeThis->runEdit (ko ("이펙트 순서 변경"), [target, from, to] { target->movePlugin (from, to); });
        safeThis->refresh();
    });
}

void PluginChainComponent::toggleBypass (int index)
{
    if (chain == nullptr || index < 0 || index >= chain->getNumSlots())
        return;

    auto* target = chain;
    const bool bypass = ! chain->getSlot (index).bypassed.load();
    runEdit (bypass ? ko ("바이패스") : ko ("바이패스 해제"), [target, index, bypass] { target->setBypassed (index, bypass); });
}

void PluginChainComponent::resized()
{
    auto area = getLocalBounds();
    auto buttons = area.removeFromRight (100);
    manageButton.setBounds (buttons.removeFromBottom (24));
    buttons.removeFromBottom (4);
    addButton.setBounds (buttons.removeFromBottom (24));
    area.removeFromRight (8);

    viewport.setBounds (area);
    emptyLabel.setBounds (area.withHeight (slotHeight));
}

void PluginChainComponent::paint (juce::Graphics&)
{
}

} // namespace gocue
