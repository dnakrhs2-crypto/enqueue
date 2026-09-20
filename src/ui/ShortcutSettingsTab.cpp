#include "ui/MidiModalScope.h"
#include "ui/ShortcutSettingsTab.h"
#include "app/ShortcutDisplay.h"
#include "app/ShortcutKeyInput.h"
#include "app/Commands.h"
#include "ui/ShortcutRouter.h"
#include "ui/UiUtils.h"

namespace gocue
{
namespace ShortcutSettingsModel
{
namespace
{
juce::String nameFor (const juce::String& id)
{
    const auto* entry = ShortcutCatalog::get().find (id);
    return entry != nullptr ? entry->name : id;
}
Category categoryFor (const ShortcutDefinition& entry)
{
    switch (entry.category)
    {
        case ShortcutCategory::playback: return Category::playback;
        case ShortcutCategory::cue: return Category::cue;
        case ShortcutCategory::edit: return Category::edit;
        case ShortcutCategory::view: return Category::view;
        case ShortcutCategory::fileSettings: return entry.id.startsWith ("file.") ? Category::file : Category::settings;
    }
    return Category::all;
}
}

Issues inspect (const ShortcutService& service, const juce::String& action, const juce::KeyPress& key, const Cues& cues)
{
    Issues issues;
    ShortcutKeyContext context;
    context.cueHotkeys = cues;
    const auto owner = service.resolveKeyOwner (key, context, true);
    if (owner.commandID != 0 && owner.id != action)
    {
        issues.commandOwner = owner.id;
        issues.conflicts.add (ko ("다른 기능에서 사용: ") + nameFor (owner.id));
    }
    for (const auto& cue : cues)
        if (ShortcutKeyInput::keysOverlap (cue.key, key))
            issues.conflicts.addIfNotAlreadyThere (ko ("현재 프로젝트 큐와 충돌: ") + cue.id + ko (" → 큐 핫키 비활성"));
    const auto* entry = ShortcutCatalog::get().find (action);
    if (entry != nullptr && entry->cueTableOnlyKeys.contains (key))
        issues.limitations.add (ShortcutDisplay::key (key) + ko (": 큐 표에 포커스가 있을 때만 작동합니다."));
    const bool panic = entry != nullptr && entry->scope == ShortcutScope::application;
    if (ShortcutKeyInput::isStandardTextEditorKey (key))
        issues.limitations.add (panic ? ko ("입력창에서도 작동합니다.") : ko ("입력창에서는 문자·편집 조작이 우선입니다."));
    if (! panic)
        for (const auto scope : { ShortcutScope::cueTable, ShortcutScope::waveform, ShortcutScope::levelMatrix,
                                 ShortcutScope::curveEditor, ShortcutScope::groupTimeline })
        {
            context.focus = scope;
            const auto focused = service.resolveKeyOwner (key, context, true);
            if (focused.kind == ShortcutKeyOwner::Kind::fixedComponent)
                issues.limitations.addIfNotAlreadyThere (ShortcutCatalog::scopeLabel (scope) + ko ("에 포커스가 있을 땐 ")
                                                        + nameFor (focused.id) + ko ("이 우선"));
        }
    return issues;
}

std::vector<Row> rows (const ShortcutService& service, const Cues& cues, const std::vector<MidiInputService::Device>& devices, const std::vector<MidiBinding>& midiCues, const MidiTriggerRouter* router)
{
    std::vector<Row> result;
    for (const auto& entry : ShortcutCatalog::get().getCommands())
    {
        Row row { entry.id, entry.name, entry.description, ShortcutCatalog::scopeLabel (entry.scope), categoryFor (entry),
                  service.getKeys (entry.id), service.getProfile().overrides.count (entry.id) != 0, {}, {}, {}, {}, {} };
        row.midi = service.getMidiTriggers (entry.id);
        row.changed |= ! row.midi.empty();
        row.midiText = ShortcutDisplay::midi (service, row.midi);
        for (const auto& trigger : row.midi)
        {
            auto state = ShortcutDisplay::midiState (service, trigger, devices);
            if (router != nullptr && (state == ko ("연결") || state == ko ("준비 대기")))
                state = router->bindingStatus ({ entry.id, trigger, entry.commandID, true });
            row.midiStates.addIfNotAlreadyThere (state);
            for (const auto& device : devices)
                if (device.overloaded && (trigger.source == "any" || trigger.source == device.identifier)) row.midiStates.addIfNotAlreadyThere (ko ("과부하"));
            for (const auto& cue : midiCues)
                if (MidiTriggerRules::intersects (trigger, cue.trigger))
                    row.conflicts.addIfNotAlreadyThere (ko ("현재 프로젝트 큐 MIDI와 충돌: ") + cue.id + ko (" → 큐 MIDI 비활성"));
        }
        for (const auto& key : row.keys)
        {
            const auto issues = inspect (service, entry.id, key, cues);
            for (const auto& message : issues.conflicts) row.conflicts.addIfNotAlreadyThere (message);
            for (const auto& message : issues.limitations) row.limitations.addIfNotAlreadyThere (message);
        }
        for (const auto& diagnostic : service.getDiagnostics())
            if (diagnostic.actionID == entry.id)
                row.conflicts.addIfNotAlreadyThere (ko ("다른 기능에서 사용: ") + nameFor (diagnostic.otherActionID)
                                                   + " (" + ShortcutDisplay::key (diagnostic.key) + ")");
        result.push_back (std::move (row));
    }
    return result;
}

bool moveRemovesLastPanic (const ShortcutService& service, const juce::String& destination, const ShortcutKeys& movingKeys)
{
    const auto& panic = service.getKeys (CommandIDs::panicAll);
    return destination != "transport.panicAll" && ! panic.isEmpty()
        && std::all_of (panic.begin(), panic.end(), [&] (const auto& key)
        { return std::any_of (movingKeys.begin(), movingKeys.end(), [&] (const auto& moving) { return ShortcutKeyInput::keysOverlap (key, moving); }); });
}

std::vector<Row> filter (std::vector<Row> rows, const juce::String& search, Category category, Status status)
{
    const auto words = juce::StringArray::fromTokens (search.trim(), " \t", "");
    rows.erase (std::remove_if (rows.begin(), rows.end(), [&] (const Row& row)
    {
        if (category != Category::all && row.category != category) return true;
        if ((status == Status::changed && ! row.changed) || (status == Status::unassigned && (! row.keys.isEmpty() || ! row.midi.empty()))
            || (status == Status::conflict && row.conflicts.isEmpty()) || (status == Status::limited && row.limitations.isEmpty())) return true;
        if ((status == Status::disconnected && ! row.midiStates.contains (ko ("미연결")))
            || (status == Status::waiting && ! row.midiStates.joinIntoString (" ").contains (ko ("준비 대기")) && ! row.midiStates.contains (ko ("움직임 종료 대기")))
            || (status == Status::unavailable && ! row.midiStates.contains (ko ("사용 불가")) && ! row.midiStates.contains (ko ("선택 안 함")))
            || (status == Status::overloaded && ! row.midiStates.contains (ko ("과부하")))) return true;
        const auto haystack = row.name + " " + row.description + " " + row.scope + " " + ShortcutDisplay::keys (row.keys) + " " + row.midiText + " " + row.midiStates.joinIntoString (" ");
        return std::any_of (words.begin(), words.end(), [&] (const auto& word) { return ! haystack.containsIgnoreCase (word); });
    }), rows.end());
    std::stable_sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b)
    {
        if (a.category != b.category) return a.category < b.category;
        const int compared = a.name.compareNatural (b.name);
        return compared != 0 ? compared < 0 : a.id < b.id;
    });
    return rows;
}

juce::String compactKeys (const ShortcutKeys& keys, int width, const std::function<int (const juce::String&)>& measure)
{
    if (keys.isEmpty()) return ShortcutDisplay::keys (keys);
    for (int count = keys.size(); count > 0; --count)
    {
        const auto text = ShortcutDisplay::keys (keys, count);
        if (measure (text) <= width) return text;
    }
    return "+" + juce::String (keys.size()) + ko ("개");
}

juce::String compactMidi (const ShortcutService& service, const MidiTriggers& triggers, int width, const std::function<int (const juce::String&)>& measure)
{
    for (int count = static_cast<int> (triggers.size()); count > 0; --count)
    {
        const auto text = ShortcutDisplay::midi (service, triggers, count);
        if (measure (text) <= width) return text;
    }
    return triggers.empty() ? ko ("미지정") : "+" + juce::String (static_cast<int> (triggers.size()));
}

ImportPreview previewImport (const ShortcutService& service, const juce::String& xml, const Cues& cues, const std::vector<MidiBinding>& midiCues)
{
    ImportPreview preview;
    const auto parsed = ShortcutProfile::parseExchange (xml);
    if (! parsed.wasOk()) { preview.error = parsed.status.getErrorMessage(); return preview; }
    preview.replacesMidi = parsed.replacesMidi;
    preview.formatKnown = true;
    const auto resolved = ShortcutService::calculateMapping (ShortcutCatalog::get(), parsed.keyboard);
    preview.applicable = resolved.wasOk();
    preview.error = resolved.getErrorMessage();
    // Do not use the incomplete mapping of a rejected file. Report all requested
    // command collisions, including ones after the first parser/mapping diagnostic.
    std::vector<std::pair<juce::String, juce::KeyPress>> explicitKeys;
    for (const auto& [id, keys] : parsed.keyboard.overrides)
        if (const auto* entry = ShortcutCatalog::get().find (id); entry != nullptr && entry->isCommand())
            for (const auto& key : keys)
            {
                for (const auto& [other, otherKey] : explicitKeys)
                    if (other != id && ShortcutKeyInput::keysOverlap (key, otherKey))
                        preview.conflicts.addIfNotAlreadyThere (ShortcutDisplay::key (key) + ": " + nameFor (other) + " / " + nameFor (id));
                explicitKeys.emplace_back (id, key);
            }
    for (const auto& entry : ShortcutCatalog::get().getCommands())
    {
        const auto found = parsed.keyboard.overrides.find (entry.id);
        const auto& next = preview.applicable ? resolved.keys.at (entry.id)
                          : found != parsed.keyboard.overrides.end() ? found->second : entry.defaultKeys;
        if (next != service.getKeys (entry.id))
            preview.changes.add (entry.name + ": " + ShortcutDisplay::keys (service.getKeys (entry.id)) + ko (" → ") + ShortcutDisplay::keys (next));
        const auto nextMidi = parsed.midi.overrides.find (entry.id);
        const bool midiEmpty = parsed.replacesMidi ? nextMidi == parsed.midi.overrides.end() || nextMidi->second.empty() : service.getMidiTriggers (entry.id).empty();
        if (next.isEmpty() && midiEmpty) preview.unassigned.add (entry.name);
        if (entry.commandID == CommandIDs::panicAll)
            preview.removesLastPanic = ! service.getKeys (entry.id).isEmpty() && next.isEmpty();
    }
    if (preview.applicable)
    {
        juce::ApplicationCommandManager manager;
        for (const auto& entry : ShortcutCatalog::get().getCommands())
        {
            juce::ApplicationCommandInfo info (entry.commandID);
            ShortcutCatalog::get().getCommandInfo (entry.commandID, info);
            manager.registerCommand (info);
        }
        ShortcutService prospective (manager, [] (const auto&, const auto&) { return juce::Result::ok(); });
        prospective.setInputStorage ([] (const auto&) { return juce::Result::ok(); });
        juce::String currentMidi;
        service.getMidiProfile().serialise (currentMidi);
        prospective.restoreMidi (currentMidi, {});
        const auto applied = prospective.importProfile (xml);
        if (applied.failed()) { preview.applicable = false; preview.error = applied.getErrorMessage(); return preview; }
        if (parsed.replacesMidi)
            for (const auto& entry : ShortcutCatalog::get().getCommands())
                if (service.getMidiTriggers (entry.id) != prospective.getMidiTriggers (entry.id))
                    preview.changes.add (entry.name + " MIDI: " + ShortcutDisplay::midi (service, service.getMidiTriggers (entry.id))
                        + ko (" → ") + ShortcutDisplay::midi (prospective, prospective.getMidiTriggers (entry.id)));
        for (const auto& row : rows (prospective, cues, MidiInputSettingsPanel::snapshot (service, nullptr), midiCues))
        {
            for (const auto& conflict : row.conflicts) preview.conflicts.add (row.name + ": " + conflict);
            for (const auto& limit : row.limitations) preview.limitations.add (row.name + ": " + limit);
            for (const auto& state : row.midiStates) preview.limitations.add (row.name + " MIDI: " + state + ko (" (입력 선택은 유지)"));
        }
    }
    return preview;
}

juce::String ImportPreview::text() const
{
    juce::String result = ! formatKnown ? ko ("가져오기 형식을 읽을 수 없습니다.\n")
        : replacesMidi ? ko ("v2 — 키보드·MIDI 모두 교체\n") : ko ("v1 — 키보드만 교체, MIDI 유지\n");
    result += ko ("활성 입력·백그라운드 옵션은 유지합니다. 장치가 없어도 매핑을 보존합니다.\n");
    if (error.isNotEmpty()) result += ko ("적용 불가: ") + error + "\n";
    if (removesLastPanic) result += ko ("키보드로 전체 정지를 할 수 없음 — 패닉 키가 모두 제거됩니다.\n");
    const auto section = [&result] (const juce::String& title, const juce::StringArray& lines)
    { result += "\n" + title + " (" + juce::String (lines.size()) + ")\n" + (lines.isEmpty() ? ko ("없음") : lines.joinIntoString ("\n")) + "\n"; };
    section (ko ("변경"), changes);
    section (ko ("미지정"), unassigned);
    section (ko ("충돌"), conflicts);
    section (ko ("제한 있음"), limitations);
    return result;
}
}

namespace
{
class LearnRow : public juce::Component
{
public:
    LearnRow()
    {
        setInterceptsMouseClicks (false, true);
        button.setButtonText (ko ("학습"));
        addAndMakeVisible (button);
    }
    void resized() override { button.setBounds (getLocalBounds().removeFromRight (58).reduced (3)); }
    juce::TextButton button;
};

class ImportAlert : public juce::AlertWindow
{
public:
    explicit ImportAlert (const ShortcutSettingsModel::ImportPreview& preview)
        : juce::AlertWindow (ko ("단축키 가져오기 미리보기"), {}, juce::MessageBoxIconType::NoIcon)
    {
        text.setMultiLine (true);
        text.setReadOnly (true);
        text.setScrollbarsShown (true);
        text.setFont (Palette::font());
        text.setText (preview.text());
        text.setSize (540, 320);
        addCustomComponent (&text);
        if (preview.applicable) addButton (ko ("교체 적용"), 1);
        addButton (ko ("취소"), 0);
    }
    ~ImportAlert() override { removeCustomComponent (0); }
private:
    juce::TextEditor text;
};
}

ShortcutSettingsTab::ShortcutSettingsTab (ShortcutService& s, ProjectDocument& d, MidiInputService* input, MidiTriggerRouter* router)
    : service (s), document (d), midiInput (input), midiRouter (router), inputPanel (s, input), table ({}, this), capture (s)
{
    for (auto* label : { &notice, &header, &detail })
    {
        label->setFont (Palette::font());
        label->setColour (juce::Label::textColourId, Palette::dimText);
        label->setMinimumHorizontalScale (1.0f);
    }
    notice.setText (ko ("이 PC에 적용됩니다. 프로젝트 큐 핫키와 별도로 저장됩니다."), juce::dontSendNotification);
    header.setInterceptsMouseClicks (false, false);
    detail.setJustificationType (juce::Justification::topLeft);
    detailView.setViewedComponent (&detail, false);
    detailView.setScrollBarsShown (true, false);
    search.setTextToShowWhenEmpty (ko ("기능명·설명·키 검색 (F13, Ctrl+Alt)"), Palette::dimText);
    search.setFont (Palette::font());
    search.onTextChange = [this] { cancelCapture(); refresh(); };
    int index = 1;
    for (const auto* name : { "전체 카테고리", "재생", "큐", "편집", "화면", "파일", "설정" }) category.addItem (ko (name), index++);
    index = 1;
    for (const auto* name : { "전체 상태", "변경됨", "미지정", "충돌", "제한 있음", "MIDI 미연결", "MIDI 준비 대기", "MIDI 사용 불가·선택 안 함", "MIDI 과부하" }) status.addItem (ko (name), index++);
    status.setTooltip (ko ("충돌·제한의 자세한 이유는 기능을 선택하면 표시됩니다. 제한 있음에는 입력창의 문자·편집 조작 우선도 포함됩니다."));
    category.setSelectedId (1, juce::dontSendNotification);
    status.setSelectedId (1, juce::dontSendNotification);
    category.onChange = status.onChange = [this] { cancelCapture(); refresh(); };
    selectedKey.onChange = [this]
    {
        const bool enabled = ! service.isEditingLocked() && selectedKey.getSelectedId() > 0;
        replaceButton.setEnabled (enabled);
        deleteButton.setEnabled (enabled);
        replaceButton.setButtonText (selectedKey.getSelectedId() >= 1000 ? ko ("MIDI 바꾸기") : ko ("키 바꾸기"));
        restoreButton.setTooltip (selectedKey.getSelectedId() >= 1000 ? ko ("MIDI만 기본값(없음)으로 복원") : ko ("키보드만 기본값으로 복원"));
    };
    table.setRowHeight (48);
    table.setColour (juce::ListBox::backgroundColourId, Palette::panel2);
    table.setColour (juce::ListBox::outlineColourId, Palette::outline);
    table.setOutlineThickness (1);
    table.setMultipleSelectionEnabled (false);
    for (auto* c : std::initializer_list<juce::Component*> { &notice, &header, &search, &category, &status, &table, &detailView, &selectedKey,
                      &replaceButton, &deleteButton, &restoreButton, &importButton, &exportButton, &resetButton }) addAndMakeVisible (c);
    addChildComponent (capture);
    addAndMakeVisible (inputPanel);
    inputPanel.onHeightChanged = [this] { cancelCapture(); resized(); };
    startTimerHz (4);
    replaceButton.setButtonText (ko ("키 바꾸기"));
    deleteButton.setButtonText (ko ("삭제"));
    restoreButton.setButtonText (ko ("이 기능 기본값"));
    importButton.setButtonText (ko ("가져오기"));
    exportButton.setButtonText (ko ("내보내기"));
    resetButton.setButtonText (ko ("전체 기본값"));
    replaceButton.onClick = [this] { learn (true); };
    deleteButton.onClick = [this] { removeKey(); };
    restoreButton.onClick = [this] { restoreSelected(); };
    importButton.onClick = [this] { importFile(); };
    exportButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem (1, ko ("키보드·MIDI 내보내기 — v2 (기본)"));
        menu.addItem (2, ko ("키보드만 내보내기 — v1"));
        const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [safe] (int choice)
        { if (safe != nullptr && choice != 0) safe->exportFile (choice == 2); });
    };
    resetButton.onClick = [this]
    {
        cancelCapture();
        juce::PopupMenu menu;
        menu.addItem (1, ko ("키보드 전체 기본값"));
        menu.addItem (2, ko ("MIDI 전체 기본값 — 모두 삭제"));
        const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&resetButton), [safe] (int choice)
        {
            if (safe == nullptr || choice == 0) return;
            safe->confirm (ko ("전체 기본값"), choice == 1 ? ko ("키보드만 기본값으로 복원합니다. MIDI는 유지합니다.")
                : ko ("이 PC의 명령 MIDI를 모두 삭제합니다. 키보드는 유지합니다."), ko ("복원"), [safe, choice]
                { if (safe != nullptr) safe->report (choice == 1 ? safe->service.restoreAllDefaults() : safe->service.replaceMidiProfile ({})); });
        });
    };
    capture.validate = [this] (const juce::KeyPress& key)
    {
        if (service.isEditingLocked()) return KeyCapture::Decision { false, ko ("쇼 모드에서는 변경할 수 없습니다.") };
        const auto* entry = ShortcutCatalog::get().find (captureID);
        if (entry != nullptr && entry->scope == ShortcutScope::application)
        {
            const auto converted = PanicKeyHook::convert (key);
            if (! converted.binding) return KeyCapture::Decision { false, converted.reason };
        }
        const auto issues = ShortcutSettingsModel::inspect (service, captureID, key, cues());
        auto messages = issues.conflicts;
        messages.addArray (issues.limitations);
        return KeyCapture::Decision { true, messages.joinIntoString ("\n") };
    };
    capture.validateMidi = [this] (const MidiTrigger& trigger)
    {
        juce::StringArray conflicts;
        for (const auto& binding : service.midiCommandBindings())
            if (binding.id != captureID && MidiTriggerRules::intersects (binding.trigger, trigger))
                conflicts.add (ShortcutCatalog::get().find (binding.id)->name + ": " + ShortcutDisplay::midi (service, { binding.trigger }));
        for (const auto& cue : midiCues())
            if (MidiTriggerRules::intersects (trigger, cue.trigger)) conflicts.add (ko ("큐 MIDI 비활성: ") + cue.id);
        return KeyCapture::Decision { true, conflicts.joinIntoString ("\n") };
    };
    capture.onSubmit = [this] (const KeyCaptureSession::Candidate& candidate, KeyCapture::Completion done)
    {
        if (const auto* key = std::get_if<juce::KeyPress> (&candidate)) registerKey (*key, std::move (done));
        else if (const auto* trigger = std::get_if<MidiTrigger> (&candidate)) registerMidi (*trigger, std::move (done));
    };
    capture.onFinished = [this]
    {
        for (auto& dialog : dialogs) if (dialog != nullptr && dialog->isCurrentlyModal()) dialog->exitModalState (0);
        capture.setVisible (false); refresh(); resized();
    };
    service.addListener (this);
    document.addListener (this);
    refresh();
}

ShortcutSettingsTab::~ShortcutSettingsTab()
{
    stopTimer();
    ++operation;
    capture.cancel (false);
    for (auto& dialog : dialogs)
        if (dialog != nullptr) delete dialog.getComponent();
    service.removeListener (this);
    document.removeListener (this);
}
ShortcutSettingsModel::Cues ShortcutSettingsTab::cues() const
{
    ShortcutSettingsModel::Cues result;
    document.forEachList ([&] (CueList& list)
    {
        for (const auto& cue : list.getAll())
            if (cue.hotkey.isNotEmpty()) result.push_back ({ cue.number + " " + cue.name,
                juce::KeyPress::createFromDescription (cue.hotkey), true, cue.armed });
    });
    return result;
}
void ShortcutSettingsTab::refresh()
{
    visibleRows = ShortcutSettingsModel::filter (ShortcutSettingsModel::rows (service, cues(), MidiInputSettingsPanel::snapshot (service, midiInput), midiCues(), midiRouter), search.getText(),
        static_cast<ShortcutSettingsModel::Category> (category.getSelectedId() - 1),
        static_cast<ShortcutSettingsModel::Status> (status.getSelectedId() - 1));
    table.updateContent();
    int selected = -1;
    for (int i = 0; i < static_cast<int> (visibleRows.size()); ++i)
        if (visibleRows[static_cast<size_t> (i)].id == selectedID) selected = i;
    if (selected < 0 && ! visibleRows.empty()) selected = 0;
    table.selectRow (selected);
    selectedID = selected >= 0 ? visibleRows[static_cast<size_t> (selected)].id : juce::String();
    showSelection();
    table.repaint();
    resized();
}
int ShortcutSettingsTab::getNumRows() { return static_cast<int> (visibleRows.size()); }
void ShortcutSettingsTab::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumRows())) return;
    const auto& entry = visibleRows[static_cast<size_t> (row)];
    g.fillAll (selected ? Palette::standby.withAlpha (0.16f) : Palette::panel2);
    auto bounds = juce::Rectangle<int> (6, 0, width - 12, height);
    bounds.removeFromRight (52);
    auto inputBounds = bounds.removeFromRight (juce::jmin (330, width * 3 / 5));
    g.setColour (Palette::text);
    g.setFont (Palette::font());
    g.drawText (entry.name + (! entry.conflicts.isEmpty() ? ko (" [충돌]") : juce::String()),
                bounds.removeFromTop (25), juce::Justification::centredLeft, true);
    g.setColour (Palette::dimText);
    g.drawText (entry.scope, bounds, juce::Justification::centredLeft, true);
    const auto measure = [] (const juce::String& value) { return juce::GlyphArrangement::getStringWidthInt (Palette::font(), value); };
    const auto keysText = ShortcutSettingsModel::compactKeys (entry.keys, inputBounds.getWidth() - 30, measure);
    g.setColour (Palette::text);
    g.drawText (ko ("키: ") + keysText, inputBounds.removeFromTop (24), juce::Justification::centredLeft, true);
    const auto midiText = ShortcutSettingsModel::compactMidi (service, entry.midi, inputBounds.getWidth() - 42, measure);
    g.setColour (! entry.midiStates.isEmpty() && ! entry.midiStates.contains (ko ("연결")) ? Palette::warn : Palette::dimText);
    g.drawText ("MIDI: " + midiText, inputBounds, juce::Justification::centredLeft, true);
}
juce::Component* ShortcutSettingsTab::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* component = dynamic_cast<LearnRow*> (existing);
    if (component == nullptr) component = new LearnRow();
    component->button.setEnabled (! service.isEditingLocked());
    const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
    const auto id = juce::isPositiveAndBelow (row, getNumRows()) ? visibleRows[static_cast<size_t> (row)].id : juce::String();
    component->button.onClick = [safe, id]
    {
        // Selecting a row refreshes/reuses its button and can replace this very
        // std::function. Keep independent copies before touching the list.
        const auto owner = safe;
        const auto action = id;
        if (owner == nullptr || action.isEmpty()) return;
        for (int i = 0; i < owner->getNumRows(); ++i)
            if (owner->visibleRows[static_cast<size_t> (i)].id == action)
            {
                owner->table.selectRow (i);
                if (owner != nullptr) owner->learn (false);
                break;
            }
    };
    return component;
}
void ShortcutSettingsTab::listBoxItemClicked (int row, const juce::MouseEvent& event)
{
    table.selectRow (row);
    if (event.x >= table.getWidth() - table.getVerticalScrollBar().getWidth() - 64) learn (false);
}
void ShortcutSettingsTab::selectedRowsChanged (int row)
{
    cancelCapture();
    selectedID = juce::isPositiveAndBelow (row, getNumRows()) ? visibleRows[static_cast<size_t> (row)].id : juce::String();
    showSelection();
}
juce::String ShortcutSettingsTab::getTooltipForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, getNumRows())) return {};
    const auto& entry = visibleRows[static_cast<size_t> (row)];
    return entry.name + ko (" · ") + entry.scope + "\n" + ShortcutDisplay::keys (entry.keys)
         + "\nMIDI: " + entry.midiText + "\n" + entry.midiStates.joinIntoString (", ")
         + "\n" + entry.conflicts.joinIntoString ("\n") + "\n" + entry.limitations.joinIntoString ("\n");
}
void ShortcutSettingsTab::showSelection()
{
    const bool learning = capture.isVisible();
    for (auto* c : std::initializer_list<juce::Component*> { &inputPanel, &table, &search, &category, &status, &header, &replaceButton, &deleteButton, &restoreButton })
        c->setVisible (! learning);
    detailView.setVisible (! learning);
    selectedKey.setVisible (! learning);
    const auto previous = selectedKey.getSelectedId();
    selectedKey.clear (juce::dontSendNotification);
    const auto& keys = service.getKeys (selectedID);
    for (int i = 0; i < keys.size(); ++i) selectedKey.addItem (ko ("키: ") + ShortcutDisplay::key (keys[i]), i + 1);
    const auto& midi = service.getMidiTriggers (selectedID);
    for (size_t i = 0; i < midi.size(); ++i) selectedKey.addItem ("MIDI: " + ShortcutDisplay::midi (service, { midi[i] }), 1000 + static_cast<int> (i));
    selectedKey.setTextWhenNothingSelected (ko ("미지정"));
    selectedKey.setSelectedId (previous >= 1000 && previous < 1000 + static_cast<int> (midi.size()) ? previous
        : ! keys.isEmpty() ? juce::jlimit (1, keys.size(), previous) : ! midi.empty() ? 1000 : 0, juce::dontSendNotification);
    replaceButton.setButtonText (selectedKey.getSelectedId() >= 1000 ? ko ("MIDI 바꾸기") : ko ("키 바꾸기"));
    const auto* entry = ShortcutCatalog::get().find (selectedID);
    juce::String text;
    if (entry != nullptr)
    {
        text = entry->name + ko (" · ") + ShortcutCatalog::scopeLabel (entry->scope)
             + ko ("\n현재 키: ") + ShortcutDisplay::keys (keys) + "\nMIDI: " + ShortcutDisplay::midi (service, midi) + "\n" + entry->description;
        for (const auto& trigger : midi) text += "\n" + ShortcutDisplay::midiDetails (service, trigger);
        for (const auto& row : visibleRows)
            if (row.id == selectedID)
                text += "\n" + row.midiStates.joinIntoString (", ") + "\n" + row.conflicts.joinIntoString ("\n") + "\n" + row.limitations.joinIntoString ("\n");
    }
    setDetail (text);
    const bool editable = ! service.isEditingLocked() && ! learning;
    replaceButton.setEnabled (editable && (! keys.isEmpty() || ! midi.empty()));
    deleteButton.setEnabled (editable && (! keys.isEmpty() || ! midi.empty()));
    restoreButton.setEnabled (editable && entry != nullptr);
    restoreButton.setTooltip (selectedKey.getSelectedId() >= 1000 ? ko ("이 기능의 MIDI만 기본값(없음)으로 복원. 키보드는 유지.")
        : ko ("이 기능의 키보드만 기본값으로 복원. MIDI는 유지."));
    importButton.setEnabled (editable);
    resetButton.setEnabled (editable);
    notice.setText (service.isEditingLocked() ? ko ("쇼 모드 — 단축키 변경 잠김")
                   : ko ("이 PC에 적용됩니다. 학습 = 키·MIDI 추가 / 항목 선택 후 바꾸기 = 교체"), juce::dontSendNotification);
}
void ShortcutSettingsTab::setDetail (const juce::String& text)
{
    detail.setText (text, juce::dontSendNotification);
    resized();
    detailView.setViewPosition (0, 0);
}
void ShortcutSettingsTab::learn (bool replace)
{
    if (service.isEditingLocked() || selectedID.isEmpty()) return;
    cancelCapture();
    captureID = selectedID;
    const int selected = selectedKey.getSelectedId();
    replacedMidi.reset();
    replaceIndex = replace && selected > 0 && selected < 1000 ? selected - 1 : -1;
    replacedKey = replaceIndex >= 0 ? service.getKeys (captureID)[replaceIndex] : juce::KeyPress();
    if (replace && selected >= 1000)
    {
        replaceIndex = selected - 1000;
        replacedMidi = service.getMidiTriggers (captureID)[static_cast<size_t> (replaceIndex)];
    }
    capture.setVisible (true);
    showSelection();
    resized();
    capture.start ((replace ? ko ("입력 바꾸기: ") : ko ("입력 추가: ")) + ShortcutCatalog::get().find (captureID)->name, false, replacedMidi);
}
void ShortcutSettingsTab::registerKey (const juce::KeyPress& key, KeyCapture::Completion done)
{
    const auto id = captureID;
    const int index = replacedMidi ? -1 : replaceIndex;
    const auto oldKey = replacedKey;
    const auto issues = ShortcutSettingsModel::inspect (service, id, key, cues());
    const auto apply = [this, id, index, oldKey, key, done, move = issues.commandOwner.isNotEmpty()]
    {
        if (index >= 0 && (! juce::isPositiveAndBelow (index, service.getKeys (id).size()) || service.getKeys (id)[index] != oldKey))
        { done (juce::Result::fail (ko ("키 목록이 바뀌었습니다. 다시 선택하세요."))); return; }
        const auto policy = move ? ShortcutService::ConflictPolicy::move : ShortcutService::ConflictPolicy::reject;
        ShortcutOperationResult result;
        { const juce::ScopedValueSetter<bool> guard (committingCapture, true);
          result = index < 0 ? service.addKey (id, key, policy) : service.replaceKey (id, index, key, policy); }
        done (result.status);
    };
    if (issues.commandOwner.isNotEmpty())
    {
        const bool lastPanic = ShortcutSettingsModel::moveRemovesLastPanic (service, id, { key });
        confirm (ko ("다른 기능에서 사용"), ShortcutDisplay::key (key) + ": "
                    + ShortcutCatalog::get().find (issues.commandOwner)->name + ko (" → ") + ShortcutCatalog::get().find (id)->name
                    + "\n" + issues.conflicts.joinIntoString ("\n")
                    + (lastPanic ? ko ("\n키보드로 전체 정지를 할 수 없음 — 마지막 패닉 키를 이동합니다.") : juce::String()),
                 ko ("기존 기능에서 이 키 이동"), apply, true, [done] { done (juce::Result::fail (ko ("이동을 취소했습니다."))); });
    }
    else apply();
}
void ShortcutSettingsTab::registerMidi (const MidiTrigger& trigger, KeyCapture::Completion done)
{
    const auto id = captureID;
    const auto original = replacedMidi;
    const int index = original ? replaceIndex : -1;
    juce::StringArray conflicts;
    for (const auto& binding : service.midiCommandBindings())
        if (binding.id != id && MidiTriggerRules::intersects (binding.trigger, trigger))
            conflicts.add (ShortcutCatalog::get().find (binding.id)->name + ": " + ShortcutDisplay::midi (service, { binding.trigger }));
    const auto apply = [this, id, trigger, original, index, done, move = ! conflicts.isEmpty()]
    {
        auto triggers = service.getMidiTriggers (id);
        if (original)
        {
            if (! juce::isPositiveAndBelow (index, static_cast<int> (triggers.size())) || triggers[static_cast<size_t> (index)] != *original)
            { done (juce::Result::fail (ko ("MIDI 목록이 바뀌었습니다. 다시 선택하세요."))); return; }
            triggers[static_cast<size_t> (index)] = trigger;
        }
        else triggers.push_back (trigger);
        ShortcutOperationResult result;
        { const juce::ScopedValueSetter<bool> guard (committingCapture, true);
          result = service.setMidiTriggers (id, std::move (triggers), move ? ShortcutService::ConflictPolicy::move : ShortcutService::ConflictPolicy::reject); }
        done (result.status);
    };
    if (conflicts.isEmpty()) apply();
    else confirm (ko ("다른 기능에서 사용"), conflicts.joinIntoString ("\n")
        + ko ("\n위 기존 소유자의 겹치는 바인딩 전체를 제거하고 이동합니다. 전체 채널·장치 범위도 분할하지 않습니다."),
        ko ("겹치는 바인딩 전체 이동"), apply, true, [done] { done (juce::Result::fail (ko ("이동을 취소했습니다."))); });
}
void ShortcutSettingsTab::removeKey()
{
    const auto id = selectedID;
    if (selectedKey.getSelectedId() >= 1000)
    {
        auto triggers = service.getMidiTriggers (id);
        const int midiIndex = selectedKey.getSelectedId() - 1000;
        if (service.isEditingLocked() || ! juce::isPositiveAndBelow (midiIndex, static_cast<int> (triggers.size()))) return;
        triggers.erase (triggers.begin() + midiIndex);
        report (service.setMidiTriggers (id, std::move (triggers)));
        return;
    }
    const int index = selectedKey.getSelectedId() - 1;
    if (index < 0 || service.isEditingLocked()) return;
    const auto apply = [this, id, index] { report (service.removeKey (id, index)); };
    if (id == "transport.panicAll" && service.getKeys (id).size() == 1) confirmPanicRemoval (apply);
    else apply();
}
void ShortcutSettingsTab::restoreSelected()
{
    const auto id = selectedID;
    if (id.isEmpty()) return;
    if (selectedKey.getSelectedId() >= 1000)
    {
        confirm (ko ("이 기능 MIDI 기본값"), ko ("이 기능의 MIDI를 모두 삭제합니다. 키보드는 유지합니다."), ko ("복원"),
            [this, id] { report (service.setMidiTriggers (id, {})); });
        return;
    }
    const auto& defaults = ShortcutCatalog::get().find (id)->defaultKeys;
    juce::StringArray conflicts;
    for (const auto& key : defaults)
    {
        const auto owner = service.resolveKeyOwner (key, {}, true);
        if (owner.commandID != 0 && owner.id != id)
            conflicts.add (ShortcutDisplay::key (key) + ": " + ShortcutCatalog::get().find (owner.id)->name);
    }
    if (! conflicts.isEmpty())
    {
        const bool removesPanic = ShortcutSettingsModel::moveRemovesLastPanic (service, id, defaults);
        confirm (ko ("다른 기능에서 사용"), conflicts.joinIntoString ("\n")
                    + (removesPanic ? ko ("\n키보드로 전체 정지를 할 수 없음 — 마지막 패닉 키를 이동합니다.") : juce::String()),
                 ko ("기존 기능에서 이 키 이동"), [this, id] { report (service.restoreCommandDefaults (id, ShortcutService::ConflictPolicy::move)); });
        return;
    }
    auto profile = service.getProfile();
    profile.overrides.erase (id);
    const auto next = ShortcutService::calculateMapping (ShortcutCatalog::get(), profile);
    const auto apply = [this, id] { report (service.restoreCommandDefaults (id)); };
    if (next.wasOk() && ! service.getKeys (CommandIDs::panicAll).isEmpty() && next.keys.at ("transport.panicAll").isEmpty())
        confirmPanicRemoval (apply);
    else apply();
}
void ShortcutSettingsTab::confirmPanicRemoval (std::function<void()> callback)
{
    confirm (ko ("마지막 패닉 키 제거"), ko ("키보드로 전체 정지를 할 수 없음\n전체 페이드 정지 버튼은 계속 사용할 수 있습니다."),
             ko ("제거"), std::move (callback));
}
void ShortcutSettingsTab::confirm (const juce::String& title, const juce::String& message, const juce::String& button,
                                   std::function<void()> apply, bool editsMapping, std::function<void()> cancelled)
{
    const auto token = ++operation;
    const auto mapping = service.getInputGeneration();
    const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
    auto* alert = new juce::AlertWindow (title, message, juce::MessageBoxIconType::WarningIcon);
    if (capture.isCapturing()) alert->getProperties().set ("inputCaptureField", true);
    dialogs.emplace_back (alert);
    alert->addButton (button, 1);
    alert->addButton (ko ("취소"), 0);
    ShortcutRouter::watchWindow (alert);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe, token, mapping, apply, editsMapping, cancelled] (int result)
    {
        if (safe != nullptr && safe->operation == token && (! editsMapping || ! safe->service.isEditingLocked())
            && safe->service.getInputGeneration() == mapping && result == 1) apply();
        else if (safe != nullptr && safe->operation == token && cancelled) cancelled();
    }), true);
}
void ShortcutSettingsTab::report (const ShortcutOperationResult& result)
{
    if (result.failed()) setDetail (ko ("변경하지 못했습니다: ") + result.getErrorMessage());
    else { refresh(); setDetail (ko ("저장했습니다.\n") + detail.getText()); }
}
void ShortcutSettingsTab::importFile()
{
    cancelCapture();
    const auto token = ++operation;
    const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
    chooser = std::make_unique<juce::FileChooser> (ko ("단축키 가져오기"), juce::File(), "*.enqueue-shortcuts.xml");
    launchMidiFileChooser (*chooser, juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe, token] (const juce::FileChooser& fileChooser)
    {
        if (safe == nullptr || safe->operation != token || safe->service.isEditingLocked()) return;
        const auto file = fileChooser.getResult();
        if (file == juce::File()) return;
        const auto xml = file.loadFileAsString();
        const auto preview = ShortcutSettingsModel::previewImport (safe->service, xml, safe->cues(), safe->midiCues());
        const auto mapping = safe->service.getInputGeneration();
        auto* alert = new ImportAlert (preview);
        safe->dialogs.emplace_back (alert);
        ShortcutRouter::watchWindow (alert);
        alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe, token, mapping, xml, panic = preview.removesLastPanic] (int result)
        {
            if (safe == nullptr || safe->operation != token || safe->service.isEditingLocked()
                || safe->service.getInputGeneration() != mapping || result != 1) return;
            const auto apply = [safe, xml] { if (safe != nullptr) safe->report (safe->service.importProfile (xml)); };
            if (panic) safe->confirmPanicRemoval (apply); else apply();
        }), true);
    });
}
void ShortcutSettingsTab::exportFile (bool keyboardOnly)
{
    cancelCapture();
    const auto token = ++operation;
    const auto xml = keyboardOnly ? service.exportProfile() : service.exportCombinedProfile();
    const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
    chooser = std::make_unique<juce::FileChooser> (ko ("단축키 내보내기"), juce::File::getCurrentWorkingDirectory().getChildFile ("keys.enqueue-shortcuts.xml"), "*.enqueue-shortcuts.xml");
    launchMidiFileChooser (*chooser, juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe, token, xml] (const juce::FileChooser& fileChooser)
    {
        if (safe == nullptr || safe->operation != token) return;
        auto file = fileChooser.getResult();
        if (file == juce::File()) return;
        if (! file.getFileName().endsWithIgnoreCase (".enqueue-shortcuts.xml"))
        {
            const auto base = file.hasFileExtension ("xml") ? file.getFileNameWithoutExtension() : file.getFileName();
            file = file.getSiblingFile (base.endsWithIgnoreCase (".enqueue-shortcuts") ? base + ".xml" : base + ".enqueue-shortcuts.xml");
        }
        const auto write = [safe, file, xml]
        {
            if (safe != nullptr)
                safe->setDetail (file.replaceWithText (xml, false, false, "\n") ? ko ("내보냈습니다: ") + file.getFullPathName() : ko ("파일을 저장하지 못했습니다."));
        };
        if (file != fileChooser.getResult() && file.existsAsFile())
            safe->confirm (ko ("파일 덮어쓰기"), file.getFullPathName(), ko ("덮어쓰기"), write, false);
        else write();
    });
}
void ShortcutSettingsTab::cancelCapture()
{
    ++operation;
    const bool wasVisible = capture.isVisible();
    capture.cancel (false);
    capture.setVisible (false);
    if (wasVisible) showSelection();
}
void ShortcutSettingsTab::shortcutsChanged() { if (! committingCapture) { cancelCapture(); refresh(); } }
void ShortcutSettingsTab::shortcutEditingLockChanged() { cancelCapture(); refresh(); }
void ShortcutSettingsTab::documentStateChanged() { if (! committingCapture) { cancelCapture(); refresh(); } }
void ShortcutSettingsTab::visibilityChanged() { if (! isShowing()) cancelCapture(); }

std::vector<MidiBinding> ShortcutSettingsTab::midiCues() const
{
    std::vector<MidiBinding> result;
    for (const auto& cue : document.getMidiTriggers()) result.push_back ({ cue.id.toString(), cue.trigger, 0, cue.armed });
    return result;
}
void ShortcutSettingsTab::timerCallback()
{
    juce::String signature;
    for (const auto& device : MidiInputSettingsPanel::snapshot (service, midiInput))
        signature += device.identifier + ":" + juce::String (static_cast<int> (device.status)) + (device.overloaded ? "!;" : ";");
    if (midiRouter != nullptr)
        for (const auto& binding : service.midiCommandBindings()) signature += midiRouter->bindingStatus (binding);
    if (signature != deviceSignature && ! capture.isCapturing())
    { deviceSignature = signature; refresh(); }
}
void ShortcutSettingsTab::resized()
{
    // Logical tab 624x514 in the fixed 640x571 settings content.
    auto area = getLocalBounds().reduced (10);
    notice.setBounds (area.removeFromTop (18));
    auto footer = area.removeFromBottom (28);
    importButton.setBounds (footer.removeFromLeft (94)); footer.removeFromLeft (6);
    exportButton.setBounds (footer.removeFromLeft (94));
    resetButton.setBounds (footer.removeFromRight (110));
    area.removeFromBottom (6);
    capture.setBounds (area);
    if (capture.isVisible()) return;
    inputPanel.setBounds (area.removeFromTop (inputPanel.preferredHeight()));
    area.removeFromTop (4);
    search.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    auto filters = area.removeFromTop (26);
    category.setBounds (filters.removeFromLeft (200)); filters.removeFromLeft (8);
    status.setBounds (filters.removeFromLeft (230));
    header.setBounds (area.removeFromTop (18));
    auto actions = area.removeFromBottom (26);
    replaceButton.setBounds (actions.removeFromLeft (100)); actions.removeFromLeft (6);
    deleteButton.setBounds (actions.removeFromLeft (64)); actions.removeFromLeft (6);
    restoreButton.setBounds (actions.removeFromLeft (122));
    area.removeFromBottom (4);
    selectedKey.setBounds (area.removeFromBottom (26));
    area.removeFromBottom (4);
    detailView.setBounds (area.removeFromBottom (inputPanel.preferredHeight() > 28 ? 32 : 54));
    const int width = juce::jmax (40, detailView.getWidth() - detailView.getScrollBarThickness());
    juce::AttributedString text;
    text.append (detail.getText(), detail.getFont());
    juce::TextLayout layout;
    layout.createLayout (text, static_cast<float> (width - 8));
    detail.setSize (width, juce::jmax (detailView.getHeight(), juce::roundToInt (layout.getHeight()) + 10));
    area.removeFromBottom (4);
    table.setBounds (area);
}
void ShortcutSettingsTab::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    if (capture.isVisible()) return;
    auto columns = header.getBounds().reduced (6, 0);
    columns.removeFromRight (table.getVerticalScrollBar().getWidth());
    g.setColour (Palette::dimText);
    g.setFont (Palette::font());
    g.drawText (ko ("학습"), columns.removeFromRight (52), juce::Justification::centred);
    g.drawText (ko ("키보드 / MIDI"), columns.removeFromRight (330), juce::Justification::centredLeft);
    g.drawText (ko ("기능 · 동작 범위"), columns, juce::Justification::centredLeft);
}
}
