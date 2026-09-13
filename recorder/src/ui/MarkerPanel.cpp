#include "MarkerPanel.h"
#include "UiState.h"
#include "model/MarkerExport.h"
#include <algorithm>

namespace gocue::recorder
{
MarkerPanel::MarkerPanel(TimelineEditController& c) : edits(c)
{
    addAndMakeVisible(viewport); viewport.setViewedComponent(&content, false); viewport.setScrollBarsShown(true, false);
    content.addAndMakeVisible(list); list.setModel(this); list.setRowHeight(28);
    for (auto* b : {&add, &change, &remove, &exportText}) { content.addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }
    for (auto* e : {&name, &position}) { content.addAndMakeVisible(e); e->onReturnKey = [this] { apply(); }; }
    content.addAndMakeVisible(colours);
    name.setTextToShowWhenEmpty(ko("이름"), Palette::dimText); position.setTextToShowWhenEmpty(ko("위치 · 시간 또는 샘플"), Palette::dimText);
    position.setTooltip(ko("위치 · mm:ss, mm:ss.mmm, h:mm:ss.mmm 또는 정수 샘플 · Enter로 적용"));
    name.setTooltip(ko("이름 · Enter로 적용"));
    position.setFont(recorderMonoFont(12)); position.setJustification(juce::Justification::centredRight);
    for (auto* button : {&add, &change, &remove, &exportText}) button->getProperties().set("recorderFontSize", 12.0f);
    add.onClick = [this] { if (onAddRequested) { onAddRequested(); return; } const auto r = edits.addMarker(); if (onEdit) onEdit(r); };
    change.onClick = [this] { apply(); };
    remove.onClick = [this] { const auto r = edits.deleteMarker(selected); if (onEdit) onEdit(r); };
    colours.onPick = [this](const juce::String& hex)
    {
        const auto project = edits.document.snapshot();
        const auto found = std::find_if(project->markers.begin(), project->markers.end(), [this](const Marker& m) { return m.markerId == selected; });
        if (found == project->markers.end()) return;
        const auto result = edits.editMarker(selected, found->sample, found->name, hex);
        if (result.failed()) colours.setSelected(found->colour);
        if (onEdit) onEdit(result);
    };
    exportText.onClick = [this] { chooseExportFile(); };
    refresh();
}
void MarkerPanel::refresh()
{
    markers = edits.document.getProject().markers;
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.sample < b.sample; });
    list.updateContent(); int index = -1;
    for (unsigned i = 0; i < markers.size(); ++i) if (markers[i].markerId == selected) index = int(i);
    if (index < 0) { selected.clear(); list.deselectAllRows(); name.clear(); position.clear(); colours.setSelected({}); }
    else
    {
        list.selectRow(index); colours.setSelected(markers[std::size_t(index)].colour);
        if (!name.hasKeyboardFocus(true) && !position.hasKeyboardFocus(true) && !colours.hasKeyboardFocus(true)) selectedRowsChanged(index);
    }
    const bool canEdit = !selected.isEmpty() && edits.enabled(TimelineAction::editMarker);
    for (auto* e : {&name, &position}) e->setEnabled(canEdit);
    colours.setEnabled(canEdit); exportText.setEnabled(!markers.empty() && !chooser);
    change.setEnabled(canEdit); remove.setEnabled(canEdit); add.setEnabled(edits.enabled(TimelineAction::addMarker)); list.repaint();
}
void MarkerPanel::selectedRowsChanged(int row)
{
    if (row < 0 || row >= int(markers.size())) return;
    const auto& m = markers[std::size_t(row)]; selected = m.markerId;
    name.setText(m.name, false); position.setText(rowTimeText(row), false); colours.setSelected(m.colour);
    const bool canEdit = edits.enabled(TimelineAction::editMarker);
    for (auto* e : {&name, &position}) e->setEnabled(canEdit);
    colours.setEnabled(canEdit); change.setEnabled(canEdit); remove.setEnabled(canEdit);
}
void MarkerPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{ if (row >= 0 && row < int(markers.size())) { const auto r = edits.seek(markers[std::size_t(row)].sample); if (onEdit) onEdit(r); } }
void MarkerPanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool isSelected)
{
    if (row < 0 || row >= int(markers.size())) return; const auto& m = markers[std::size_t(row)];
    if (isSelected) g.fillAll(Palette::selection);
    g.setColour(Palette::line.withAlpha(.6f)); g.fillRect(0, h - 1, w, 1);
    g.setColour(juce::Colour::fromString(juce::String("ff") + m.colour.substring(1))); g.fillEllipse(5, float(h / 2 - 4), 8, 8);
    const auto time = rowTimeText(row); const auto font = recorderMonoFont(11.5f);
    const auto width = juce::jmin(juce::jmax(0, w - 20), juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, time)) + 6);
    g.setColour(Palette::dimText); g.setFont(font); g.drawText(time, 18, 0, width, h, juce::Justification::centredLeft, true);
    g.setColour(Palette::text); g.setFont(recorderFont(12.5f));
    g.drawText(ko(" · ") + m.name, 18 + width, 0, juce::jmax(0, w - 20 - width), h, juce::Justification::centredLeft, true);
}
void MarkerPanel::apply()
{
    Sample at = 0; const auto text = position.getText(); const auto& project = edits.document.getProject();
    const auto valid = TimelineEditController::parseTimecode(text, project.Fs, at) || TimelineEditController::parseSample(text, at);
    // A displayed millisecond may contain several samples: renaming must preserve the exact stored position.
    for (const auto& marker : project.markers) if (marker.markerId == selected && text == formatMarkerTime(marker.sample, project.Fs)) at = marker.sample;
    const auto r = valid ? edits.editMarker(selected, at, name.getText(), colours.selected())
        : juce::Result::fail(ko("마커 위치는 시간(mm:ss, mm:ss.mmm, h:mm:ss.mmm) 또는 정수 샘플로 입력하세요."));
    if (onEdit) onEdit(r);
}
juce::String MarkerPanel::rowTimeText(int row) const
{
    return row >= 0 && row < int(markers.size()) ? formatMarkerTime(markers[std::size_t(row)].sample, edits.document.getProject().Fs) : juce::String();
}
juce::File MarkerPanel::defaultExportFile() const
{
    const auto& file = edits.document.getFile();
    const auto folder = file == juce::File() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory) : file.getParentDirectory();
    const auto name = juce::File::createLegalFileName(edits.document.getProject().name);
    return folder.getChildFile((name.isEmpty() ? ko("프로젝트") : name) + ko(" 마커.txt"));
}
void MarkerPanel::chooseExportFile()
{
    const auto project = edits.document.snapshot();
    if (project->markers.empty() || chooser) return;
    if (chooseExportFileForTesting)
    {
        const auto file = chooseExportFileForTesting(); if (file != juce::File()) exportFile(file, *project); return;
    }
    chooser = std::make_unique<juce::FileChooser>(ko("마커 텍스트 내보내기"), defaultExportFile(), "*.txt");
    exportText.setEnabled(false);
    const juce::Component::SafePointer<MarkerPanel> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, project](const juce::FileChooser& choice)
        {
            if (!safe) return;
            const auto file = choice.getResult(); safe->chooser.reset(); safe->refresh();
            if (file != juce::File()) safe->exportFile(file, *project);
        });
}
void MarkerPanel::exportFile(const juce::File& file, const RecorderProject& project)
{
    const auto text = markerExportText(project);
    const auto write = [&]
    {
        juce::TemporaryFile temporary(file, juce::TemporaryFile::useHiddenFile);
        {
            juce::FileOutputStream stream(temporary.getFile());
            if (stream.failedToOpen() || !stream.write(text.toRawUTF8(), text.getNumBytesAsUTF8())) return false;
            stream.flush(); if (stream.getStatus().failed()) return false;
        }
        return temporary.overwriteTargetFileWithTemporary();
    };
    if (!write())
    {
        if (onEdit) onEdit(juce::Result::fail(ko("마커를 저장할 수 없습니다. ") + file.getFullPathName()));
        return;
    }
    if (onEdit) onEdit(juce::Result::ok());
    if (onStatusChanged) onStatusChanged(ko("마커 ") + juce::String(project.markers.size()) + ko("개를 내보냈습니다: ") + file.getFileName());
}
void MarkerPanel::resized()
{
    const auto width = juce::jmax(140, getWidth() - 18); const auto swatchHeight = colours.heightForWidth(width - 6);
    viewport.setBounds(getLocalBounds()); content.setSize(width, juce::jmax(226 + swatchHeight, getHeight()));
    auto a = content.getLocalBounds().reduced(3); exportText.setBounds(a.removeFromBottom(28)); a.removeFromBottom(4);
    auto buttons = a.removeFromBottom(28); add.setBounds(buttons.removeFromLeft(92)); remove.setBounds(buttons.removeFromRight(50)); change.setBounds(buttons);
    a.removeFromBottom(6); colours.setBounds(a.removeFromBottom(swatchHeight)); a.removeFromBottom(6);
    position.setBounds(a.removeFromBottom(26)); a.removeFromBottom(2); name.setBounds(a.removeFromBottom(26)); a.removeFromBottom(4); list.setBounds(a);
}
}
