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

std::vector<Row> rows (const ShortcutService& service, const Cues& cues)
{
    std::vector<Row> result;
    for (const auto& entry : ShortcutCatalog::get().getCommands())
    {
        Row row { entry.id, entry.name, entry.description, ShortcutCatalog::scopeLabel (entry.scope), categoryFor (entry),
                  service.getKeys (entry.id), service.getProfile().overrides.count (entry.id) != 0, {}, {} };
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
        if ((status == Status::changed && ! row.changed) || (status == Status::unassigned && ! row.keys.isEmpty())
            || (status == Status::conflict && row.conflicts.isEmpty()) || (status == Status::limited && row.limitations.isEmpty())) return true;
        const auto haystack = row.name + " " + row.description + " " + row.scope + " " + ShortcutDisplay::keys (row.keys);
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

ImportPreview previewImport (const ShortcutService& service, const juce::String& xml, const Cues& cues)
{
    ImportPreview preview;
    const auto parsed = ShortcutProfile::parse (xml);
    if (! parsed.wasOk()) { preview.error = parsed.message; return preview; }
    const auto resolved = ShortcutService::calculateMapping (ShortcutCatalog::get(), parsed.profile);
    preview.applicable = resolved.wasOk();
    preview.error = resolved.getErrorMessage();
    // Do not use the incomplete mapping of a rejected file. Report all requested
    // command collisions, including ones after the first parser/mapping diagnostic.
    std::vector<std::pair<juce::String, juce::KeyPress>> explicitKeys;
    for (const auto& [id, keys] : parsed.profile.overrides)
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
        const auto found = parsed.profile.overrides.find (entry.id);
        const auto& next = preview.applicable ? resolved.keys.at (entry.id)
                          : found != parsed.profile.overrides.end() ? found->second : entry.defaultKeys;
        if (next != service.getKeys (entry.id))
            preview.changes.add (entry.name + ": " + ShortcutDisplay::keys (service.getKeys (entry.id)) + ko (" → ") + ShortcutDisplay::keys (next));
        if (next.isEmpty()) preview.unassigned.add (entry.name);
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
        ShortcutService prospective (manager, {});
        prospective.restore (xml, {});
        for (const auto& row : rows (prospective, cues))
        {
            for (const auto& conflict : row.conflicts) preview.conflicts.add (row.name + ": " + conflict);
            for (const auto& limit : row.limitations) preview.limitations.add (row.name + ": " + limit);
        }
    }
    return preview;
}

juce::String ImportPreview::text() const
{
    juce::String result = ko ("이 PC의 단축키 설정을 교체합니다.\n");
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

ShortcutSettingsTab::ShortcutSettingsTab (ShortcutService& s, ProjectDocument& d)
    : service (s), document (d), table ({}, this), capture (s)
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
    for (const auto* name : { "전체 상태", "변경됨", "미지정", "충돌", "제한 있음" }) status.addItem (ko (name), index++);
    category.setSelectedId (1, juce::dontSendNotification);
    status.setSelectedId (1, juce::dontSendNotification);
    category.onChange = status.onChange = [this] { cancelCapture(); refresh(); };
    selectedKey.onChange = [this]
    {
        const bool enabled = ! service.isEditingLocked() && selectedKey.getSelectedId() > 0;
        replaceButton.setEnabled (enabled);
        deleteButton.setEnabled (enabled);
    };
    table.setRowHeight (28);
    table.setColour (juce::ListBox::backgroundColourId, Palette::panel2);
    table.setColour (juce::ListBox::outlineColourId, Palette::outline);
    table.setOutlineThickness (1);
    table.setMultipleSelectionEnabled (false);
    for (auto* c : std::initializer_list<juce::Component*> { &notice, &header, &search, &category, &status, &table, &detailView, &selectedKey,
                      &replaceButton, &deleteButton, &restoreButton, &importButton, &exportButton, &resetButton }) addAndMakeVisible (c);
    addChildComponent (capture);
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
    exportButton.onClick = [this] { exportFile(); };
    resetButton.onClick = [this]
    {
        cancelCapture();
        confirm (ko ("전체 기본값"), ko ("이 PC의 모든 명령 단축키를 기본값으로 복원합니다."), ko ("복원"),
                 [this] { report (service.restoreAllDefaults()); });
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
    capture.onRegister = [this] (const juce::KeyPress& key)
    {
        capture.setVisible (false);
        showSelection();
        registerKey (key); // preserve any save error after leaving the capture panel
    };
    capture.onFinished = [this] { if (capture.isVisible()) { capture.setVisible (false); showSelection(); } };
    service.addListener (this);
    document.addListener (this);
    refresh();
}

ShortcutSettingsTab::~ShortcutSettingsTab()
{
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
    visibleRows = ShortcutSettingsModel::filter (ShortcutSettingsModel::rows (service, cues()), search.getText(),
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
}
int ShortcutSettingsTab::getNumRows() { return static_cast<int> (visibleRows.size()); }
void ShortcutSettingsTab::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (row, getNumRows())) return;
    const auto& entry = visibleRows[static_cast<size_t> (row)];
    g.fillAll (selected ? Palette::standby.withAlpha (0.16f) : Palette::panel2);
    auto bounds = juce::Rectangle<int> (6, 0, width - 12, height);
    bounds.removeFromRight (52);
    auto keysBounds = bounds.removeFromRight (190);
    g.setColour (Palette::text);
    g.setFont (Palette::font());
    g.drawText (entry.name + (! entry.conflicts.isEmpty() ? ko (" [충돌]") : ! entry.limitations.isEmpty() ? ko (" [제한]") : juce::String())
                + ko (" · ") + entry.scope, bounds, juce::Justification::centredLeft, true);
    const auto text = ShortcutSettingsModel::compactKeys (entry.keys, keysBounds.getWidth() - 8,
        [] (const juce::String& value) { return juce::GlyphArrangement::getStringWidthInt (Palette::font(), value); });
    g.setColour (! entry.conflicts.isEmpty() || ! entry.limitations.isEmpty() ? Palette::warn : Palette::text);
    g.drawText (text, keysBounds.reduced (4, 0), juce::Justification::centredLeft, true);
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
         + "\n" + entry.conflicts.joinIntoString ("\n") + "\n" + entry.limitations.joinIntoString ("\n");
}
void ShortcutSettingsTab::showSelection()
{
    const bool learning = capture.isVisible();
    detailView.setVisible (! learning);
    selectedKey.setVisible (! learning);
    const auto previous = selectedKey.getSelectedId();
    selectedKey.clear (juce::dontSendNotification);
    const auto& keys = service.getKeys (selectedID);
    for (int i = 0; i < keys.size(); ++i) selectedKey.addItem (ShortcutDisplay::key (keys[i]), i + 1);
    selectedKey.setTextWhenNothingSelected (ko ("미지정"));
    selectedKey.setSelectedId (keys.isEmpty() ? 0 : juce::jlimit (1, keys.size(), previous), juce::dontSendNotification);
    const auto* entry = ShortcutCatalog::get().find (selectedID);
    juce::String text;
    if (entry != nullptr)
    {
        text = entry->name + ko (" · ") + ShortcutCatalog::scopeLabel (entry->scope)
             + ko ("\n현재 키: ") + ShortcutDisplay::keys (keys) + "\n" + entry->description;
        for (const auto& row : visibleRows)
            if (row.id == selectedID)
                text += "\n" + row.conflicts.joinIntoString ("\n") + "\n" + row.limitations.joinIntoString ("\n");
    }
    setDetail (text);
    const bool editable = ! service.isEditingLocked() && ! learning;
    replaceButton.setEnabled (editable && ! keys.isEmpty());
    deleteButton.setEnabled (editable && ! keys.isEmpty());
    restoreButton.setEnabled (editable && entry != nullptr);
    importButton.setEnabled (editable);
    resetButton.setEnabled (editable);
    notice.setText (service.isEditingLocked() ? ko ("쇼 모드 — 단축키 변경 잠김")
                   : ko ("이 PC에 적용됩니다. 학습 = 키 추가 / 키 선택 후 바꾸기 = 교체"), juce::dontSendNotification);
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
    replaceIndex = replace ? selectedKey.getSelectedId() - 1 : -1;
    replacedKey = replaceIndex >= 0 ? service.getKeys (captureID)[replaceIndex] : juce::KeyPress();
    capture.setVisible (true);
    showSelection();
    capture.start ((replace ? ko ("키 바꾸기: ") : ko ("키 추가: ")) + ShortcutCatalog::get().find (captureID)->name);
}
void ShortcutSettingsTab::registerKey (const juce::KeyPress& key)
{
    const auto id = captureID;
    const int index = replaceIndex;
    const auto oldKey = replacedKey;
    const auto issues = ShortcutSettingsModel::inspect (service, id, key, cues());
    const auto apply = [this, id, index, oldKey, key, move = issues.commandOwner.isNotEmpty()]
    {
        if (index >= 0 && (! juce::isPositiveAndBelow (index, service.getKeys (id).size()) || service.getKeys (id)[index] != oldKey))
        { setDetail (ko ("키 목록이 바뀌었습니다. 다시 선택하세요.")); return; }
        const auto policy = move ? ShortcutService::ConflictPolicy::move : ShortcutService::ConflictPolicy::reject;
        report (index < 0 ? service.addKey (id, key, policy) : service.replaceKey (id, index, key, policy));
    };
    if (issues.commandOwner.isNotEmpty())
    {
        const bool lastPanic = ShortcutSettingsModel::moveRemovesLastPanic (service, id, { key });
        confirm (ko ("다른 기능에서 사용"), ShortcutDisplay::key (key) + ": "
                    + ShortcutCatalog::get().find (issues.commandOwner)->name + ko (" → ") + ShortcutCatalog::get().find (id)->name
                    + "\n" + issues.conflicts.joinIntoString ("\n")
                    + (lastPanic ? ko ("\n키보드로 전체 정지를 할 수 없음 — 마지막 패닉 키를 이동합니다.") : juce::String()),
                 ko ("기존 기능에서 이 키 이동"), apply);
    }
    else apply();
}
void ShortcutSettingsTab::removeKey()
{
    const auto id = selectedID;
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
                                   std::function<void()> apply, bool editsMapping)
{
    const auto token = ++operation;
    const auto mapping = service.getInputGeneration();
    const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
    auto* alert = new juce::AlertWindow (title, message, juce::MessageBoxIconType::WarningIcon);
    dialogs.emplace_back (alert);
    alert->addButton (button, 1);
    alert->addButton (ko ("취소"), 0);
    ShortcutRouter::watchWindow (alert);
    alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe, token, mapping, apply, editsMapping] (int result)
    {
        if (safe != nullptr && safe->operation == token && (! editsMapping || ! safe->service.isEditingLocked())
            && safe->service.getInputGeneration() == mapping && result == 1) apply();
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
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe, token] (const juce::FileChooser& fileChooser)
    {
        if (safe == nullptr || safe->operation != token || safe->service.isEditingLocked()) return;
        const auto file = fileChooser.getResult();
        if (file == juce::File()) return;
        const auto xml = file.loadFileAsString();
        const auto preview = ShortcutSettingsModel::previewImport (safe->service, xml, safe->cues());
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
void ShortcutSettingsTab::exportFile()
{
    cancelCapture();
    const auto token = ++operation;
    const auto xml = service.exportProfile();
    const juce::Component::SafePointer<ShortcutSettingsTab> safe (this);
    chooser = std::make_unique<juce::FileChooser> (ko ("단축키 내보내기"), juce::File::getCurrentWorkingDirectory().getChildFile ("keys.enqueue-shortcuts.xml"), "*.enqueue-shortcuts.xml");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
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
void ShortcutSettingsTab::shortcutsChanged() { cancelCapture(); refresh(); }
void ShortcutSettingsTab::shortcutEditingLockChanged() { cancelCapture(); refresh(); }
void ShortcutSettingsTab::documentStateChanged() { cancelCapture(); refresh(); }
void ShortcutSettingsTab::visibilityChanged() { if (! isShowing()) cancelCapture(); }

void ShortcutSettingsTab::resized()
{
    // Tab content: 624 x 514, inset 10. Footer is always reserved first.
    auto area = getLocalBounds().reduced (10);
    notice.setBounds (area.removeFromTop (18));
    search.setBounds (area.removeFromTop (28));
    area.removeFromTop (4);
    auto filters = area.removeFromTop (28);
    category.setBounds (filters.removeFromLeft (210));
    filters.removeFromLeft (8);
    status.setBounds (filters.removeFromLeft (180));
    area.removeFromTop (4);
    header.setBounds (area.removeFromTop (20));
    auto footer = area.removeFromBottom (28);
    importButton.setBounds (footer.removeFromLeft (94));
    footer.removeFromLeft (6);
    exportButton.setBounds (footer.removeFromLeft (94));
    resetButton.setBounds (footer.removeFromRight (110));
    area.removeFromBottom (6);
    auto actions = area.removeFromBottom (28);
    replaceButton.setBounds (actions.removeFromLeft (100));
    actions.removeFromLeft (6);
    deleteButton.setBounds (actions.removeFromLeft (64));
    actions.removeFromLeft (6);
    restoreButton.setBounds (actions.removeFromLeft (122));
    area.removeFromBottom (4);
    auto selection = area.removeFromBottom (96);
    capture.setBounds (selection);
    selectedKey.setBounds (selection.removeFromBottom (26));
    selection.removeFromBottom (4);
    detailView.setBounds (selection);
    const int width = juce::jmax (40, selection.getWidth() - detailView.getScrollBarThickness());
    juce::AttributedString text;
    text.append (detail.getText(), detail.getFont());
    juce::TextLayout layout;
    layout.createLayout (text, static_cast<float> (width - 8));
    detail.setSize (width, juce::jmax (selection.getHeight(), juce::roundToInt (layout.getHeight()) + 10));
    area.removeFromBottom (4);
    table.setBounds (area);
}
void ShortcutSettingsTab::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);
    auto columns = header.getBounds().reduced (6, 0);
    columns.removeFromRight (table.getVerticalScrollBar().getWidth());
    g.setColour (Palette::dimText);
    g.setFont (Palette::font());
    g.drawText (ko ("학습"), columns.removeFromRight (52), juce::Justification::centred);
    g.drawText (ko ("현재 키"), columns.removeFromRight (190), juce::Justification::centredLeft);
    g.drawText (ko ("기능 · 동작 범위"), columns, juce::Justification::centredLeft);
}
}
