#include "ui/ContainerTabs.h"
#include "ui/FooterBar.h"

#include "ui/CueMenuIcons.h"
#include "ui/UiUtils.h"

namespace gocue
{

ContainerTabs::ContainerTabs (ProjectDocument& d) : document (d)
{
    setWantsKeyboardFocus (false);
    refresh();
}

void ContainerTabs::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;
    repaint();
}

void ContainerTabs::setInfoText (juce::String text)
{
    if (infoText == text)
        return;
    infoText = std::move (text);
    resized();
    repaint();
}

void ContainerTabs::setStatusBar (FooterBar& status)
{
    statusBar = &status;
    addAndMakeVisible (status);
    resized();
}

void ContainerTabs::refresh()
{
    tabs.clear();

    for (int i = 0; i < document.getNumContainers(); ++i)
    {
        const auto info = document.getContainerInfo (i);
        Tab t;
        t.name = info.name;
        t.isCart = info.isCart;
        t.active = i == document.getActiveContainer();
        tabs.push_back (std::move (t));
    }

    resized();
    repaint();
}

void ContainerTabs::resized()
{
    const auto font = Palette::font (Palette::tabSize, true);
    const int infoWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::headerSize), infoText);
    const int statusWidth = statusBar != nullptr ? statusBar->getPreferredWidth() : 0;
    const int desired = infoWidth + (statusBar != nullptr ? 16 + statusWidth : 0);
    auto right = getLocalBounds().withTrimmedRight (14).withY (6).withHeight (juce::jmax (0, getHeight() - 6));
    right = right.removeFromRight (juce::jmin (desired, juce::jmax (0, getWidth() * 3 / 5)));
    const int tabsRight = juce::jmax (8, right.getX() - 16);
    infoBounds = right.removeFromRight (right.getWidth() >= desired ? infoWidth
        : juce::jmin (infoWidth, right.getWidth() / (statusBar != nullptr ? 3 : 1)));
    if (statusBar != nullptr)
    {
        right.removeFromRight (16);
        statusBar->setBounds (right);
    }

    int preferredTabsWidth = 0;
    for (const auto& t : tabs)
        preferredTabsWidth += juce::jlimit (60, 220, juce::GlyphArrangement::getStringWidthInt (font, t.name) + 44) + 2;
    const int available = juce::jmax (0, tabsRight - 8 - 28);
    int x = 8;

    for (auto& t : tabs)
    {
        const int preferred = juce::jlimit (60, 220, juce::GlyphArrangement::getStringWidthInt (font, t.name) + 44);
        const int width = preferredTabsWidth <= available ? preferred
            : juce::jmax (0, (preferred + 2) * available / juce::jmax (1, preferredTabsWidth) - 2);
        t.bounds = { x, 6, width, getHeight() - 6 };
        x += width + 2;
    }

    addButton = { x + 2, 6, 26, getHeight() - 6 };
}

void ContainerTabs::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel2);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, (float) getHeight() - 0.5f, (float) getWidth(), (float) getHeight() - 0.5f);

    for (const auto& t : tabs)
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (t.bounds);
        auto r = t.bounds;
        Palette::drawTab (g, r, t.active);
        int textX = r.getX() + 12;
        g.setColour (t.active ? Palette::text : Palette::muted);
        const float gx = (float) textX, gy = (float) r.getCentreY() - 5.0f;
        juce::Path icon;

        if (t.isCart)
        {
            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    icon.addRoundedRectangle (gx + (float) i * 6.0f, gy + (float) j * 6.0f, 4.0f, 4.0f, 0.5f);
        }
        else
            for (int i = 0; i < 3; ++i)
                icon.addRectangle (gx, gy + 1.0f + (float) i * 3.0f, 9.0f, 1.3f);
        g.fillPath (icon);
        textX += 16;

        g.setColour (t.active ? Palette::text : Palette::dimText);
        g.setFont (Palette::font (Palette::tabSize, t.active));
        g.drawText (t.name, textX, r.getY(), r.getRight() - textX - 6, r.getHeight(), juce::Justification::centredLeft, true);
    }

    g.setColour (Palette::accent.withMultipliedAlpha (editable ? 1.0f : Palette::disabledAlpha));
    g.setFont (Palette::font (Palette::bodySize, true));
    g.drawText ("+", addButton, juce::Justification::centred, false);
    g.setColour (Palette::muted);
    g.setFont (Palette::font (Palette::headerSize));
    g.drawText (infoText, infoBounds, juce::Justification::centredRight, true);
}

int ContainerTabs::tabAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) tabs.size(); ++i)
        if (tabs[(size_t) i].bounds.contains (p))
            return i;

    return -1;
}

void ContainerTabs::mouseDown (const juce::MouseEvent& e)
{
    const int tab = tabAt (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (tab >= 0 && editable)
            showTabMenu (tab, e.getScreenPosition());

        return;
    }

    if (tab >= 0)
    {
        if (onSelect && editable)   // show mode: the GO target list does not change by a click
            onSelect (tab);

        return;
    }

    if (addButton.contains (e.getPosition()) && editable)
        showAddMenu();
}

void ContainerTabs::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int tab = tabAt (e.getPosition());

    if (tab >= 0 && editable && onRename)
        onRename (tab);
}

void ContainerTabs::showAddMenu()
{
    juce::PopupMenu menu;
    menu.addItem (juce::PopupMenu::Item (juce::String::fromUTF8 ("새 큐 리스트"))
                      .setID (1).setImage (CueMenuIcons::create (CommandIDs::addCueList)));
    menu.addItem (juce::PopupMenu::Item (juce::String::fromUTF8 ("새 카트 (버튼 격자)"))
                      .setID (2).setImage (CueMenuIcons::create (CommandIDs::addCart)));
    juce::Component::SafePointer<ContainerTabs> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (addButton)), [safeThis] (int result)
    {
        if (safeThis == nullptr)
            return;

        if (result == 1 && safeThis->onAddList)
            safeThis->onAddList();
        else if (result == 2 && safeThis->onAddCart)
            safeThis->onAddCart();
    });
}

void ContainerTabs::showTabMenu (int index, juce::Point<int> screenPosition)
{
    const auto info = document.getContainerInfo (index);
    juce::PopupMenu menu;
    menu.addItem (1, juce::String::fromUTF8 ("이름 바꾸기..."));
    menu.addItem (2, info.isCart ? juce::String::fromUTF8 ("큐 리스트로 전환") : juce::String::fromUTF8 ("카트로 전환"));
    menu.addItem (3, juce::String::fromUTF8 ("카트 격자 크기...") + " (" + juce::String (info.cartRows) + " x " + juce::String (info.cartCols) + ")", info.isCart);
    menu.addSeparator();
    menu.addItem (4, juce::String::fromUTF8 ("삭제"), document.getNumContainers() > 1);
    juce::Component::SafePointer<ContainerTabs> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPosition.x, screenPosition.y, 1, 1 }), [safeThis, index] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& self = *safeThis;

        if (result == 1 && self.onRename)         self.onRename (index);
        else if (result == 2 && self.onToggleCart) self.onToggleCart (index);
        else if (result == 3 && self.onGridSize)   self.onGridSize (index);
        else if (result == 4 && self.onRemove)     self.onRemove (index);
    });
}

} // namespace gocue
