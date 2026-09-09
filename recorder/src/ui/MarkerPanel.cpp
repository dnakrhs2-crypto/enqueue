#include "MarkerPanel.h"
#include <algorithm>

namespace gocue::recorder
{
MarkerPanel::MarkerPanel(TimelineEditController& c) : edits(c)
{
    addAndMakeVisible(viewport); viewport.setViewedComponent(&content, false); viewport.setScrollBarsShown(true, false);
    content.addAndMakeVisible(list); list.setModel(this); list.setRowHeight(28);
    for (auto* b : {&add, &change, &remove}) { content.addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* e : {&name, &position, &colour}) { content.addAndMakeVisible(e); e->onReturnKey = [this] { apply(); }; }
    name.setTextToShowWhenEmpty(ko("이름"), Palette::dimText); position.setTextToShowWhenEmpty(ko("위치 · 샘플"), Palette::dimText);
    colour.setTextToShowWhenEmpty(ko("색 #RRGGBB"), Palette::dimText); colour.setTooltip(ko("색 #RRGGBB · Enter로 적용"));
    position.setInputRestrictions(19, "0123456789"); name.setTooltip(ko("이름 · Enter로 적용"));
    add.onClick = [this] { const auto r = edits.addMarker(); if (onEdit) onEdit(r); };
    change.onClick = [this] { apply(); };
    remove.onClick = [this] { const auto r = edits.deleteMarker(selected); if (onEdit) onEdit(r); };
}
void MarkerPanel::refresh()
{
    markers = edits.document.getProject().markers;
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.sample < b.sample; });
    list.updateContent(); int index = -1;
    for (unsigned i = 0; i < markers.size(); ++i) if (markers[i].markerId == selected) index = int(i);
    if (index < 0) { selected.clear(); list.deselectAllRows(); }
    else { list.selectRow(index); if (!name.hasKeyboardFocus(true) && !position.hasKeyboardFocus(true) && !colour.hasKeyboardFocus(true)) selectedRowsChanged(index); }
    const bool canEdit = !selected.isEmpty() && edits.enabled(TimelineAction::editMarker);
    for (auto* e : {&name, &position, &colour}) e->setEnabled(canEdit);
    change.setEnabled(canEdit); remove.setEnabled(canEdit); add.setEnabled(edits.enabled(TimelineAction::addMarker)); list.repaint();
}
void MarkerPanel::selectedRowsChanged(int row)
{
    if (row < 0 || row >= int(markers.size())) return;
    const auto& m = markers[std::size_t(row)]; selected = m.markerId;
    name.setText(m.name, false); position.setText(juce::String(m.sample), false); colour.setText(m.colour, false);
    const bool canEdit = edits.enabled(TimelineAction::editMarker);
    for (auto* e : {&name, &position, &colour}) e->setEnabled(canEdit); change.setEnabled(canEdit); remove.setEnabled(canEdit);
}
void MarkerPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{ if (row >= 0 && row < int(markers.size())) { const auto r = edits.seek(markers[std::size_t(row)].sample); if (onEdit) onEdit(r); } }
void MarkerPanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool isSelected)
{
    if (row < 0 || row >= int(markers.size())) return; const auto& m = markers[std::size_t(row)];
    if (isSelected) g.fillAll(Palette::card2);
    g.setColour(juce::Colour::fromString("ff" + m.colour.substring(1))); g.fillEllipse(5, float(h / 2 - 4), 8, 8);
    g.setColour(Palette::text); g.setFont(juce::Font(juce::FontOptions(14)));
    g.drawText(juce::String(m.sample) + " · " + m.name, 18, 0, w - 20, h, juce::Justification::centredLeft, true);
}
void MarkerPanel::apply()
{
    Sample at = 0; const auto r = TimelineEditController::parseSample(position.getText(), at)
        ? edits.editMarker(selected, at, name.getText(), colour.getText()) : juce::Result::fail(ko("마커 위치는 정수 샘플로 입력하세요."));
    if (onEdit) onEdit(r);
}
void MarkerPanel::resized()
{
    viewport.setBounds(getLocalBounds()); content.setSize(juce::jmax(140, getWidth() - 18), juce::jmax(210, getHeight()));
    auto a = content.getLocalBounds().reduced(3); auto buttons = a.removeFromBottom(28); add.setBounds(buttons.removeFromLeft(92)); remove.setBounds(buttons.removeFromRight(50)); change.setBounds(buttons);
    auto fields = a.removeFromBottom(54); name.setBounds(fields.removeFromTop(26)); position.setBounds(fields.removeFromLeft(fields.getWidth() / 2)); colour.setBounds(fields.reduced(2, 0)); list.setBounds(a);
}
}
