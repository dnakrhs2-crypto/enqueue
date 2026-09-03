#include "ui/ContainerTabs.h"

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
    juce::Font font (juce::FontOptions (15.0f));
    int x = 4;

    for (auto& t : tabs)
    {
        const int width = juce::jlimit (60, 220, (int) juce::GlyphArrangement::getStringWidth (font, t.name) + (t.isCart ? 44 : 28));
        t.bounds = { x, 2, width, getHeight() - 4 };
        x += width + 2;
    }

    addButton = { x + 2, 2, 26, getHeight() - 4 };
}

void ContainerTabs::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    g.setColour (Palette::outline);
    g.drawLine (0.0f, (float) getHeight() - 0.5f, (float) getWidth(), (float) getHeight() - 0.5f);

    for (const auto& t : tabs)
    {
        auto r = t.bounds;
        g.setColour (t.active ? Palette::rowEven.brighter (0.25f) : Palette::background);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);

        if (t.active)
        {
            g.setColour (Palette::standby);
            g.fillRect (r.getX() + 6, r.getBottom() - 3, r.getWidth() - 12, 2);
        }

        int textX = r.getX() + 8;

        if (t.isCart)
        {
            // a little 2x2 grid marks a cart
            g.setColour (t.active ? Palette::text : Palette::dimText);
            const float gx = (float) textX, gy = (float) r.getCentreY() - 5.0f;

            for (int i = 0; i < 2; ++i)
                for (int j = 0; j < 2; ++j)
                    g.fillRect (gx + (float) i * 6.0f, gy + (float) j * 6.0f, 4.5f, 4.5f);

            textX += 16;
        }

        g.setColour (t.active ? Palette::text : Palette::dimText);
        g.setFont (juce::Font (juce::FontOptions (15.0f, t.active ? juce::Font::bold : juce::Font::plain)));
        g.drawText (t.name, textX, r.getY(), r.getRight() - textX - 6, r.getHeight(), juce::Justification::centredLeft, true);
    }

    g.setColour (editable ? Palette::dimText : Palette::outline);
    g.setFont (juce::Font (juce::FontOptions (18.0f)));
    g.drawText ("+", addButton, juce::Justification::centred, false);
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
        if (onSelect)
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
    menu.addItem (1, juce::String::fromUTF8 ("새 큐 리스트"));
    menu.addItem (2, juce::String::fromUTF8 ("새 카트 (버튼 격자)"));
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
