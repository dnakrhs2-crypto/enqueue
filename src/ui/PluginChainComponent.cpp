#include "ui/PluginChainComponent.h"

#include "ui/UiUtils.h"

namespace gocue
{

namespace
{
    constexpr int slotWidth = 172;
    constexpr int slotGap = 26;   // room for the arrow between slots
    constexpr int slotHeight = 54;
}

//==============================================================================
class PluginChainComponent::SlotView : public juce::Component,
                                       public juce::SettableTooltipClient
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

        name.setText (title, juce::dontSendNotification);   // the number sits in the badge on the left
        name.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        name.setColour (juce::Label::textColourId, missing ? Palette::missing : Palette::text);
        name.setMinimumHorizontalScale (0.8f);
        name.setInterceptsMouseClicks (false, false);   // a double-click on the name reaches mouseDoubleClick() below
        setTooltip (slot.state.fileOrIdentifier);
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

        // 활성 (green) / 비활성 (orange): the state is the label and the colour
        const bool bypassed = slot.bypassed.load();
        bypass.setButtonText (bypassed ? ko ("비활성") : ko ("활성"));
        bypass.setTooltip (ko ("누르면 활성 ↔ 비활성 (비활성 = 소리가 이 플러그인을 건너뜀)"));
        bypass.setColour (juce::TextButton::buttonColourId, bypassed ? Palette::fadingOut : Palette::playing);
        bypass.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
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
        top.removeFromLeft (24);   // the number badge
        name.setBounds (top);
        area.removeFromTop (2);
        bypass.setBounds (area.removeFromLeft (54));
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

        // the processing order: a numbered badge
        const auto badge = juce::Rectangle<float> (5.0f, 5.0f, 18.0f, 18.0f);
        g.setColour (missing ? Palette::missing : Palette::standby);
        g.fillEllipse (badge);
        g.setColour (juce::Colours::white);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (juce::String (index + 1), badge, juce::Justification::centred, false);
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
void PluginChainComponent::Strip::paint (juce::Graphics& g)
{
    // an arrow in every gap: the sound leaves slot n and enters slot n + 1
    g.setColour (Palette::dimText);

    for (int i = 1; i < numSlots; ++i)
    {
        const float x = (float) (i * slotWidth + (i - 1) * slotGap) + (float) slotGap * 0.5f;
        const float y = (float) slotHeight * 0.5f;
        juce::Path arrow;
        arrow.addTriangle (x - 5.0f, y - 6.0f, x + 5.0f, y, x - 5.0f, y + 6.0f);
        g.fillPath (arrow);
    }
}

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

        strip.numSlots = numSlots;
        strip.setSize (juce::jmax (1, numSlots * (slotWidth + slotGap)), slotHeight);
        emptyLabel.setText (numSlots == 0 ? ko ("플러그인 없음 - [+ 플러그인]으로 추가하세요 (①→②→③ 순서대로 직렬 처리)") : juce::String(),
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
        juce::KnownPluginList::addToMenu (menu, types, juce::KnownPluginList::sortByManufacturer);   // one submenu per maker
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
    if (chain == nullptr || from < 0 || from >= chain->getNumSlots())
        return;

    // Deferred: the click arrives from a button inside a slot view that refresh() destroys.
    juce::Component::SafePointer<PluginChainComponent> safeThis (this);
    auto* expectedChain = chain;
    const int expectedCount = chain->getNumSlots();
    const PluginChain::Slot* expectedSlot = &chain->getSlot (from);   // the slot objects keep their address across moves

    juce::MessageManager::callAsync ([safeThis, expectedChain, expectedCount, expectedSlot, from, to]
    {
        if (safeThis == nullptr || safeThis->chain == nullptr || safeThis->chain != expectedChain)
            return;

        if (safeThis->chain->getNumSlots() != expectedCount || to < 0 || to >= expectedCount
            || &safeThis->chain->getSlot (from) != expectedSlot)
        {
            safeThis->refresh();   // the chain changed underneath us (another move, an edit elsewhere): do not guess
            return;
        }

        auto* target = safeThis->chain;
        safeThis->runEdit (ko ("플러그인 순서 변경"), [target, from, to] { target->movePlugin (from, to); });
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
