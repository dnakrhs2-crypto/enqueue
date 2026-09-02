#include "ui/PatchEditorDialog.h"

#include "ui/LevelMatrixComponent.h"
#include "ui/PluginChainComponent.h"
#include "ui/UiUtils.h"

namespace gocue::PatchEditorDialog
{

namespace
{
    juce::Component::SafePointer<juce::DialogWindow> dialog;
    juce::Component::SafePointer<juce::DialogWindow> insertsDialog;

    void styleLabel (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, Palette::dimText);
        label.setFont (juce::Font (juce::FontOptions (12.0f)));
    }

    /** One insert chain strip in its own window (cue output or device output inserts). */
    class InsertsContent : public juce::Component
    {
    public:
        InsertsContent (AudioEngine& engine, PluginWindowManager& windows, std::function<void()> onOpenPluginManager,
                        PluginChain& chain, const juce::String& description, const juce::String& ownerName, ProjectDocument& document)
            : strip (engine, windows)
        {
            title.setText (description, juce::dontSendNotification);
            title.setColour (juce::Label::textColourId, Palette::dimText);
            title.setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (title);

            strip.onOpenPluginManager = std::move (onOpenPluginManager);
            strip.performEdit = [&document] (const juce::String&, const std::function<void()>& edit)
            {
                edit();                 // patch inserts sit outside the undo history (like the patch itself)
                document.markDirty();
            };
            strip.setChain (&chain, ownerName);
            addAndMakeVisible (strip);
            setSize (760, 120);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12, 10);
            title.setBounds (area.removeFromTop (20));
            area.removeFromTop (8);
            strip.setBounds (area);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }
        void chainChanged (PluginChain* chain) { strip.chainChanged (chain); }

    private:
        juce::Label title;
        PluginChainComponent strip;
    };

    juce::Component::SafePointer<InsertsContent> insertsContent;

    juce::DialogWindow* launch (juce::Component* content, const juce::String& title, juce::Component* centreAround, bool resizable)
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = title;
        options.content.setOwned (content);
        options.componentToCentreAround = centreAround;
        options.dialogBackgroundColour = Palette::panel;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = resizable;
        return options.launchAsync();
    }

    //==========================================================================
    class Content : public juce::Component,
                    private juce::ListBoxModel
    {
    public:
        Content (ProjectDocument& doc, AudioEngine& e, PluginWindowManager& w, std::function<void()> openManager)
            : document (doc), engine (e), windows (w), onOpenPluginManager (std::move (openManager))
        {
            patchList.setModel (this);
            patchList.setRowHeight (24);
            patchList.setColour (juce::ListBox::backgroundColourId, Palette::rowOdd);
            addAndMakeVisible (patchList);

            auto button = [this] (juce::TextButton& b, const char* text, std::function<void()> fn)
            {
                b.setButtonText (ko (text));
                b.setWantsKeyboardFocus (false);
                b.onClick = std::move (fn);
                addAndMakeVisible (b);
            };

            button (addButton, "추가", [this] { addPatch(); });
            button (duplicateButton, "복제", [this] { duplicatePatch(); });
            button (removeButton, "삭제", [this] { removePatch(); });
            button (defaultsButton, "기본 라우팅으로", [this] { resetRouting(); });

            styleLabel (nameLabel, ko ("이름"));
            addAndMakeVisible (nameLabel);
            nameEditor.setSelectAllWhenFocused (true);
            nameEditor.onReturnKey = [this] { commitName(); nameEditor.giveAwayKeyboardFocus(); };
            nameEditor.onFocusLost = [this] { commitName(); };
            addAndMakeVisible (nameEditor);

            styleLabel (outputsLabel, ko ("큐 출력 개수 (1~128)"));
            addAndMakeVisible (outputsLabel);
            outputsEditor.setInputRestrictions (3, "0123456789");
            outputsEditor.setJustification (juce::Justification::centredRight);
            outputsEditor.setSelectAllWhenFocused (true);
            outputsEditor.onReturnKey = [this] { commitOutputs(); outputsEditor.giveAwayKeyboardFocus(); };
            outputsEditor.onFocusLost = [this] { commitOutputs(); };
            addAndMakeVisible (outputsEditor);

            styleLabel (deviceLabel, "");
            addAndMakeVisible (deviceLabel);

            tabs.setTabBarDepth (26);
            tabs.setOutline (0);
            tabs.setColour (juce::TabbedComponent::backgroundColourId, Palette::panel);
            tabs.addTab (ko ("큐 출력"), Palette::panel, &cueOutputsPage, false);
            tabs.addTab (ko ("패치 라우팅"), Palette::panel, &routingPage, false);
            tabs.addTab (ko ("장치 출력"), Palette::panel, &deviceOutputsPage, false);
            addAndMakeVisible (tabs);

            // cue outputs page
            cueOutputsViewport.setViewedComponent (&cueOutputsStrip, false);
            cueOutputsViewport.setScrollBarsShown (true, false);
            cueOutputsPage.addAndMakeVisible (cueOutputsViewport);
            styleLabel (cueOutputsHint, ko ("이름 · 다음 출력과 스테레오로 묶기(인서트가 2채널을 받음) · 출력별 VST3 인서트"));
            cueOutputsPage.addAndMakeVisible (cueOutputsHint);

            // routing page
            routingViewport.setViewedComponent (&routingGrid, false);
            routingViewport.setScrollBarsShown (true, true);
            routingPage.addAndMakeVisible (routingViewport);
            styleLabel (routingHint, ko ("행 = 큐 출력, 열 = 장치 출력, 왼쪽 위 = 패치 메인. 드래그 / 더블클릭 / 숫자 입력 / 우클릭 겡 — 재생 중에도 즉시 반영"));
            routingPage.addAndMakeVisible (routingHint);
            routingGrid.setEdgeLevelsVisible (false);
            routingGrid.onChange = [this] (double mainDb, const LevelMatrix& m, bool) { commitRouting (mainDb, m); };

            // device outputs page
            deviceOutputsViewport.setViewedComponent (&deviceOutputsStrip, false);
            deviceOutputsViewport.setScrollBarsShown (true, false);
            deviceOutputsPage.addAndMakeVisible (deviceOutputsViewport);
            styleLabel (deviceOutputsHint, ko ("장치 출력별 VST3 인서트 (이 패치의 소리에만 적용). 마스터 버스 인서트는 오디오 메뉴에 그대로 있습니다"));
            deviceOutputsPage.addAndMakeVisible (deviceOutputsHint);

            reload();
            setSize (900, 560);
        }

        ~Content() override
        {
            patchList.setModel (nullptr);
        }

        void reload()
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            patchList.updateContent();

            if (selected >= (int) document.patches.size())
                selected = (int) document.patches.size() - 1;

            if (selected < 0 && ! document.patches.empty())
                selected = 0;

            patchList.selectRow (selected, juce::dontSendNotification);
            refreshDetails();
        }

        void chainChanged (PluginChain* chain)
        {
            if (insertsContent != nullptr)
                insertsContent->chainChanged (chain);

            refreshInsertCounts();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (12, 10);
            auto left = area.removeFromLeft (200);
            auto buttons = left.removeFromBottom (26);
            addButton.setBounds (buttons.removeFromLeft (60));
            buttons.removeFromLeft (4);
            duplicateButton.setBounds (buttons.removeFromLeft (60));
            buttons.removeFromLeft (4);
            removeButton.setBounds (buttons.removeFromLeft (60));
            left.removeFromBottom (6);
            patchList.setBounds (left);

            area.removeFromLeft (12);
            auto row = area.removeFromTop (24);
            nameLabel.setBounds (row.removeFromLeft (36));
            nameEditor.setBounds (row.removeFromLeft (220));
            row.removeFromLeft (12);
            outputsLabel.setBounds (row.removeFromLeft (130));
            outputsEditor.setBounds (row.removeFromLeft (50));
            row.removeFromLeft (12);
            defaultsButton.setBounds (row.removeFromLeft (120));
            area.removeFromTop (4);
            deviceLabel.setBounds (area.removeFromTop (18));
            area.removeFromTop (4);
            tabs.setBounds (area);

            auto page = tabs.getLocalBounds().withTrimmedTop (tabs.getTabBarDepth()).reduced (8, 6);
            juce::ignoreUnused (page);
            layoutPages();
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

    private:
        //======================================================================
        // ListBoxModel
        int getNumRows() override { return (int) document.patches.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowIsSelected) override
        {
            if (row < 0 || row >= (int) document.patches.size())
                return;

            g.fillAll (rowIsSelected ? Palette::standby.withAlpha (0.35f) : (row % 2 == 0 ? Palette::rowEven : Palette::rowOdd));
            g.setColour (Palette::text);
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            const auto& p = document.patches[(size_t) row];
            g.drawFittedText ((row == 0 ? ko ("★ ") : juce::String()) + p.name + "  (" + juce::String (p.numCueOutputs) + ")",
                              6, 0, width - 12, height, juce::Justification::centredLeft, 1);
        }

        void selectedRowsChanged (int lastRowSelected) override
        {
            if (refreshing)
                return;

            selected = lastRowSelected;
            refreshDetails();
        }

        //======================================================================
        AudioPatch* current()
        {
            if (selected < 0 || selected >= (int) document.patches.size())
                return nullptr;

            return &document.patches[(size_t) selected];
        }

        void refreshDetails()
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            const auto* p = current();
            const bool has = p != nullptr;

            for (auto* c : std::initializer_list<juce::Component*> { &nameEditor, &outputsEditor, &defaultsButton, &tabs })
                c->setEnabled (has);

            removeButton.setEnabled (has && document.patches.size() > 1);
            duplicateButton.setEnabled (has);

            const int deviceOutputs = engine.getNumDeviceOutputs();
            deviceLabel.setText (ko ("장치 출력 ") + juce::String (deviceOutputs) + ko ("채널 (오디오 > 오디오 출력 설정에서 채널 수 변경)"), juce::dontSendNotification);

            if (! has)
            {
                nameEditor.setText ("", false);
                outputsEditor.setText ("", false);
                rebuildCueOutputRows (nullptr);
                rebuildDeviceOutputRows (nullptr);
                routingGrid.setLevels (0.0, LevelMatrix());
                return;
            }

            nameEditor.setText (p->name, false);
            outputsEditor.setText (juce::String (p->numCueOutputs), false);
            rebuildCueOutputRows (p);
            rebuildDeviceOutputRows (p);

            LevelMatrix routing;
            routing.resize (p->numCueOutputs, juce::jmax (2, deviceOutputs));

            for (int k = 0; k < p->numCueOutputs; ++k)
                for (int m = 0; m < routing.numOutputs(); ++m)
                    routing.crosspointDb[(size_t) k][(size_t) m] = p->routing (k, m);

            juce::StringArray inputNames, outputNames;

            for (int k = 0; k < p->numCueOutputs; ++k)
                inputNames.add (p->cueOutputName (k));

            for (int m = 0; m < routing.numOutputs(); ++m)
                outputNames.add (ko ("장치 ") + juce::String (m + 1));

            routingGrid.setLabels (inputNames, outputNames);
            routingGrid.setLimits (document.settings.minLevelDb, document.settings.maxLevelDb);
            routingGrid.setLevels (p->mainDb, routing);
            layoutPages();
        }

        struct CueOutputRow
        {
            juce::Label label;
            juce::TextEditor name;
            juce::ToggleButton pair;
            juce::TextButton inserts;
        };

        struct DeviceOutputRow
        {
            juce::Label label;
            juce::TextButton inserts;
        };

        juce::String insertsText (PluginChain* chain) const
        {
            const int n = chain != nullptr ? chain->getNumSlots() : 0;
            return n > 0 ? ko ("인서트 (") + juce::String (n) + ")" : ko ("인서트...");
        }

        void rebuildCueOutputRows (const AudioPatch* p)
        {
            cueOutputRows.clear();

            if (p == nullptr)
                return;

            for (int k = 0; k < p->numCueOutputs; ++k)
            {
                auto* r = cueOutputRows.add (new CueOutputRow());
                styleLabel (r->label, juce::String (k + 1));
                r->label.setJustificationType (juce::Justification::centredRight);
                cueOutputsStrip.addAndMakeVisible (r->label);

                r->name.setText (k < p->cueOutputNames.size() ? p->cueOutputNames[k] : juce::String(), false);
                r->name.setTextToShowWhenEmpty (p->cueOutputName (k), Palette::dimText);
                r->name.setSelectAllWhenFocused (true);
                r->name.onReturnKey = [this, k, r] { commitOutputName (k, r->name.getText()); r->name.giveAwayKeyboardFocus(); };
                r->name.onFocusLost = [this, k, r] { commitOutputName (k, r->name.getText()); };
                cueOutputsStrip.addAndMakeVisible (r->name);

                r->pair.setButtonText (ko ("다음과 스테레오"));
                r->pair.setColour (juce::ToggleButton::textColourId, Palette::text);
                r->pair.setColour (juce::ToggleButton::tickColourId, Palette::standby);
                r->pair.setWantsKeyboardFocus (false);
                r->pair.setToggleState (p->isFirstOfPair (k), juce::dontSendNotification);
                r->pair.setEnabled (k + 1 < p->numCueOutputs && ! p->isSecondOfPair (k));
                r->pair.onClick = [this, k, r] { commitPair (k, r->pair.getToggleState()); };
                cueOutputsStrip.addAndMakeVisible (r->pair);

                r->inserts.setButtonText (insertsText (engine.findPatchCueOutputChain (p->id, k)));
                r->inserts.setWantsKeyboardFocus (false);
                r->inserts.setEnabled (! p->isSecondOfPair (k));
                r->inserts.onClick = [this, k] { openCueOutputInserts (k); };
                cueOutputsStrip.addAndMakeVisible (r->inserts);
            }
        }

        void rebuildDeviceOutputRows (const AudioPatch* p)
        {
            deviceOutputRows.clear();

            if (p == nullptr)
                return;

            const int outs = engine.getNumDeviceOutputs();

            for (int m = 0; m < outs; ++m)
            {
                auto* r = deviceOutputRows.add (new DeviceOutputRow());
                styleLabel (r->label, ko ("장치 출력 ") + juce::String (m + 1));
                deviceOutputsStrip.addAndMakeVisible (r->label);
                r->inserts.setButtonText (insertsText (engine.findPatchDeviceOutputChain (p->id, m)));
                r->inserts.setWantsKeyboardFocus (false);
                r->inserts.onClick = [this, m] { openDeviceOutputInserts (m); };
                deviceOutputsStrip.addAndMakeVisible (r->inserts);
            }
        }

        void refreshInsertCounts()
        {
            const auto* p = current();

            if (p == nullptr)
                return;

            for (int k = 0; k < cueOutputRows.size(); ++k)
                cueOutputRows[k]->inserts.setButtonText (insertsText (engine.findPatchCueOutputChain (p->id, k)));

            for (int m = 0; m < deviceOutputRows.size(); ++m)
                deviceOutputRows[m]->inserts.setButtonText (insertsText (engine.findPatchDeviceOutputChain (p->id, m)));
        }

        void layoutPages()
        {
            const auto pageArea = tabs.getLocalBounds().withTrimmedTop (tabs.getTabBarDepth());

            for (auto* page : { &cueOutputsPage, &routingPage, &deviceOutputsPage })
                page->setBounds (pageArea);

            auto area = cueOutputsPage.getLocalBounds().reduced (8, 6);
            cueOutputsHint.setBounds (area.removeFromTop (18));
            area.removeFromTop (4);
            cueOutputsViewport.setBounds (area);
            const int rowH = 28;
            cueOutputsStrip.setSize (juce::jmax (100, area.getWidth() - 14), juce::jmax (area.getHeight(), cueOutputRows.size() * rowH));

            for (int k = 0; k < cueOutputRows.size(); ++k)
            {
                auto* r = cueOutputRows[k];
                auto row = juce::Rectangle<int> (0, k * rowH, cueOutputsStrip.getWidth(), rowH).reduced (0, 2);
                r->label.setBounds (row.removeFromLeft (30));
                row.removeFromLeft (6);
                r->name.setBounds (row.removeFromLeft (200));
                row.removeFromLeft (12);
                r->pair.setBounds (row.removeFromLeft (140));
                row.removeFromLeft (12);
                r->inserts.setBounds (row.removeFromLeft (110));
            }

            area = routingPage.getLocalBounds().reduced (8, 6);
            routingHint.setBounds (area.removeFromTop (18));
            area.removeFromTop (4);
            routingViewport.setBounds (area);

            area = deviceOutputsPage.getLocalBounds().reduced (8, 6);
            deviceOutputsHint.setBounds (area.removeFromTop (18));
            area.removeFromTop (4);
            deviceOutputsViewport.setBounds (area);
            deviceOutputsStrip.setSize (juce::jmax (100, area.getWidth() - 14), juce::jmax (area.getHeight(), deviceOutputRows.size() * rowH));

            for (int m = 0; m < deviceOutputRows.size(); ++m)
            {
                auto* r = deviceOutputRows[m];
                auto row = juce::Rectangle<int> (0, m * rowH, deviceOutputsStrip.getWidth(), rowH).reduced (0, 2);
                r->label.setBounds (row.removeFromLeft (110));
                row.removeFromLeft (6);
                r->inserts.setBounds (row.removeFromLeft (110));
            }
        }

        //======================================================================
        void store (bool structural)
        {
            auto patches = document.patches;   // copy: setPatches replaces the vector
            document.setPatches (patches);

            if (structural)
                engine.setPatches (document.patches);
            else if (const auto* p = current())
                engine.updatePatchLevels (*p);

            patchList.repaint();
        }

        void addPatch()
        {
            auto p = AudioPatch::makeDefault (ko ("패치 ") + juce::String (document.patches.size() + 1));
            document.patches.push_back (p);
            selected = (int) document.patches.size() - 1;
            store (true);
            reload();
        }

        void duplicatePatch()
        {
            const auto* p = current();

            if (p == nullptr)
                return;

            AudioPatch copy = *p;
            engine.capturePatchInsertStates (copy);
            copy.id = juce::Uuid();
            copy.name = p->name + ko (" 사본");
            document.patches.insert (document.patches.begin() + selected + 1, copy);
            ++selected;
            store (true);
            reload();
        }

        void removePatch()
        {
            if (current() == nullptr || document.patches.size() <= 1)
                return;

            document.patches.erase (document.patches.begin() + selected);
            selected = juce::jmax (0, selected - 1);
            store (true);
            reload();
        }

        void resetRouting()
        {
            auto* p = current();

            if (p == nullptr)
                return;

            p->routingDb.clear();
            p->mainDb = 0.0;
            p->sanitise();
            store (false);
            refreshDetails();
        }

        void commitName()
        {
            auto* p = current();

            if (p == nullptr || refreshing)
                return;

            const auto name = nameEditor.getText().trim();

            if (name.isEmpty() || name == p->name)
                return;

            p->name = name;
            store (false);
        }

        void commitOutputs()
        {
            auto* p = current();

            if (p == nullptr || refreshing)
                return;

            const int n = juce::jlimit (1, AudioPatch::maxCueOutputs, outputsEditor.getText().getIntValue());

            if (n == p->numCueOutputs)
            {
                outputsEditor.setText (juce::String (n), false);
                return;
            }

            engine.capturePatchInsertStates (*p);   // keep inserts of outputs that survive
            p->numCueOutputs = n;
            p->sanitise();
            store (true);
            refreshDetails();
        }

        void commitOutputName (int k, const juce::String& text)
        {
            auto* p = current();

            if (p == nullptr || refreshing || k >= p->cueOutputNames.size())
                return;

            const auto name = text.trim();

            if (p->cueOutputNames[k] == name)
                return;

            p->cueOutputNames.set (k, name);
            store (false);
        }

        void commitPair (int k, bool on)
        {
            auto* p = current();

            if (p == nullptr || refreshing || k >= (int) p->cueOutputStereoWithNext.size())
                return;

            p->cueOutputStereoWithNext[(size_t) k] = on ? 1 : 0;
            p->sanitise();
            store (false);
            refreshDetails();
        }

        void commitRouting (double mainDb, const LevelMatrix& m)
        {
            auto* p = current();

            if (p == nullptr || refreshing)
                return;

            p->mainDb = mainDb;

            for (int k = 0; k < juce::jmin (p->numCueOutputs, m.numInputs()); ++k)
                for (int d = 0; d < m.numOutputs(); ++d)
                    p->setRouting (k, d, m.crosspointDb[(size_t) k][(size_t) d]);

            store (false);
        }

        void openCueOutputInserts (int k)
        {
            const auto* p = current();

            if (p == nullptr)
                return;

            auto& chain = engine.getPatchCueOutputChain (p->id, k);
            const auto label = p->cueOutputName (k) + (p->isFirstOfPair (k) ? " + " + p->cueOutputName (k + 1) : juce::String());
            showInserts (chain, p->name + ko (" - 큐 출력 ") + label + ko (" 인서트 (라우팅 앞)"), p->name + " / " + label);
        }

        void openDeviceOutputInserts (int m)
        {
            const auto* p = current();

            if (p == nullptr)
                return;

            auto& chain = engine.getPatchDeviceOutputChain (p->id, m);
            showInserts (chain, p->name + ko (" - 장치 출력 ") + juce::String (m + 1) + ko (" 인서트 (라우팅 뒤)"), p->name + ko (" / 장치 ") + juce::String (m + 1));
        }

        void showInserts (PluginChain& chain, const juce::String& description, const juce::String& ownerName)
        {
            if (insertsDialog != nullptr)
                delete insertsDialog.getComponent();

            auto* content = new InsertsContent (engine, windows, onOpenPluginManager, chain, description, ownerName, document);
            insertsContent = content;
            insertsDialog = launch (content, ko ("패치 인서트"), this, false);
        }

        ProjectDocument& document;
        AudioEngine& engine;
        PluginWindowManager& windows;
        std::function<void()> onOpenPluginManager;
        int selected = 0;
        bool refreshing = false;

        juce::ListBox patchList;
        juce::TextButton addButton, duplicateButton, removeButton, defaultsButton;
        juce::Label nameLabel, outputsLabel, deviceLabel;
        juce::TextEditor nameEditor, outputsEditor;
        juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
        juce::Component cueOutputsPage, routingPage, deviceOutputsPage;
        juce::Label cueOutputsHint, routingHint, deviceOutputsHint;
        juce::Viewport cueOutputsViewport, routingViewport, deviceOutputsViewport;
        juce::Component cueOutputsStrip, deviceOutputsStrip;
        juce::OwnedArray<CueOutputRow> cueOutputRows;
        juce::OwnedArray<DeviceOutputRow> deviceOutputRows;
        LevelMatrixComponent routingGrid;
    };

    juce::Component::SafePointer<Content> content;
}

void show (ProjectDocument& document, AudioEngine& engine, PluginWindowManager& windows,
           std::function<void()> onOpenPluginManager, juce::Component* centreAround)
{
    if (dialog != nullptr)
    {
        dialog->toFront (true);
        return;
    }

    auto* c = new Content (document, engine, windows, std::move (onOpenPluginManager));
    content = c;
    dialog = launch (c, ko ("오디오 패치"), centreAround, true);
}

void closeIfOpen()
{
    if (insertsDialog != nullptr)
        delete insertsDialog.getComponent();

    if (dialog != nullptr)
        delete dialog.getComponent();
}

void chainChanged (PluginChain* chain)
{
    if (content != nullptr)
        content->chainChanged (chain);
}

void patchesChanged()
{
    if (content != nullptr)
        content->reload();
}

} // namespace gocue::PatchEditorDialog
