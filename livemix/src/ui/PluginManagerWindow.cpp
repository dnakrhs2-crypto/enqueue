#include "ui/PluginManagerWindow.h"
#include "PluginScan.h"
#include "ui/PluginScanListComponent.h"

#include "PluginSearch.h"
#include "Widgets.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace gocue::livemix
{

namespace
{
    void styleTable (juce::TableListBox& table)
    {
        table.setHeaderHeight (30);
        table.setRowHeight (30);
        table.setColour (juce::ListBox::backgroundColourId, Palette::card);
        table.setColour (juce::ListBox::outlineColourId, Palette::line);
        table.setOutlineThickness (1);
        auto& header = table.getHeader();
        header.setColour (juce::TableHeaderComponent::backgroundColourId, Palette::card2);
        header.setColour (juce::TableHeaderComponent::textColourId, Palette::dimText);
        header.setColour (juce::TableHeaderComponent::outlineColourId, Palette::line);
        header.setColour (juce::TableHeaderComponent::highlightColourId, Palette::card2);
        header.setStretchToFitActive (true);
    }

    void styleList (juce::ListBox& list)
    {
        list.setRowHeight (28);
        list.setColour (juce::ListBox::backgroundColourId, Palette::card);
        list.setColour (juce::ListBox::outlineColourId, Palette::line);
        list.setOutlineThickness (1);
    }

    PluginSlotState slotFor (const juce::PluginDescription& d)
    {
        PluginSlotState s;
        s.format = d.pluginFormatName;
        s.name = d.name;
        s.fileOrIdentifier = d.fileOrIdentifier;
        s.uniqueId = d.uniqueId;

        if (const auto xml = d.createXml())
            s.descriptionXml = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

        return s;
    }

    juce::String formatLabel (const juce::String& formatName)
    {
        return pluginFormatLabel (formatName);
    }

    constexpr int columnFlags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable;

    /** The preset builder's own window (not modal: the manager stays usable). Closing it - the title bar, Esc -
        tells the manager, which deletes it. */
    class BuilderWindow : public juce::DialogWindow
    {
    public:
        BuilderWindow (const juce::String& title, std::function<void()> closed)
            : DialogWindow (title, Palette::card, true, true), onClose (std::move (closed)) {}

        void closeButtonPressed() override
        {
            if (onClose)
                onClose();
        }

        /** Esc closes it the same way (DialogWindow's own Esc handling would only hide it, leaving it to reopen invisible). */
        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress (juce::KeyPress::escapeKey))
            {
                closeButtonPressed();
                return true;
            }

            return DialogWindow::keyPressed (key);
        }

    private:
        std::function<void()> onClose;
    };
}

//==============================================================================
/** 새 프리셋 만들기: the enabled plugins on the left, the chain being put together on the right (1. 2. 3. ...). */
class PresetBuilder : public juce::Component
{
public:
    PresetBuilder (juce::Array<juce::PluginDescription> types, std::function<juce::Result (const PluginPreset&)> onSave, std::function<void()> onCancel)
        : allAvailable (std::move (types)), available (allAvailable), save (std::move (onSave)), cancel (std::move (onCancel)),
          availableModel (*this, false), chosenModel (*this, true)
    {
        styleCaption (nameCaption, ko ("프리셋 이름"));
        addAndMakeVisible (nameCaption);
        nameEditor.setFont (bodyFont());
        nameEditor.setTextToShowWhenEmpty (ko ("예: 보컬 기본, 스트리밍 마이크"), Palette::dimText);
        addAndMakeVisible (nameEditor);

        styleCaption (availableCaption, ko ("쓸 수 있는 플러그인 (더블클릭으로 추가)"));
        addAndMakeVisible (availableCaption);
        searchEditor.setFont (bodyFont (13.5f));
        searchEditor.setTextToShowWhenEmpty (ko ("검색: 이름, 제조사, 형식"), Palette::dimText);
        searchEditor.onTextChange = [this] { applyFilter(); };
        searchEditor.onEscapeKey = [this] { if (searchEditor.getText().isNotEmpty()) searchEditor.setText ({}, true); else if (cancel) cancel(); };   // setText sends the change (clear() does not: the list would stay filtered)
        addAndMakeVisible (searchEditor);
        styleCaption (chosenCaption, ko ("프리셋의 체인 (위에서 아래 순서로 통과)"));
        addAndMakeVisible (chosenCaption);

        availableList.setModel (&availableModel);
        styleList (availableList);
        addAndMakeVisible (availableList);
        chosenList.setModel (&chosenModel);
        styleList (chosenList);
        addAndMakeVisible (chosenList);

        auto button = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> fn)
        {
            b.setButtonText (text);
            b.setWantsKeyboardFocus (false);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };

        button (addButton, ko ("추가 ▶"), [this] { addSelected(); });
        button (removeButton, ko ("◀ 빼기"), [this] { removeSelected(); });
        button (upButton, ko ("▲ 위로"), [this] { moveSelected (-1); });
        button (downButton, ko ("▼ 아래로"), [this] { moveSelected (1); });
        button (saveButton, ko ("프리셋 저장"), [this] { commit(); });
        saveButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
        saveButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button (cancelButton, ko ("취소"), [this] { if (cancel) cancel(); });

        statusLabel.setFont (bodyFont (13.0f));
        statusLabel.setColour (juce::Label::textColourId, Palette::danger);
        addAndMakeVisible (statusLabel);

        setSize (760, 520);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 16);
        auto top = area.removeFromTop (30);
        nameCaption.setBounds (top.removeFromLeft (90));
        nameEditor.setBounds (top);
        area.removeFromTop (12);

        auto bottom = area.removeFromBottom (32);
        saveButton.setBounds (bottom.removeFromRight (130));
        bottom.removeFromRight (8);
        cancelButton.setBounds (bottom.removeFromRight (90));
        statusLabel.setBounds (bottom);
        area.removeFromBottom (12);

        // the available plugins | the buttons | the chain being built
        const int buttonsW = 110;
        auto left = area.removeFromLeft ((area.getWidth() - buttonsW) / 2);
        auto middle = area.removeFromLeft (buttonsW).withTrimmedTop (26);
        auto right = area;

        availableCaption.setBounds (left.removeFromTop (22));
        searchEditor.setBounds (left.removeFromTop (28));
        left.removeFromTop (6);
        availableList.setBounds (left);
        chosenCaption.setBounds (right.removeFromTop (22));
        chosenList.setBounds (right);

        middle = middle.withSizeKeepingCentre (96, 4 * 34 + 3 * 8);
        addButton.setBounds (middle.removeFromTop (34));
        middle.removeFromTop (8);
        removeButton.setBounds (middle.removeFromTop (34));
        middle.removeFromTop (8);
        upButton.setBounds (middle.removeFromTop (34));
        middle.removeFromTop (8);
        downButton.setBounds (middle.removeFromTop (34));
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    /** The plugins offered changed (a scan, the VST2 switch, a '사용' switch): the list follows, the chain being built stays. */
    void setTypes (juce::Array<juce::PluginDescription> types)
    {
        allAvailable = std::move (types);
        applyFilter();
    }

private:
    struct Model : public juce::ListBoxModel
    {
        Model (PresetBuilder& o, bool right) : owner (o), chosenSide (right) {}

        int getNumRows() override { return chosenSide ? (int) owner.chosen.size() : owner.available.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            g.fillAll (selected ? Palette::accent.withAlpha (0.35f) : (row % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f)));
            g.setColour (Palette::text);
            g.setFont (bodyFont (13.5f));
            juce::String text;

            if (chosenSide && row < (int) owner.chosen.size())
                text = juce::String (row + 1) + ".  " + owner.chosen[(size_t) row].name + "   (" + formatLabel (owner.chosen[(size_t) row].pluginFormatName) + ")";
            else if (! chosenSide && row < owner.available.size())
                text = owner.available[row].name + "   " + owner.available[row].manufacturerName + "  (" + formatLabel (owner.available[row].pluginFormatName) + ")";

            g.drawText (text, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            if (chosenSide)
            {
                owner.chosenList.selectRow (row);
                owner.removeSelected();
            }
            else
            {
                owner.availableList.selectRow (row);
                owner.addSelected();
            }
        }

        PresetBuilder& owner;
        bool chosenSide;
    };

    /** The available list shows the plugins the search matches (pluginMatchesSearch: name, maker, format; all of them when it is empty). */
    void applyFilter()
    {
        const int selectedRow = availableList.getSelectedRow();
        const auto selectedKey = selectedRow >= 0 && selectedRow < available.size() ? PluginHost::keyFor (available[selectedRow]) : juce::String();
        available = filterPlugins (allAvailable, searchEditor.getText());
        availableList.deselectAllRows();

        if (selectedKey.isNotEmpty())   // the same plugin stays selected when it is still shown (a refresh from a scan or a switch)
            for (int i = 0; i < available.size(); ++i)
                if (PluginHost::keyFor (available[i]) == selectedKey)
                {
                    availableList.selectRow (i);
                    break;
                }

        availableList.updateContent();
        availableList.repaint();
    }

    void addSelected()
    {
        const int row = availableList.getSelectedRow();

        if (row < 0 || row >= available.size())
            return;

        if ((int) chosen.size() >= PluginPreset::maxPlugins)
        {
            statusLabel.setText (ko ("프리셋에는 플러그인을 ") + juce::String (PluginPreset::maxPlugins) + ko ("개까지 넣을 수 있습니다."), juce::dontSendNotification);
            return;
        }

        chosen.push_back (available[row]);
        chosenList.updateContent();
        chosenList.selectRow ((int) chosen.size() - 1);
        statusLabel.setText ({}, juce::dontSendNotification);
    }

    void removeSelected()
    {
        const int row = chosenList.getSelectedRow();

        if (row < 0 || row >= (int) chosen.size())
            return;

        chosen.erase (chosen.begin() + row);
        chosenList.updateContent();
        chosenList.selectRow (juce::jmin (row, (int) chosen.size() - 1));
        chosenList.repaint();
    }

    void moveSelected (int delta)
    {
        const int row = chosenList.getSelectedRow();
        const int to = row + delta;

        if (row < 0 || row >= (int) chosen.size() || to < 0 || to >= (int) chosen.size())
            return;

        std::swap (chosen[(size_t) row], chosen[(size_t) to]);
        chosenList.updateContent();
        chosenList.selectRow (to);
        chosenList.repaint();
    }

    void commit()
    {
        const auto name = nameEditor.getText().trim();

        if (name.isEmpty())
        {
            statusLabel.setText (ko ("프리셋 이름을 넣으세요."), juce::dontSendNotification);
            nameEditor.grabKeyboardFocus();
            return;
        }

        if (chosen.empty())
        {
            statusLabel.setText (ko ("플러그인을 하나 이상 넣으세요."), juce::dontSendNotification);
            return;
        }

        PluginPreset preset;
        preset.name = name;

        for (const auto& d : chosen)
            preset.plugins.push_back (slotFor (d));

        if (save)
            if (const auto result = save (preset); result.failed())
                statusLabel.setText (result.getErrorMessage(), juce::dontSendNotification);   // here, where the operator is looking
    }

    juce::Array<juce::PluginDescription> allAvailable, available;   // every enabled plugin; the ones the search shows
    std::vector<juce::PluginDescription> chosen;
    std::function<juce::Result (const PluginPreset&)> save;
    std::function<void()> cancel;
    Model availableModel, chosenModel;

    juce::Label nameCaption, availableCaption, chosenCaption, statusLabel;
    juce::TextEditor nameEditor, searchEditor;
    juce::ListBox availableList, chosenList;
    juce::TextButton addButton, removeButton, upButton, downButton, saveButton, cancelButton;
};

//==============================================================================
class PluginManagerWindow::Content : public juce::Component,
                                     private juce::ChangeListener,
                                     private juce::Timer
{
public:
    Content (PluginManagerWindow& w, PluginHost& h, LiveMixSettings& s, juce::File folder)
        : owner (w), host (h), settings (s), presetsFolder (std::move (folder)), pluginModel (*this), presetModel (*this)
    {
        styleCaption (pluginsCaption, ko ("플러그인"));
        addAndMakeVisible (pluginsCaption);

        vst2Toggle.setButtonText (ko ("VST2 플러그인 사용 (기본 꺼짐)"));
        vst2Toggle.setToggleState (host.isVst2Enabled(), juce::dontSendNotification);
        vst2Toggle.setEnabled (PluginHost::hasVst2Support());
        vst2Toggle.setWantsKeyboardFocus (false);
        vst2Toggle.onClick = [this] { applyVst2Switch (vst2Toggle.getToggleState()); };
        addAndMakeVisible (vst2Toggle);

        styleCaption (note, PluginHost::hasVst2Support()
                                ? ko ("'사용'을 끈 플러그인은 '+ 추가' 메뉴에서 빠집니다. VST3가 기본이고, 옛 VST2 플러그인이 꼭 필요할 때만 VST2를 켜세요.")
                                : ko ("'사용'을 끈 플러그인은 '+ 추가' 메뉴에서 빠집니다. (이 빌드에는 VST2 지원이 없습니다)"));
        note.setFont (bodyFont (12.5f));
        note.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (note);

        // the search: the table shows the plugins it matches (every word: name, maker, format); Esc clears it
        pluginSearch.setFont (bodyFont (13.5f));
        pluginSearch.setTextToShowWhenEmpty (ko ("검색: 이름, 제조사, 형식 (예: waves comp, vst2)"), Palette::dimText);
        pluginSearch.onTextChange = [this] { applyPluginSearch(); };
        pluginSearch.onEscapeKey = [this] { pluginSearch.setText ({}, true); };   // with the change message: the table shows everything again
        addAndMakeVisible (pluginSearch);
        styleCaption (pluginCount, "");
        pluginCount.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (pluginCount);

        pluginTable.setModel (&pluginModel);
        styleTable (pluginTable);
        auto& header = pluginTable.getHeader();
        header.addColumn (ko ("사용"), colEnabled, 56, 56, 56, juce::TableHeaderComponent::visible);
        header.addColumn (ko ("이름"), colName, 220, 100, 500, columnFlags);
        header.addColumn (ko ("제조사"), colMaker, 150, 60, 300, columnFlags);
        header.addColumn (ko ("형식"), colFormat, 60, 50, 80, columnFlags);
        header.addColumn (ko ("파일"), colFile, 300, 100, 900, columnFlags);
        addAndMakeVisible (pluginTable);

        auto button = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> fn)
        {
            b.setButtonText (text);
            b.setWantsKeyboardFocus (false);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };

        button (scanVst3Button, ko ("VST3 스캔"), [this] { scan ("VST3"); });
        button (scanVst2Button, ko ("VST2 스캔"), [this] { scan ("VST"); });
        button (removeButton, ko ("목록에서 빼기"), [this] { removeSelectedPlugin(); });
        button (enableAllButton, ko ("전부 사용"), [this] { setAll (true); });
        button (resetListButton, ko ("목록 초기화"), [this] { resetList(); });

        auto isolatedScanner = std::make_unique<PluginScanCoordinator>();
        scanCoordinator = isolatedScanner.get();
        host.getKnownPlugins().setCustomScanner (std::move (isolatedScanner));

        // JUCE drives progress/cancellation; plugin code runs on the worker's message thread.
        scanner = std::make_unique<PluginScanListComponent> (host.getFormatManager(), host.getKnownPlugins(),
                                                            crashMarker, *scanCoordinator);
        addChildComponent (*scanner);

        styleCaption (presetsCaption, ko ("플러그인 프리셋"));
        addAndMakeVisible (presetsCaption);
        styleCaption (presetsNote, ko ("플러그인을 순서대로 골라 프리셋으로 두면, 어느 체인에서든 '+ 추가 > 프리셋 불러오기'로 한 번에 넣습니다. 체인 서랍의 '+ 추가' 메뉴에서 지금 체인을(설정째) 프리셋으로 저장할 수도 있습니다. 파일: ") + presetsFolder.getFullPathName());
        presetsNote.setFont (bodyFont (12.5f));
        presetsNote.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (presetsNote);

        presetTable.setModel (&presetModel);
        styleTable (presetTable);
        auto& ph = presetTable.getHeader();
        ph.addColumn (ko ("이름"), colPresetName, 200, 80, 400, columnFlags);
        ph.addColumn (ko ("플러그인"), colPresetChain, 500, 100, 1200, columnFlags);
        addAndMakeVisible (presetTable);

        button (newPresetButton, ko ("새 프리셋 만들기..."), [this] { newPreset(); });
        newPresetButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
        newPresetButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button (renamePresetButton, ko ("이름 바꾸기..."), [this] { renamePreset(); });
        button (deletePresetButton, ko ("삭제"), [this] { deletePreset(); });
        button (exportPresetButton, ko ("파일로 저장..."), [this] { exportPreset(); });
        button (importPresetButton, ko ("파일에서 불러오기..."), [this] { importPreset(); });
        button (openFolderButton, ko ("폴더 열기"), [this] { presetsFolder.createDirectory(); presetsFolder.revealToUser(); });

        statusLabel.setFont (bodyFont (13.0f));
        statusLabel.setColour (juce::Label::textColourId, Palette::dimText);
        statusLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (statusLabel);

        host.getKnownPlugins().addChangeListener (this);
        refreshPlugins();
        refreshPresets();
        setSize (960, 760);
    }

    ~Content() override
    {
        stopTimer();
        host.getKnownPlugins().removeChangeListener (this);
        closeBuilder();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 16);
        auto top = area.removeFromTop (24);
        pluginsCaption.setBounds (top.removeFromLeft (120));
        vst2Toggle.setBounds (top.removeFromRight (280));
        area.removeFromTop (2);
        note.setBounds (area.removeFromTop (34));
        area.removeFromTop (6);
        auto searchRow = area.removeFromTop (28);
        pluginCount.setBounds (searchRow.removeFromRight (juce::jmin (240, searchRow.getWidth() / 3)));
        searchRow.removeFromRight (8);
        pluginSearch.setBounds (searchRow.removeFromLeft (juce::jmin (420, searchRow.getWidth())));
        area.removeFromTop (6);

        auto bottom = area.removeFromBottom (30);
        statusLabel.setBounds (bottom);
        area.removeFromBottom (6);

        const int presetsH = juce::jmax (150, area.getHeight() * 2 / 5);
        auto presetsArea = area.removeFromBottom (presetsH);
        area.removeFromBottom (14);

        auto pluginButtons = area.removeFromBottom (32);
        scanVst3Button.setBounds (pluginButtons.removeFromLeft (100));
        pluginButtons.removeFromLeft (8);
        scanVst2Button.setBounds (pluginButtons.removeFromLeft (100));
        pluginButtons.removeFromLeft (8);
        enableAllButton.setBounds (pluginButtons.removeFromLeft (90));
        pluginButtons.removeFromLeft (8);
        resetListButton.setBounds (pluginButtons.removeFromLeft (100));
        removeButton.setBounds (pluginButtons.removeFromRight (120));
        area.removeFromBottom (8);
        pluginTable.setBounds (area);

        presetsCaption.setBounds (presetsArea.removeFromTop (22));
        presetsNote.setBounds (presetsArea.removeFromTop (34));
        presetsArea.removeFromTop (6);
        const bool twoRows = presetsArea.getWidth() < 720;   // a narrow window: the file actions on a row of their own
        auto presetButtons = presetsArea.removeFromBottom (32);
        newPresetButton.setBounds (presetButtons.removeFromLeft (150));
        presetButtons.removeFromLeft (8);
        renamePresetButton.setBounds (presetButtons.removeFromLeft (110));
        presetButtons.removeFromLeft (8);
        deletePresetButton.setBounds (presetButtons.removeFromLeft (64));
        auto fileRow = presetButtons;

        if (twoRows)
        {
            presetsArea.removeFromBottom (6);
            fileRow = presetsArea.removeFromBottom (32);
        }

        openFolderButton.setBounds (fileRow.removeFromRight (90));
        fileRow.removeFromRight (8);
        importPresetButton.setBounds (fileRow.removeFromRight (140));
        fileRow.removeFromRight (8);
        exportPresetButton.setBounds (fileRow.removeFromRight (110));
        presetsArea.removeFromBottom (8);
        presetTable.setBounds (presetsArea);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    /** The builder's window goes with the manager (closed, or the app quitting). */
    void closeBuilder()
    {
        if (builderWindow != nullptr)
            delete builderWindow.getComponent();
    }

    void focusSearch()
    {
        juce::Component::SafePointer<juce::TextEditor> editor (&pluginSearch);
        juce::MessageManager::callAsync ([editor] { if (editor != nullptr && editor->isShowing()) editor->grabKeyboardFocus(); });
    }

    void refreshPresets()
    {
        // the selection stays on the same preset (by file), never on the row number: a restore or an import can put
        // a new name before it, and rename / delete act on what is selected
        const auto* selected = selectedPreset();
        const auto selectedFile = selected != nullptr ? selected->file : juce::File();
        juce::StringArray problems;
        presets = PluginPreset::listFolder (presetsFolder, &problems);
        presetTable.updateContent();
        presetTable.deselectAllRows();

        if (selectedFile != juce::File())
            for (size_t i = 0; i < presets.size(); ++i)
                if (presets[i].file == selectedFile)
                {
                    presetTable.selectRow ((int) i);
                    break;
                }

        presetTable.repaint();
        presetButtonsEnabled();

        if (! problems.isEmpty())
            setStatus (ko ("읽을 수 없는 프리셋 파일: ") + problems.joinIntoString (" / "), true);
    }

private:
    enum { colEnabled = 1, colName, colMaker, colFormat, colFile };
    enum { colPresetName = 1, colPresetChain };

    struct PluginModel : public juce::TableListBoxModel
    {
        explicit PluginModel (Content& o) : owner (o) {}
        int getNumRows() override { return owner.types.size(); }

        void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
        {
            g.fillAll (selected ? Palette::accent.withAlpha (0.35f) : (row % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f)));
        }

        void paintCell (juce::Graphics& g, int row, int columnId, int width, int height, bool) override
        {
            if (row < 0 || row >= owner.types.size())
                return;

            const auto& d = owner.types[row];
            const bool switchedOn = ! owner.host.isPluginSwitchedOff (d);   // the box: this plugin's own switch
            const bool enabled = owner.host.isPluginEnabled (d);           // the text: offered in the menus (its format on too)

            if (columnId == colEnabled)
            {
                const auto box = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ (float) width * 0.5f, (float) height * 0.5f });
                g.setColour (switchedOn ? (enabled ? Palette::accent : Palette::dimText) : Palette::line);
                g.drawRoundedRectangle (box, 4.0f, 1.5f);

                if (switchedOn)
                {
                    g.setColour (enabled ? Palette::accent : Palette::dimText);
                    juce::Path tick;
                    tick.startNewSubPath (box.getX() + 4.0f, box.getCentreY());
                    tick.lineTo (box.getCentreX() - 1.0f, box.getBottom() - 5.0f);
                    tick.lineTo (box.getRight() - 4.0f, box.getY() + 5.0f);
                    g.strokePath (tick, juce::PathStrokeType (2.2f));
                }

                return;
            }

            juce::String text;

            switch (columnId)
            {
                case colName:   text = d.name; break;
                case colMaker:  text = d.manufacturerName; break;
                case colFormat: text = formatLabel (d.pluginFormatName); break;
                case colFile:   text = d.fileOrIdentifier; break;
                default: break;
            }

            g.setColour (enabled ? Palette::text : Palette::dimText);
            g.setFont (bodyFont (13.5f));
            g.drawText (text, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void cellClicked (int row, int columnId, const juce::MouseEvent&) override
        {
            if (columnId == colEnabled && row >= 0 && row < owner.types.size())
            {
                const auto& d = owner.types[row];
                owner.host.setPluginEnabled (d, owner.host.isPluginSwitchedOff (d));   // its own switch, whatever its format's switch says
                owner.settings.setDisabledPlugins (owner.host.getDisabledPlugins());
                owner.pluginTable.repaint();
                owner.refreshBuilder();
            }
        }

        void selectedRowsChanged (int) override { owner.removeButton.setEnabled (owner.pluginTable.getSelectedRow() >= 0); }

        Content& owner;
    };

    struct PresetModel : public juce::TableListBoxModel
    {
        explicit PresetModel (Content& o) : owner (o) {}
        int getNumRows() override { return (int) owner.presets.size(); }

        void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
        {
            g.fillAll (selected ? Palette::accent.withAlpha (0.35f) : (row % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f)));
        }

        void paintCell (juce::Graphics& g, int row, int columnId, int width, int height, bool) override
        {
            if (row < 0 || row >= (int) owner.presets.size())
                return;

            const auto& p = owner.presets[(size_t) row];
            g.setColour (Palette::text);
            g.setFont (bodyFont (13.5f));
            g.drawText (columnId == colPresetName ? p.name : p.summary(), 8, 0, width - 16, height, juce::Justification::centredLeft, true);
        }

        void selectedRowsChanged (int) override { owner.presetButtonsEnabled(); }
        void cellDoubleClicked (int row, int, const juce::MouseEvent&) override { owner.presetTable.selectRow (row); owner.renamePreset(); }

        Content& owner;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refreshPlugins(); }

    /** Runs while JUCE's scanner is up: when it is gone (done or cancelled), the status line says what is known now. */
    void timerCallback() override
    {
        if (scanner->isScanning())
            return;

        stopTimer();
        refreshPlugins();
        const auto skipped = scanCoordinator->getSkippedMessage();
        setStatus (skipped.isNotEmpty() ? skipped
                                       : ko ("스캔 끝: 플러그인 ") + juce::String (allTypes.size()) + ko ("개 (스캔 창에서 고른 폴더 기준)"),
                   skipped.isNotEmpty());
        statusLabel.setTooltip (skipped);
    }

    void refreshPlugins()
    {
        allTypes = host.getAllEffectTypes();
        scanVst2Button.setEnabled (PluginHost::hasVst2Support());   // never a dead button: it switches VST2 on itself
        applyPluginSearch();
        refreshBuilder();
    }

    /** An open builder offers what the menus offer now (a scan, the VST2 switch, a '사용' switch changed it). */
    void refreshBuilder()
    {
        if (builderWindow != nullptr)
            if (auto* builder = dynamic_cast<PresetBuilder*> (builderWindow->getContentComponent()))
                builder->setTypes (host.getEffectTypes());
    }

    bool isSearching() const { return pluginSearch.getText().trim().isNotEmpty(); }   // a query that happens to match everything is still a search

    /** The table shows the plugins the search box matches. The selection stays on the same plugin when it is still
        shown (a row number would point at another plugin once rows are hidden), otherwise nothing is selected. */
    void applyPluginSearch()
    {
        const int selectedRow = pluginTable.getSelectedRow();
        const auto selectedKey = selectedRow >= 0 && selectedRow < types.size() ? PluginHost::keyFor (types[selectedRow]) : juce::String();
        types = filterPlugins (allTypes, pluginSearch.getText());
        pluginTable.updateContent();
        pluginTable.deselectAllRows();

        if (selectedKey.isNotEmpty())
            for (int i = 0; i < types.size(); ++i)
                if (PluginHost::keyFor (types[i]) == selectedKey)
                {
                    pluginTable.selectRow (i);
                    break;
                }

        pluginTable.repaint();
        removeButton.setEnabled (pluginTable.getSelectedRow() >= 0);
        pluginCount.setText (! isSearching() ? ko ("플러그인 ") + juce::String (allTypes.size()) + ko ("개")
                                             : ko ("검색 결과 ") + juce::String (types.size()) + ko ("개 / 전체 ") + juce::String (allTypes.size()) + ko ("개"),
                             juce::dontSendNotification);
    }

    void presetButtonsEnabled()
    {
        const bool have = presetTable.getSelectedRow() >= 0 && presetTable.getSelectedRow() < (int) presets.size();
        renamePresetButton.setEnabled (have);
        deletePresetButton.setEnabled (have);
        exportPresetButton.setEnabled (have);
    }

    void setStatus (const juce::String& text, bool error)
    {
        statusLabel.setColour (juce::Label::textColourId, error ? Palette::danger : Palette::dimText);
        statusLabel.setText (text, juce::dontSendNotification);
    }

    /** The switch's work: stored, applied to the host, the table and the owner told. */
    void applyVst2Switch (bool on)
    {
        vst2Toggle.setToggleState (on, juce::dontSendNotification);
        settings.setVst2Enabled (on);
        host.setVst2Enabled (on);
        refreshPlugins();
        setStatus (on ? ko ("VST2 플러그인을 씁니다. 'VST2 스캔'으로 찾으세요.") : ko ("VST2 플러그인은 목록과 메뉴에서 빠집니다 (세션에 든 VST2 자리는 비워 둡니다)."), false);

        if (owner.onVst2Changed)
            owner.onVst2Changed (on);
    }

    void scan (const juce::String& formatName)
    {
        if (formatName == "VST" && ! host.isVst2Enabled())
        {
            if (! PluginHost::hasVst2Support())
            {
                setStatus (ko ("이 빌드에는 VST2 지원이 없습니다."), true);
                return;
            }

            applyVst2Switch (true);   // asking for a VST2 scan is asking for VST2: the switch goes on, then the scan
        }

        auto* format = host.getFormat (formatName);   // VST2 is registered by the switch, so it is here once that is on

        if (format == nullptr)
        {
            setStatus (ko ("이 빌드에는 그 형식이 없습니다: ") + formatLabel (formatName), true);
            return;
        }

        if (scanner->isScanning())
        {
            setStatus (ko ("이미 찾는 중입니다 - 스캔 창을 먼저 끝내세요."), true);
            return;
        }

        scanCoordinator->prepareForScan (host.getKnownPlugins(), crashMarker);
        statusLabel.setTooltip ({});
        setStatus (formatLabel (formatName) + ko (" 플러그인을 찾는 중... (창이 뜹니다)"), false);
        scanner->scanFor (*format);
        startTimer (250);
    }

    /** '목록 초기화': the scanned list, the files a crash put on the skip list and the crash marker all go, so the next
        scan looks at every file afresh (a rescan alone skips files already in the list). The '사용' switches and every
        chain stay as they are: a chain keeps its own description of each plugin. */
    void resetList()
    {
        if (scanner->isScanning())
        {
            setStatus (ko ("스캔 중에는 초기화할 수 없습니다 - 스캔 창을 먼저 끝내세요."), true);
            return;
        }

        juce::Component::SafePointer<Content> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle (ko ("플러그인 목록 초기화"))
                                          .withMessage (ko ("찾아 둔 플러그인 목록과 '문제 파일' 기록을 모두 지웁니다. 초기화한 뒤 [VST3 스캔]으로 다시 찾으세요.\n"
                                                            "체인에 넣어 둔 플러그인과 '사용' 체크는 그대로입니다."))
                                          .withButton (ko ("초기화"))
                                          .withButton (ko ("취소")),
                                      [safe] (int result)
        {
            if (safe == nullptr || result != 1 || safe->scanner->isScanning())
                return;

            auto& list = safe->host.getKnownPlugins();
            list.clear();
            list.clearBlacklistedFiles();
            safe->crashMarker.deleteFile();
            safe->statusLabel.setTooltip ({});
            safe->setStatus (ko ("플러그인 목록을 비웠습니다 - [VST3 스캔]으로 다시 찾으세요."), false);
        });
    }

    void removeSelectedPlugin()
    {
        const int row = pluginTable.getSelectedRow();

        if (row < 0 || row >= types.size())
            return;

        host.getKnownPlugins().removeType (types[row]);   // the list announces the change: the table refreshes
        setStatus (ko ("목록에서 뺐습니다 (다시 스캔하면 돌아옵니다): ") + types[row].name, false);
    }

    /** '전부 사용': the plugins the table shows - all of them, or the ones a search narrowed it to. */
    void setAll (bool enabled)
    {
        for (const auto& d : types)
            host.setPluginEnabled (d, enabled);

        settings.setDisabledPlugins (host.getDisabledPlugins());
        pluginTable.repaint();
        refreshBuilder();

        if (isSearching())
            setStatus (ko ("검색 결과 ") + juce::String (types.size()) + ko ("개를 '사용'으로 켰습니다 (검색을 지우면 전부 보입니다)"), false);
    }

    const PluginPreset* selectedPreset() const
    {
        const int row = presetTable.getSelectedRow();
        return row >= 0 && row < (int) presets.size() ? &presets[(size_t) row] : nullptr;
    }

    void presetsChanged()
    {
        refreshPresets();

        if (owner.onPresetsChanged)
            owner.onPresetsChanged();
    }

    void newPreset()
    {
        if (builderWindow != nullptr)
        {
            builderWindow->setVisible (true);
            builderWindow->toFront (true);
            return;
        }

        const auto enabled = host.getEffectTypes();

        if (enabled.isEmpty())
        {
            setStatus (ko ("쓸 수 있는 플러그인이 없습니다. 먼저 스캔하고 '사용'을 켜세요."), true);
            return;
        }

        juce::Component::SafePointer<Content> safe (this);
        auto closeLater = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeBuilder(); }); };
        auto* builder = new PresetBuilder (enabled,
            [safe, closeLater] (const PluginPreset& preset) -> juce::Result
            {
                if (safe == nullptr)
                    return juce::Result::fail (ko ("플러그인 관리 창이 닫혔습니다"));

                // what is saved is checked again now: a plugin switched off (or its format) since it was picked
                for (const auto& slot : preset.plugins)
                {
                    juce::PluginDescription d;
                    d.pluginFormatName = slot.format;
                    d.name = slot.name;
                    d.uniqueId = slot.uniqueId;

                    if (! safe->host.isPluginEnabled (d))
                        return juce::Result::fail (ko ("이제 쓸 수 없는 플러그인이 들어 있습니다 ('사용'이 꺼졌거나 그 형식이 꺼짐): ") + slot.name);
                }

                const auto file = PluginPreset::fileFor (preset.name, safe->presetsFolder);

                if (file.existsAsFile())
                    return juce::Result::fail (ko ("같은 이름의 프리셋이 이미 있습니다: ") + preset.name);

                if (const auto result = preset.save (file); result.failed())
                    return juce::Result::fail (ko ("프리셋을 저장하지 못했습니다: ") + result.getErrorMessage());

                safe->setStatus (ko ("프리셋 저장: ") + preset.name + "  (" + preset.summary() + ")", false);
                safe->presetsChanged();
                closeLater();
                return juce::Result::ok();
            },
            closeLater);

        // a window of its own, not a modal dialog: the manager (and the rest of the app) stays usable meanwhile
        auto* window = new BuilderWindow (ko ("새 플러그인 프리셋"), closeLater);
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (builder, true);
        window->setResizable (true, false);
        window->centreAroundComponent (this, window->getWidth(), window->getHeight());
        window->setVisible (true);
        window->toFront (true);
        builderWindow = window;
    }

    void renamePreset()
    {
        const auto* p = selectedPreset();

        if (p == nullptr)
            return;

        const auto oldName = p->name;
        const auto from = p->file;   // the file it was listed from (its name inside may differ from the file name: a restored backup)
        auto* alert = new juce::AlertWindow (ko ("프리셋 이름"), ko ("새 이름"), juce::MessageBoxIconType::NoIcon);
        alert->addTextEditor ("name", oldName, ko ("이름"));
        alert->addButton (ko ("확인"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        alert->addButton (ko ("취소"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<Content> safe (this);
        alert->enterModalState (true, juce::ModalCallbackFunction::create ([safe, alert, oldName, from] (int r)
        {
            if (safe == nullptr || r != 1)
                return;

            const auto newName = alert->getTextEditorContents ("name").trim();

            if (newName.isEmpty() || newName == oldName)
                return;

            const auto to = PluginPreset::fileFor (newName, safe->presetsFolder);

            if (to.existsAsFile() && to != from)   // (== from: only the case or the name inside changes)
            {
                safe->setStatus (ko ("같은 이름의 프리셋이 이미 있습니다: ") + newName, true);
                return;
            }

            PluginPreset p;

            if (const auto result = PluginPreset::load (from, p); result.failed())
            {
                safe->setStatus (result.getErrorMessage(), true);
                return;
            }

            p.name = newName;

            if (const auto result = p.save (to); result.failed())
            {
                safe->setStatus (result.getErrorMessage(), true);
                return;
            }

            if (to != from && ! from.deleteFile())
            {
                to.deleteFile();   // back to one preset under the old name, rather than two
                safe->setStatus (ko ("이름을 바꾸지 못했습니다 - 원래 파일을 지울 수 없습니다 (다른 프로그램이 열고 있나요?): ") + from.getFullPathName(), true);
                safe->presetsChanged();
                return;
            }

            safe->setStatus (ko ("이름을 바꿨습니다: ") + newName, false);
            safe->presetsChanged();
        }), true);
        focusAlertTextEditor (*alert, "name");
    }

    void deletePreset()
    {
        const auto* p = selectedPreset();

        if (p == nullptr)
            return;

        const auto name = p->name;
        const auto file = p->file;
        juce::Component::SafePointer<Content> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle (ko ("프리셋 삭제"))
                                          .withMessage (ko ("'") + name + ko ("' 프리셋을 지울까요? (파일이 지워집니다)"))
                                          .withButton (ko ("삭제"))
                                          .withButton (ko ("취소")),
                                      [safe, name, file] (int result)
        {
            if (safe == nullptr || result != 1)
                return;

            if (! file.deleteFile())
            {
                safe->setStatus (ko ("지우지 못했습니다: ") + file.getFullPathName(), true);
                safe->presetsChanged();
                return;
            }

            safe->setStatus (ko ("지웠습니다: ") + name, false);
            safe->presetsChanged();
        });
    }

    void exportPreset()
    {
        const auto* p = selectedPreset();

        if (p == nullptr)
            return;

        const auto source = p->file;
        chooser = std::make_unique<juce::FileChooser> (ko ("프리셋을 파일로 저장"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (PluginPreset::fileNameFor (p->name)),
                                                       "*" + juce::String (PluginPreset::fileExtension));
        juce::Component::SafePointer<Content> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe, source] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File())
                return;

            auto target = fc.getResult();
            const bool extensionAdded = ! PluginPreset::isPresetFileName (target.getFileName());

            if (extensionAdded)
            {
                // the chooser checked "Vocal.txt" for an overwrite, not "Vocal.livemixpreset": that one is checked here
                target = target.withFileExtension (PluginPreset::fileExtension);

                if (target.existsAsFile())
                {
                    safe->setStatus (ko ("같은 이름의 파일이 이미 있습니다 (확장자를 붙인 이름): ") + target.getFullPathName(), true);
                    return;
                }
            }

            // read back and written whole (a verified, atomic write): a failed copy leaves the old file, and the
            // preset's own file as the target is not deleted from under itself
            PluginPreset p;

            if (const auto loaded = PluginPreset::load (source, p); loaded.failed())
            {
                safe->setStatus (loaded.getErrorMessage(), true);
                return;
            }

            const auto saved = p.save (target);
            safe->setStatus (saved.wasOk() ? ko ("파일로 저장했습니다: ") + target.getFullPathName()
                                           : ko ("파일로 저장하지 못했습니다: ") + saved.getErrorMessage(), saved.failed());
        });
    }

    void importPreset()
    {
        chooser = std::make_unique<juce::FileChooser> (ko ("프리셋 파일 불러오기"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                       "*" + juce::String (PluginPreset::fileExtension));
        juce::Component::SafePointer<Content> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File())
                return;

            PluginPreset p;

            if (const auto result = PluginPreset::load (fc.getResult(), p); result.failed())
            {
                safe->setStatus (result.getErrorMessage(), true);
                return;
            }

            auto target = PluginPreset::fileFor (p.name, safe->presetsFolder);

            if (target.existsAsFile())
                target = target.getNonexistentSibling (true);   // "이름(2)": the one already here is not touched

            p.name = target.getFileNameWithoutExtension();

            if (const auto result = p.save (target); result.failed())
            {
                safe->setStatus (result.getErrorMessage(), true);
                return;
            }

            safe->setStatus (ko ("프리셋을 불러왔습니다: ") + p.name, false);
            safe->presetsChanged();
        });
    }

    PluginManagerWindow& owner;
    PluginHost& host;
    PluginScanCoordinator* scanCoordinator = nullptr; // owned by host's KnownPluginList
    const juce::File crashMarker = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                      .getChildFile ("LiveMix/scan.crashed");
    LiveMixSettings& settings;
    juce::File presetsFolder;
    juce::Array<juce::PluginDescription> allTypes, types;   // every known effect plugin; the ones the search shows (the table's rows)
    std::vector<PluginPreset> presets;
    PluginModel pluginModel;
    PresetModel presetModel;
    std::unique_ptr<PluginScanListComponent> scanner;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Component::SafePointer<juce::DialogWindow> builderWindow;

    juce::Label pluginsCaption, note, pluginCount, presetsCaption, presetsNote, statusLabel;
    juce::TextEditor pluginSearch;
    juce::ToggleButton vst2Toggle;
    juce::TableListBox pluginTable, presetTable;
    juce::TextButton scanVst3Button, scanVst2Button, removeButton, enableAllButton, resetListButton;
    juce::TextButton newPresetButton, renamePresetButton, deletePresetButton, exportPresetButton, importPresetButton, openFolderButton;
};

//==============================================================================
PluginManagerWindow::PluginManagerWindow (PluginHost& host, LiveMixSettings& settings, const juce::File& presetsFolder)
    : DocumentWindow (ko ("플러그인 관리"), Palette::background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    auto* c = new Content (*this, host, settings, presetsFolder);
    content = c;
    setContentOwned (c, true);
    setResizable (true, false);
    setResizeLimits (600, 480, 10000, 10000);   // a portrait monitor at 150% is about 720 wide
    centreWithSize (getWidth(), getHeight());   // the owner then centres it on its own display (centreAroundComponent)
}

PluginManagerWindow::~PluginManagerWindow()
{
    clearContentComponent();
}

void PluginManagerWindow::open()
{
    if (content != nullptr)
        content->refreshPresets();

    setVisible (true);
    toFront (true);

    if (content != nullptr)
        content->focusSearch();   // typing starts the search at once
}

void PluginManagerWindow::closeButtonPressed()
{
    if (content != nullptr)
        content->closeBuilder();   // its window goes with this one

    setVisible (false);   // kept: reopening is instant
}

void PluginManagerWindow::refreshPresets()
{
    if (content != nullptr)
        content->refreshPresets();
}

} // namespace gocue::livemix
