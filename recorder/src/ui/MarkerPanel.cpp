#include "MarkerPanel.h"
#include "UiState.h"
#include "model/MarkerExport.h"
#include <algorithm>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // ReplaceFileW with a backup name; juce::File::replaceFileIn passes none

namespace gocue::recorder
{
MarkerPanel::MarkerPanel(TimelineEditController& c) : edits(c)
{
    addAndMakeVisible(viewport); viewport.setViewedComponent(&content, false); viewport.setScrollBarsShown(true, false);
    content.addAndMakeVisible(list); list.setModel(this); list.setRowHeight(28);
    for (auto* b : {&add, &change, &remove, &exportText}) { addAndMakeVisible(b); b->setWantsKeyboardFocus(false); }   // pinned under the scrolling part
    for (auto* e : {&name, &position}) { content.addAndMakeVisible(e); e->onReturnKey = [this] { apply(); }; }
    content.addAndMakeVisible(colours);
    content.addAndMakeVisible(inputError); inputError.setFont(recorderFont(11.5f));
    inputError.setColour(juce::Label::textColourId, noticeColour(NoticeKind::failure));
    inputError.setJustificationType(juce::Justification::topLeft); inputError.setMinimumHorizontalScale(1);
    name.onTextChange = position.onTextChange = [this] { clearInputError(); };
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
    const auto& next = edits.document.getProject().markers;
    if (inputError.getText().isNotEmpty())
        for (const auto& before : markers) if (before.markerId == selected)
            for (const auto& after : next) if (after.markerId == selected && (before.sample != after.sample || before.name != after.name || before.colour != after.colour)) clearInputError();
    markers = next;
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.sample < b.sample; });
    list.updateContent(); int index = -1;
    for (unsigned i = 0; i < markers.size(); ++i) if (markers[i].markerId == selected) index = int(i);
    if (index < 0) { selected.clear(); list.deselectAllRows(); name.clear(); position.clear(); colours.setSelected({}); clearInputError(); }
    else
    {
        list.selectRow(index); colours.setSelected(markers[std::size_t(index)].colour);
        if (inputError.getText().isEmpty() && !name.hasKeyboardFocus(true) && !position.hasKeyboardFocus(true) && !colours.hasKeyboardFocus(true)) selectedRowsChanged(index);
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
    clearInputError();
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
    if (!valid)
    {
        inputError.setText(ko("마커 위치는 시간(mm:ss, mm:ss.mmm, h:mm:ss.mmm) 또는 정수 샘플로 입력하세요."), juce::dontSendNotification);
        errorAtName = false; setInputError(name, false); setInputError(position, true); resized(); return;
    }
    clearInputError();
    const auto r = edits.editMarker(selected, at, name.getText(), colours.selected());
    if (r.failed() && name.getText().trim().isEmpty())
    {
        errorAtName = true; inputError.setText(r.getErrorMessage(), juce::dontSendNotification);
        setInputError(name, true); resized(); return;
    }
    if (onEdit) onEdit(r);
}
void MarkerPanel::clearInputError()
{
    if (inputError.getText().isEmpty()) return;
    inputError.setText({}, juce::dontSendNotification); setInputError(position, false); setInputError(name, false); resized();
}
juce::String MarkerPanel::rowTimeText(int row) const
{
    return row >= 0 && row < int(markers.size()) ? formatMarkerTime(markers[std::size_t(row)].sample, edits.document.getProject().Fs) : juce::String();
}
juce::File MarkerPanel::defaultExportFile() const
{
    const auto& file = edits.document.getFile();
    const auto folder = file == juce::File() ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory) : file.getParentDirectory();
    const auto stem = juce::File::createLegalFileName(edits.document.getProject().name); // not `name`: that is the editor member
    return folder.getChildFile((stem.isEmpty() ? ko("프로젝트") : stem) + ko(" 마커.txt"));
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
juce::Result MarkerPanel::landExport(const juce::File& target, const juce::File& temporary, const SwapFunction& swap)
{
    const auto cannotSave = juce::Result::fail(ko("마커를 저장할 수 없습니다. ") + target.getFullPathName());
    if (!target.existsAsFile()) return temporary.moveFileTo(target) ? juce::Result::ok() : cannotSave;
    // Swap *with* a backup name so the previous file survives every failure: the backup-less swap JUCE performs can lose
    // the target on ERROR_UNABLE_TO_MOVE_REPLACEMENT, and a copy fallback deletes the target before copying. The name must
    // not exist yet, because ReplaceFileW would overwrite an existing backup.
    const auto backup = target.getParentDirectory().getNonexistentChildFile("." + target.getFileNameWithoutExtension() + "_backup", target.getFileExtension());
    const SwapFunction replaceFile = [](const juce::File& t, const juce::File& n, const juce::File& b)
    {
        return ReplaceFileW(t.getFullPathName().toWideCharPointer(), n.getFullPathName().toWideCharPointer(), b.getFullPathName().toWideCharPointer(),
                            REPLACEFILE_IGNORE_MERGE_ERRORS | 4 /*REPLACEFILE_IGNORE_ACL_ERRORS*/, nullptr, nullptr) != 0;
    };
    if ((swap ? swap : replaceFile)(target, temporary, backup)) { backup.deleteFile(); return juce::Result::ok(); }
    // Failed swap: ERROR_UNABLE_TO_REMOVE_REPLACED / _MOVE_REPLACEMENT leave the previous file under its own name, but
    // _MOVE_REPLACEMENT_2 leaves it under the backup name. Put it back, and when even that fails say where it survived.
    if (target.existsAsFile() || !backup.existsAsFile() || backup.moveFileTo(target)) return cannotSave;
    return juce::Result::fail(ko("마커를 저장할 수 없고 이전 파일을 원래 이름으로 되돌리지 못했습니다. 이전 파일은 여기에 남아 있습니다: ") + backup.getFullPathName());
}
void MarkerPanel::exportFile(const juce::File& file, const RecorderProject& project)
{
    const auto text = markerExportText(project);
    const auto write = [&]() -> juce::Result
    {
        juce::TemporaryFile temporary(file, juce::TemporaryFile::useHiddenFile);
        {
            juce::FileOutputStream stream(temporary.getFile());
            const bool written = !stream.failedToOpen() && stream.write(text.toRawUTF8(), text.getNumBytesAsUTF8());
            if (written) stream.flush();
            if (!written || stream.getStatus().failed()) return juce::Result::fail(ko("마커를 저장할 수 없습니다. ") + file.getFullPathName());
        }
        return landExport(file, temporary.getFile()); // a temporary that is still there afterwards is deleted by its owner
    };
    if (const auto landed = write(); landed.failed())
    {
        if (onEdit) onEdit(landed);
        return;
    }
    if (onEdit) onEdit(juce::Result::ok());
    if (onStatusChanged) onStatusChanged(ko("마커 ") + juce::String(project.markers.size()) + ko("개를 내보냈습니다: ") + file.getFileName());
}
void MarkerPanel::resized()
{
    const auto width = juce::jmax(140, getWidth() - 18); const auto swatchHeight = colours.heightForWidth(width - 6);
    int errorHeight = 0;
    if (inputError.getText().isNotEmpty())
    {
        juce::AttributedString text; text.append(inputError.getText(), inputError.getFont(), noticeColour(NoticeKind::failure));
        juce::TextLayout layout; layout.createLayout(text, float(width - 6 - inputError.getBorderSize().getLeftAndRight()));
        errorHeight = int(std::ceil(layout.getHeight())) + inputError.getBorderSize().getTopAndBottom() + 4;
    }
    // The buttons stay pinned under the scrolling part, so a short panel (1920x1080 at 150 %) still shows all four.
    auto panel = getLocalBounds();
    auto pinned = panel.removeFromBottom(6 + 28 + 4 + 28 + 3).withWidth(width).reduced(3, 0);
    pinned.removeFromTop(6); exportText.setBounds(pinned.removeFromBottom(31).removeFromTop(28)); pinned.removeFromBottom(4);
    auto buttons = pinned.removeFromBottom(28); add.setBounds(buttons.removeFromLeft(92)); remove.setBounds(buttons.removeFromRight(50)); change.setBounds(buttons);
    viewport.setBounds(panel); content.setSize(width, juce::jmax(160 + swatchHeight + errorHeight, panel.getHeight()));
    auto a = content.getLocalBounds().reduced(3);
    colours.setBounds(a.removeFromBottom(swatchHeight)); a.removeFromBottom(6);
    if (!errorAtName) inputError.setBounds(a.removeFromBottom(errorHeight));
    position.setBounds(a.removeFromBottom(26)); a.removeFromBottom(2);
    if (errorAtName) inputError.setBounds(a.removeFromBottom(errorHeight));
    name.setBounds(a.removeFromBottom(26)); a.removeFromBottom(4); list.setBounds(a);
}
}
