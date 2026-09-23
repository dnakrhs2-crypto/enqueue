#include "app/Commands.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/LevelMatrixComponent.h"
#include "MainComponentTestAccess.h"

#include <array>
#include <cmath>
#include <cstring>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <shlobj.h>
#endif

// EE-2, EE-3 and EC-3 from audit0923_ref/Audit0923EnqTests.cpp.
// Visible-peer variants are deliberately omitted: the hidden component tree
// queues JUCE's public focusLost event before synchronous table selection.
// No production completion callback or OS input is substituted.
namespace gocue::tests
{
namespace
{
constexpr double sampleRate0923 = 44100.0;
constexpr int blockSize0923 = 512;

// Observation only, using the same explicit-instantiation access technique as
// Recheck0920E5Tests. No private production edit/completion function is called.
template <class Tag, typename Tag::Type member>
struct Audit0923MemberAccess
{
    friend typename Tag::Type audit0923Member (Tag) { return member; }
};
struct Audit0923DocumentMember
{
    using Type = ProjectDocument MainComponent::*;
    friend Type audit0923Member (Audit0923DocumentMember);
};
template struct Audit0923MemberAccess<Audit0923DocumentMember, &MainComponent::document>;

template <typename T, typename Predicate>
T* child0923 (juce::Component& root, Predicate predicate)
{
    if (auto* found = dynamic_cast<T*> (&root); found != nullptr && predicate (*found)) return found;
    for (auto* child : root.getChildren())
        if (auto* found = child0923<T> (*child, predicate)) return found;
    return nullptr;
}
template <typename T> T* child0923 (juce::Component& root)
{
    return child0923<T> (root, [] (const T&) { return true; });
}

// JUCE_MODAL_LOOPS_PERMITTED=0 in this project. This dispatches JUCE's Windows
// MessageManager window, including TextEditor::postCommandMessage and AsyncUpdater.
void dispatch0923()
{
   #if JUCE_WINDOWS
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    MSG message {};
    for (int n = 0; n < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}
struct Scratch0923
{
    Scratch0923() { folder.createDirectory(); }
    ~Scratch0923()
    {
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("EnqueueAudit0923-"))
            folder.deleteRecursively();
    }
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("EnqueueAudit0923-" + juce::Uuid().toString());
};

bool writeTone0923 (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (! stream) return false;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate0923)
                                         .withNumChannels (2).withBitsPerSample (16));
    if (! writer) return false;
    juce::AudioBuffer<float> block (2, 44100);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < block.getNumSamples(); ++i)
            block.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate0923));
    for (int second = 0; second < 30; ++second)
        if (! writer->writeFromAudioSampleBuffer (block, 0, block.getNumSamples())) return false;
    return true;
}

Project project0923 (const juce::File& file, int count = 2)
{
    Project p;
    p.name = "Audit0923";
    p.ensureMainList();
    p.settings.autoBackup = false;
    p.settings.backupBeforeSave = false;
    p.settings.copyFilesIntoProject = false;
    p.settings.autoLoadNewCues = false;
    auto& patch = p.ensureDefaultPatch();
    patch.numCueOutputs = 2;
    patch.sanitise();
    for (int i = 0; i < count; ++i)
    {
        Cue c;
        c.name = i == 0 ? "A" : "B";
        c.number = juce::String (i + 1);
        c.notes = c.name + " original notes";
        c.file = file;
        c.numChannels = 2;
        c.durationSeconds = 30.0;
        c.autoLoad = false;
        c.levels.resize (2, 2);
        c.levels.setDefaults();
        p.cues().push_back (c);
    }
    return p;
}

struct Fixture0923
{
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options o;
        o.millisecondsBeforeSaving = -1;
        return o;
    }
    explicit Fixture0923 (int outputs = 2)
        : storage (scratch.folder.getChildFile ("test.settings"), options()), settings (storage), numOutputs (outputs)
    {
        engine.prepare (sampleRate0923, blockSize0923, outputs);
        main = std::make_unique<MainComponent> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        main->setSize (1541, 980);
    }
    ~Fixture0923()
    {
        main.reset();
        dispatch0923();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        dispatch0923();
        engine.shutdown();
        storage.saveIfNeeded();
    }
    ProjectDocument& document() { return (*main).*audit0923Member (Audit0923DocumentMember {}); }
    CueInspector& inspector() { return *child0923<CueInspector> (*main); }
    juce::TableListBox& list() { return *child0923<juce::TableListBox> (*child0923<CueTable> (*main)); }
    bool command (juce::CommandID id) { return main->perform (juce::ApplicationCommandTarget::InvocationInfo (id)); }
    bool key (const juce::KeyPress& k)
    {
        return ReopenLastProjectTestAccess::keyboard (*main).keyPressed (k, &list());
    }
    void settle()
    {
        juce::AudioBuffer<float> block (numOutputs, blockSize0923);
        for (int n = 0; n < 8; ++n) engine.renderBlock (block, blockSize0923);
        engine.reapIfNeeded();
    }
    bool open (const Project& p, const juce::String& name = "show.enqueue")
    {
        const auto file = scratch.folder.getChildFile (name);
        if (ProjectSerializer::save (p, file).failed()) return false;
        main->openProjectFile (file, false);
        settle();
        dispatch0923();
        return main->getProjectFile() == file && ! document().isDirty();
    }
    void hiddenPeer()
    {
        main->addToDesktop (0); // stays invisible; never grabs OS focus
        main->setVisible (false);
        dispatch0923();
    }
    void focus (juce::TextEditor& editor)
    {
        pendingFocus = &editor;
        editor.focusGained (juce::Component::focusChangedDirectly);
    }
    bool tab (const juce::String& name)
    {
        auto* tabs = child0923<juce::TabbedComponent> (inspector());
        const int index = tabs != nullptr ? tabs->getTabNames().indexOf (name) : -1;
        if (index < 0) return false;
        tabs->setCurrentTabIndex (index);
        dispatch0923();
        return true;
    }
    bool row (int index)
    {
        // Component::internalMouseDown moves focus first (juce_Component.cpp:2536).
        // TextEditor::focusLost posts onFocusLost (juce_TextEditor.cpp:1987/2041).
        // Selection and CueInspector refresh finish BEFORE that message is drained.
        if (pendingFocus != nullptr)
            pendingFocus->focusLost (juce::Component::focusChangedByMouseClick);
        pendingFocus = nullptr;
        list().selectRowsBasedOnModifierKeys (index, {}, false);
        return document().cues.getSelectedIndex() == index;
    }
    juce::Component::SafePointer<juce::TextEditor> pendingFocus;
    std::array<double, 4> rms()
    {
        settle();
        juce::AudioBuffer<float> block (numOutputs, blockSize0923);
        std::array<double, 4> sums {};
        for (int b = 0; b < 16; ++b)
        {
            engine.renderBlock (block, blockSize0923);
            for (int ch = 0; ch < numOutputs; ++ch)
                for (int n = 0; n < blockSize0923; ++n)
                    sums[(size_t) ch] += (double) block.getSample (ch, n) * block.getSample (ch, n);
        }
        for (auto& value : sums) value = std::sqrt (value / (16 * blockSize0923));
        return sums;
    }
    Scratch0923 scratch;
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 }; // synchronous file reads, as in AuditFixUndoTests
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    int numOutputs;
};

struct FocusWitness0923 : juce::TextEditor::Listener
{
    FocusWitness0923 (juce::TextEditor& e, ProjectDocument& d) : editor (&e), document (d) { e.addListener (this); }
    ~FocusWitness0923() override { if (editor != nullptr) editor->removeListener (this); }
    void textEditorFocusLost (juce::TextEditor& e) override
    {
        ++calls;
        text = e.getText();
        selected = document.cues.getSelectedIndex();
    }
    juce::Component::SafePointer<juce::TextEditor> editor;
    ProjectDocument& document;
    int calls = 0, selected = -1;
    juce::String text;
};

#if JUCE_WINDOWS
// The product deliberately uses Windows shell folders, NOT APPDATA environment
// variables. Redirect only this test process's SHGetSpecialFolderPathW import;
// never change a real known-folder registration, registry value or user file.
// This is a filesystem dependency substitution, not a download/UI callback mock.
class ShellFolders0923
{
public:
    explicit ShellFolders0923 (const juce::File& folder)
    {
        root = folder;
        for (const auto* name : { "Documents", "LocalAppData", "AppData", "Profile" }) root.getChildFile (name).createDirectory();
        auto* base = reinterpret_cast<unsigned char*> (GetModuleHandleW (nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*> (base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*> (base + dos->e_lfanew);
        const auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (rva == 0) return;
        for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*> (base + rva); desc->Name != 0; ++desc)
        {
            if (desc->OriginalFirstThunk == 0) continue;
            auto* name = reinterpret_cast<IMAGE_THUNK_DATA*> (base + desc->OriginalFirstThunk);
            auto* address = reinterpret_cast<IMAGE_THUNK_DATA*> (base + desc->FirstThunk);
            for (; name->u1.AddressOfData != 0; ++name, ++address)
            {
                if (IMAGE_SNAP_BY_ORDINAL (name->u1.Ordinal)) continue;
                const auto* imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*> (base + name->u1.AddressOfData);
                if (std::strcmp (reinterpret_cast<const char*> (imported->Name), "SHGetSpecialFolderPathW") != 0) continue;
                slot = &address->u1.Function;
                original = reinterpret_cast<Function> (*slot);
                installed = replace (reinterpret_cast<ULONG_PTR> (&redirect));
                return;
            }
        }
    }
    ~ShellFolders0923() { if (installed) replace (reinterpret_cast<ULONG_PTR> (original)); }
    bool ok() const { return installed; }
private:
    using Function = BOOL (WINAPI*) (HWND, LPWSTR, int, BOOL);
    bool replace (ULONG_PTR value)
    {
        DWORD old = 0, ignored = 0;
        if (! VirtualProtect (slot, sizeof (*slot), PAGE_READWRITE, &old)) return false;
        *slot = value;
        VirtualProtect (slot, sizeof (*slot), old, &ignored);
        return true;
    }
    static BOOL WINAPI redirect (HWND window, LPWSTR path, int type, BOOL create)
    {
        const int id = type & 0xff;
        const char* child = id == CSIDL_PERSONAL ? "Documents" : id == CSIDL_LOCAL_APPDATA ? "LocalAppData"
                          : id == CSIDL_APPDATA ? "AppData" : id == CSIDL_PROFILE ? "Profile" : nullptr;
        if (child == nullptr) return original (window, path, type, create);
        const auto result = root.getChildFile (child).getFullPathName();
        if (result.length() >= MAX_PATH) return FALSE;
        lstrcpyW (path, result.toWideCharPointer());
        return TRUE;
    }
    static inline juce::File root;
    static inline Function original = nullptr;
    ULONG_PTR* slot = nullptr;
    bool installed = false;
};

#endif

juce::MouseEvent mouse0923 (juce::Component& component, juce::Point<int> point,
                            juce::Point<int> start, bool dragged = false)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point.toFloat(),
             juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
             &component, &component, now, start.toFloat(), now, 1, dragged };
}

class AuditFix0923UiTests final : public juce::UnitTest
{
public:
    AuditFix0923UiTests() : UnitTest ("AuditFix0923 Ui", "Enqueue") {}
    void runTest() override
    {
       #if JUCE_WINDOWS
        Scratch0923 sandbox;
        ShellFolders0923 folders (sandbox.folder);
        beginTest ("temporary user folders and hidden peers");
        if (! require (folders.ok(), "isolate Windows shell folders")) return;
        for (int field = 0; field < 4; ++field) basics (field);
        basicsCancelAndInvalid();
        basicsModelRefresh();
        levels();
        for (bool cancel : { false, true }) levelsCompletion (cancel);
        levelsDrag();
        patchUndo();
        patchUndo (true, false);
        patchUndo (false, true);
        patchUndo (true, true);
        for (int count : { 1, 5 }) timelineSwitch (count);
       #else
        logMessage ("Windows JUCE message dispatch is required for these UI regressions.");
       #endif
    }
private:
    bool require (bool condition, const juce::String& what)
    {
        expect (condition, "SETUP / NOT REPRODUCED: " + what);
        return condition;
    }
    void basics (int field)
    {
        const char* fields[] { "notes", "name", "pre-wait", "post-wait" };
        beginTest ("audit0923 EE-3: " + juce::String (fields[field]) + " survives " + juce::String ("hidden focus then table selection"));
        Fixture0923 f;
        const auto tone = f.scratch.folder.getChildFile ("tone.wav");
        if (! require (writeTone0923 (tone), "create temporary audio")) return;
        auto p = project0923 (tone);
        p.cues()[0].preWaitSeconds = 1.25;
        p.cues()[0].postWaitSeconds = 2.5;
        p.cues()[1].preWaitSeconds = 3.25;
        p.cues()[1].postWaitSeconds = 4.5;
        if (! require (f.open (p), "open saved A/B project clean")) return;
        f.hiddenPeer();
        if (! require (f.tab (ko ("기본")), "open Basics tab")) return;
        const auto original = field == 0 ? p.cues()[0].notes : field == 1 ? p.cues()[0].name
                              : formatTimeMs (field == 2 ? 1.25 : 2.5);
        auto* editor = child0923<juce::TextEditor> (f.inspector(), [&] (const auto& e) { return e.getText() == original; });
        if (! require (editor != nullptr, "find actual Basics editor")) return;
        if (field == 0) f.inspector().showNotes();
        f.focus (*editor);
        dispatch0923();
        if (! require (! f.main->isVisible() && f.main->getPeer() != nullptr, "hidden peer never owns OS focus")) return;
        FocusWitness0923 witness (*editor, f.document());
        const juce::String wanted = field == 0 ? "A edited notes" : field == 1 ? "A edited name" : "7.75";
        editor->selectAll();
        editor->insertTextAtCaret (wanted);
        dispatch0923(); // normal typing messages; still focused, no focus-loss callback
        if (! require (editor->getText() == wanted && ! f.document().isDirty() && ! f.document().canUndo(), "uncommitted input")) return;
        if (! require (f.row (1), "B selected through the real table model")) return;
        const auto beforeDispatch = editor->getText();
        if (! require (! editor->hasKeyboardFocus (true) && witness.calls == 0, "selection refreshed before queued TextEditor focus-loss callback")) return;
        dispatch0923();
        if (! require (witness.calls == 1 && witness.selected == 1, "MessageManager delivered focus loss after B selection")) return;
        const auto a = *f.document().findCueAnywhere (p.cues()[0].id);
        const bool dirty = f.document().isDirty(), undo = f.document().canUndo();
        const bool changed = field == 0 ? a.notes == wanted : field == 1 ? a.name == wanted
                             : std::abs ((field == 2 ? a.preWaitSeconds : a.postWaitSeconds) - 7.75) < 1.0e-9;
        if (! require (f.row (0), "return to A using table")) return;
        dispatch0923();
        logMessage ("OBS EE-3 " + juce::String (fields[field]) + " / " + juce::String ("hidden focus then table selection")
                    + ": before-dispatch editor=" + beforeDispatch + "; callback text=" + witness.text
                    + "; A model changed=" + juce::String ((int) changed) + "; dirty=" + juce::String ((int) dirty)
                    + "; undo=" + juce::String ((int) undo) + "; A redisplayed=" + editor->getText());
        expect (changed, "A must retain the input when B is clicked");
        expect (dirty, "the committed A edit must dirty the saved project");
        expect (undo, "the committed A edit must create project undo history");
        if (field < 2) expectEquals (editor->getText(), wanted, "A inspector must redisplay the edit");
        else expectEquals (editor->getText(), formatTimeMs (7.75), "A inspector must redisplay the wait edit");

        const auto* b = f.document().findCueAnywhere (p.cues()[1].id);
        expect (b != nullptr && b->notes == p.cues()[1].notes && b->name == p.cues()[1].name
                && b->preWaitSeconds == 3.25 && b->postWaitSeconds == 4.5, "B fields stay unchanged");
        if (! require (f.command (CommandIDs::undo), "undo committed field")) return;
        f.row (0);
        // The model's restored value is not pending user input, including a late focus loss.
        editor->focusLost (juce::Component::focusChangedDirectly);
        dispatch0923();
        expectEquals (editor->getText(), original, "undo remains visible after queued focus loss");
        expect (! f.document().canUndo() && f.document().canRedo(), "exactly one history step, no phantom edit on undo");
        if (! require (f.command (CommandIDs::redo), "redo committed field")) return;
        f.row (0);
        editor->focusLost (juce::Component::focusChangedDirectly);
        dispatch0923();
        expectEquals (editor->getText(), field < 2 ? wanted : formatTimeMs (7.75), "redo is not overwritten by stale UI text");
        expect (! f.document().canRedo(), "redo adds no extra history");
    }

    void basicsCancelAndInvalid()
    {
        beginTest ("audit0923 EE-3: Esc, invalid waits and queued text changes");
        Fixture0923 f;
        auto p = project0923 ({});
        p.cues()[0].preWaitSeconds = 1.25;
        if (! require (f.open (p) && f.tab (ko ("기본")), "open basic fields")) return;
        f.hiddenPeer();
        auto* nameField = child0923<juce::TextEditor> (f.inspector(), [] (const auto& e) { return e.getText() == "A"; });
        if (! require (nameField != nullptr, "find name editor")) return;
        f.focus (*nameField);
        nameField->selectAll();
        nameField->insertTextAtCaret ("cancel me");
        nameField->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        dispatch0923();
        f.row (1);
        dispatch0923();
        expectEquals (f.document().cues.get (0).name, juce::String ("A"), "Esc discards the edit");
        expect (! f.document().isDirty() && ! f.document().canUndo(), "Esc creates no dirty state or history");
        f.row (0);
        auto* wait = child0923<juce::TextEditor> (f.inspector(), [] (const auto& e) { return e.getText() == formatTimeMs (1.25); });
        if (! require (wait != nullptr, "find pre-wait editor")) return;
        f.focus (*wait);
        wait->selectAll();
        wait->insertTextAtCaret ("1:2:3:4"); // existing parser rejects more than three time components
        dispatch0923();
        f.row (1);
        dispatch0923();
        expectWithinAbsoluteError (f.document().cues.get (0).preWaitSeconds, 1.25, 1.0e-9, "invalid time keeps the original value");
        expect (! f.document().isDirty() && ! f.document().canUndo(), "invalid input creates no history");
        f.row (0);
        f.focus (*nameField);
        nameField->selectAll();
        nameField->insertTextAtCaret ("A fast edit");
        f.row (1); // no message pump: even onTextChange is still pending
        expectEquals (f.document().cues.get (0).name, juce::String ("A fast edit"), "fast edit commits synchronously before queued text change");
        dispatch0923();
        f.command (CommandIDs::undo);
        f.row (0);
        dispatch0923();
        expectEquals (f.document().cues.get (0).name, juce::String ("A"), "late text-change notification cannot undo the undo");
        expect (! f.document().canUndo() && f.document().canRedo(), "no phantom fast-edit history");
    }

    void basicsModelRefresh()
    {
        beginTest ("audit0923 EE-3: undo and model refresh never commit pending text");
        Fixture0923 f;
        auto p = project0923 ({});
        if (! require (f.open (p) && f.tab (ko ("기본")), "open basic fields")) return;
        f.hiddenPeer();
        f.document().perform ("Change A", [&] { f.document().cues.update (0, [] (Cue& c) { c.name = "A changed"; }); });
        f.row (1);
        auto* editor = child0923<juce::TextEditor> (f.inspector(), [] (const auto& e) { return e.getText() == "B"; });
        if (! require (editor != nullptr, "find B name editor")) return;
        f.focus (*editor);
        editor->selectAll();
        editor->insertTextAtCaret ("B unfinished");
        dispatch0923();
        if (! require (f.command (CommandIDs::undo), "undo model while B input is pending")) return;
        dispatch0923();
        expectEquals (f.document().cues.get (0).name, juce::String ("A"), "A remains undone");
        expectEquals (f.document().cues.get (1).name, juce::String ("B"), "model restoration does not commit B");
        expect (! f.document().canUndo() && f.document().canRedo(), "restore adds no edit and preserves redo");
        if (! require (f.command (CommandIDs::redo), "redo is still available")) return;
        f.row (1);
        dispatch0923();
        expectEquals (f.document().cues.get (0).name, juce::String ("A changed"), "redo survives late focus loss");
        expectEquals (f.document().cues.get (1).name, juce::String ("B"), "late focus loss leaves B unchanged");
        editor->selectAll();
        editor->insertTextAtCaret ("B stale input");
        dispatch0923();
        f.document().cues.update (1, [] (Cue& c) { c.name = "B external edit"; });
        editor->focusLost (juce::Component::focusChangedDirectly);
        dispatch0923();
        expectEquals (editor->getText(), juce::String ("B external edit"), "model refresh synchronises and clears user input");
        expectEquals (f.document().cues.get (1).name, juce::String ("B external edit"), "focus loss cannot overwrite an external edit");
    }

    void levels()
    {
        beginTest ("audit0923 EE-2: matrix typing preserves playing B after " + juce::String ("hidden focus then table selection"));
        Fixture0923 f;
        const auto tone = f.scratch.folder.getChildFile ("tone.wav");
        if (! require (writeTone0923 (tone), "create temporary audio")) return;
        auto p = project0923 (tone);
        p.cues()[1].gainDb = -30.0;
        p.cues()[1].levels.outputDb[1] = -12.0; // identify whole-matrix copying, outside the edited cell
        if (! require (f.open (p), "open saved same-channel A/B project")) return;
        f.hiddenPeer();
        if (! require (f.row (1) && f.key (juce::KeyPress ('V', 0, 'v')), "select B and dispatch V through ShortcutRouter")) return;
        dispatch0923();
        if (! require (f.engine.isPlaying (p.cues()[1].id), "B is playing")) return;
        const auto before = f.rms();
        AudioEngine::LiveState initialLive;
        if (! require (f.engine.getLiveState (p.cues()[1].id, initialLive) && initialLive.gainDb == -30.0
                       && initialLive.levels == p.cues()[1].levels && before[0] > 0.01 && before[0] < 0.012,
                       "B starts with the intended -30 dB live state and audible output")) return;
        if (! require (f.row (0) && f.tab (ko ("레벨")), "select A and Levels tab")) return;
        auto* grid = child0923<LevelMatrixComponent> (f.inspector(), [] (const auto& g) { return g.getMatrix().numInputs() == 2; });
        if (! require (grid != nullptr && grid->getMainDb() == 0.0, "A grid at 0 dB")) return;
        const juce::Point<int> cell (LevelMatrixComponent::headerWidth + LevelMatrixComponent::gap
                                      + LevelMatrixComponent::cellWidth + LevelMatrixComponent::gap + LevelMatrixComponent::cellWidth / 2,
                                    LevelMatrixComponent::headerHeight + LevelMatrixComponent::gap
                                      + LevelMatrixComponent::cellHeight + LevelMatrixComponent::gap + LevelMatrixComponent::cellHeight / 2);
        grid->mouseDown (mouse0923 (*grid, cell, cell));
        grid->mouseUp (mouse0923 (*grid, cell, cell));
        if (! require (ReopenLastProjectTestAccess::keyboard (*f.main).keyPressed (juce::KeyPress ('6', 0, '6'), grid), "type 6 through ShortcutRouter")) return;
        auto* editor = child0923<juce::TextEditor> (*grid);
        if (! require (editor != nullptr && editor->getText() == "6", "matrix typing active")) return;
        f.focus (*editor);
        FocusWitness0923 witness (*editor, f.document());
        dispatch0923();
        if (! require (f.row (1) && ! editor->hasKeyboardFocus (true) && witness.calls == 0, "focus loss queued, B synchronously selected")) return;
        const auto staleMain = grid->getMainDb();
        dispatch0923();
        if (! require (witness.calls == 1 && witness.selected == 1, "deliver matrix focus loss after B selection")) return;
        const auto a = *f.document().findCueAnywhere (p.cues()[0].id);
        const auto b = *f.document().findCueAnywhere (p.cues()[1].id);
        AudioEngine::LiveState live;
        if (! require (f.engine.getLiveState (b.id, live), "read B real engine state")) return;
        const auto after = f.rms();
        logMessage ("OBS EE-2 / " + juce::String ("hidden focus then table selection") + ": A cross11=" + juce::String (a.levels.crosspointDb[0][0])
                    + "; stale grid main=" + juce::String (staleMain) + "; B document/live gain=" + juce::String (b.gainDb) + "/" + juce::String (live.gainDb)
                    + "; B cross11=" + juce::String (b.levels.crosspointDb[0][0]) + "; B output2=" + juce::String (b.levels.outputDb[1])
                    + "; RMS 1 " + juce::String (before[0], 6) + " -> " + juce::String (after[0], 6)
                    + "; RMS 2 " + juce::String (before[1], 6) + " -> " + juce::String (after[1], 6));
        expectWithinAbsoluteError (a.levels.crosspointDb[0][0], -6.0, 1.0e-9, "unsigned 6 must commit -6 dB to A");
        expectWithinAbsoluteError (b.gainDb, -30.0, 1.0e-9, "B document gain must remain -30 dB");
        expect (b.levels == p.cues()[1].levels, "B entire document matrix must stay unchanged");
        expectWithinAbsoluteError (live.gainDb, -30.0, 1.0e-9, "B live gain must remain -30 dB");
        expect (live.levels == p.cues()[1].levels, "B entire live matrix must stay unchanged");
        expectWithinAbsoluteError (after[0], before[0], 0.0002, "B output 1 remains at its previous audible level");
        expectWithinAbsoluteError (after[1], before[1], 0.0002, "B output 2 remains at its previous audible level");
        expectWithinAbsoluteError (grid->getMainDb(), -30.0, 1.0e-9, "grid advances to selected B after committing A");
    }

    static juce::Point<int> matrixCell()
    {
        return { LevelMatrixComponent::headerWidth + LevelMatrixComponent::gap
                 + LevelMatrixComponent::cellWidth + LevelMatrixComponent::gap + LevelMatrixComponent::cellWidth / 2,
                 LevelMatrixComponent::headerHeight + LevelMatrixComponent::gap
                 + LevelMatrixComponent::cellHeight + LevelMatrixComponent::gap + LevelMatrixComponent::cellHeight / 2 };
    }

    void levelsCompletion (bool cancel)
    {
        beginTest ("audit0923 EE-2: matrix " + juce::String (cancel ? "Esc" : "Enter") + " after selection moves");
        Fixture0923 f;
        auto p = project0923 ({});
        p.cues()[1].gainDb = -30.0;
        if (! require (f.open (p) && f.tab (ko ("레벨")), "open level matrix")) return;
        f.hiddenPeer();
        auto* grid = child0923<LevelMatrixComponent> (f.inspector());
        if (! require (grid != nullptr, "find level matrix")) return;
        const auto cell = matrixCell();
        grid->mouseDown (mouse0923 (*grid, cell, cell));
        grid->mouseUp (mouse0923 (*grid, cell, cell));
        if (! require (grid->keyPressed (juce::KeyPress ('6', 0, '6')), "begin numeric entry")) return;
        auto* editor = child0923<juce::TextEditor> (*grid);
        if (! require (editor != nullptr, "numeric editor exists")) return;
        f.row (1); // selection may also move through auto-follow while typing
        editor->keyPressed (juce::KeyPress (cancel ? juce::KeyPress::escapeKey : juce::KeyPress::returnKey));
        dispatch0923();
        expect (child0923<juce::TextEditor> (*grid) == nullptr, "numeric session ended");
        expectWithinAbsoluteError (f.document().cues.get (0).levels.crosspointDb[0][0], cancel ? 0.0 : -6.0, 1.0e-9, "completion applies only to A");
        expect (f.document().cues.get (1).levels == p.cues()[1].levels, "B matrix unchanged");
        expectWithinAbsoluteError (f.document().cues.get (1).gainDb, -30.0, 1.0e-9, "B gain unchanged");
        expectWithinAbsoluteError (grid->getMainDb(), -30.0, 1.0e-9, "completion displays B");
        expect (f.document().isDirty() == ! cancel && f.document().canUndo() == ! cancel, "Esc creates no edit; Enter creates one");
        if (! cancel)
        {
            f.command (CommandIDs::undo);
            expect (f.document().cues.get (0).levels == p.cues()[0].levels, "matrix undo restores A");
            expect (! f.document().canUndo() && f.document().canRedo(), "single matrix history step");
        }
    }

    void levelsDrag()
    {
        beginTest ("audit0923 EE-2: existing matrix drag protection survives selection change");
        Fixture0923 f;
        auto p = project0923 ({});
        p.cues()[1].gainDb = -30.0;
        if (! require (f.open (p) && f.tab (ko ("레벨")), "open level matrix")) return;
        f.hiddenPeer();
        auto* grid = child0923<LevelMatrixComponent> (f.inspector());
        if (! require (grid != nullptr, "find level matrix")) return;
        const auto start = matrixCell();
        const auto end = start.translated (0, -24);
        grid->mouseDown (mouse0923 (*grid, start, start));
        grid->mouseDrag (mouse0923 (*grid, end, start, true));
        f.row (1);
        grid->mouseUp (mouse0923 (*grid, end, start, true));
        expectWithinAbsoluteError (f.document().cues.get (0).levels.crosspointDb[0][0], 6.0, 1.0e-9, "drag commits to A");
        expect (f.document().cues.get (1).levels == p.cues()[1].levels, "drag preserves B");
        expectWithinAbsoluteError (grid->getMainDb(), -30.0, 1.0e-9, "finished drag displays B");
    }

    void patchUndo (bool redo = false, bool startBeforeUndo = false)
    {
        beginTest ("audit0923 EC-3: " + juce::String (startBeforeUndo ? "started" : "LOAD")
                   + " patch change then " + (redo ? "undo/redo" : "undo"));
        Fixture0923 f (4);
        const auto tone = f.scratch.folder.getChildFile ("tone.wav");
        if (! require (writeTone0923 (tone), "create temporary stereo audio")) return;
        auto p = project0923 (tone, 1);
        auto a = p.patches[0];
        a.name = "Patch A";
        auto b = AudioPatch::makeDefault ("Patch B");
        b.numCueOutputs = 2;
        for (int in = 0; in < 2; ++in)
            for (int out = 0; out < 4; ++out)
            {
                a.setRouting (in, out, out == in ? 0.0 : LevelMatrix::silentDb);
                b.setRouting (in, out, out == in + 2 ? 0.0 : LevelMatrix::silentDb);
            }
        p.patches = { a, b };
        if (! require (f.open (p), "open 4-output project with two 2-channel patches")) return;
        f.hiddenPeer();
        if (! require (f.tab (ko ("레벨")) && f.command (CommandIDs::loadCue) && f.engine.isLoaded (p.cues()[0].id), "manually LOAD on A")) return;
        auto* combo = child0923<juce::ComboBox> (f.inspector(), [] (const auto& c) { return c.getText().startsWith ("Patch A"); });
        if (! require (combo != nullptr, "actual inspector patch selector")) return;
        combo->setSelectedId (2, juce::sendNotificationSync); // actual LevelsPanel::commitPatch
        if (! require (f.document().cues.get (0).patchId == b.id && f.engine.isLoaded (p.cues()[0].id), "patch B edit reloaded the player")) return;
        if (startBeforeUndo)
            if (! require (f.command (CommandIDs::go) && f.engine.isPlaying (p.cues()[0].id), "start B before history restore")) return;
        if (! require (f.key (juce::KeyPress ('Z', juce::ModifierKeys::ctrlModifier, 'z')), "Ctrl+Z through ShortcutRouter")) return;
        dispatch0923();
        if (! require (f.document().patchForCue (f.document().cues.get (0)).id == a.id && combo->getSelectedId() == 1, "undo shows patch A in document and inspector")) return;
        if (redo)
        {
            if (! require (f.command (CommandIDs::redo), "redo patch B")) return;
            dispatch0923();
            expect (f.document().cues.get (0).patchId == b.id && combo->getSelectedId() == 2, "redo restores patch B in model and inspector");
        }
        if (! startBeforeUndo)
        {
            if (! require (f.engine.isLoaded (p.cues()[0].id), "history preserves LOAD state")) return;
            if (! require (f.command (CommandIDs::go) && f.engine.isPlaying (p.cues()[0].id), "GO starts the restored loaded cue")) return;
        }
        else
            expect (f.engine.isPlaying (p.cues()[0].id) && ! f.engine.isLoaded (p.cues()[0].id), "started instance stays running");
        const auto heard = f.rms();
        logMessage ("OBS EC-3: RMS outputs 1..4=" + juce::String (heard[0], 6) + ", "
                    + juce::String (heard[1], 6) + ", " + juce::String (heard[2], 6) + ", " + juce::String (heard[3], 6));
        if (redo || startBeforeUndo)
        {
            expectWithinAbsoluteError (heard[0], 0.0, 1.0e-7, "patch B keeps output 1 silent");
            expectWithinAbsoluteError (heard[1], 0.0, 1.0e-7, "patch B keeps output 2 silent");
            expectGreaterThan (heard[2], 0.3, "patch B feeds output 3");
            expectGreaterThan (heard[3], 0.3, "patch B feeds output 4");
            return;
        }
        expectGreaterThan (heard[0], 0.3, "restored A must feed output 1");
        expectGreaterThan (heard[1], 0.3, "restored A must feed output 2");
        expectWithinAbsoluteError (heard[2], 0.0, 1.0e-7, "output 3 must stay silent after undo");
        expectWithinAbsoluteError (heard[3], 0.0, 1.0e-7, "output 4 must stay silent after undo");
    }

    void timelineSwitch (int nextChildren)
    {
        beginTest ("audit0923 EE-1: drag then Ctrl+PageDown to " + juce::String (nextChildren) + " children");
        Fixture0923 f;
        auto p = project0923 ({}, 0);
        auto makeGroup = [] (int count, int number)
        {
            CueContainer list;
            list.name = "Timeline " + juce::String (number);
            Cue group;
            group.type = CueType::group;
            group.name = list.name;
            group.number = juce::String (number);
            group.group.mode = GroupMode::timeline;
            list.cues.push_back (group);
            for (int i = 0; i < count; ++i)
            {
                Cue child;
                child.number = juce::String (number + i + 1);
                child.name = "Child " + child.number;
                child.parentId = group.id;
                child.preWaitSeconds = 1.0 + i;
                child.durationSeconds = 10.0;
                list.cues.push_back (child);
            }
            return list;
        };
        p.lists = { makeGroup (3, 1), makeGroup (nextChildren, 10) };
        const auto secondGroup = makeGroup (2, 30);
        p.cues().insert (p.cues().end(), secondGroup.cues.begin(), secondGroup.cues.end());
        if (! require (f.open (p), "open two timeline lists")) return;
        f.hiddenPeer();
        // Remember the group selection in the other list too.
        f.document().setActiveContainer (1);
        f.row (0);
        f.document().setActiveContainer (0);
        if (! require (f.row (0) && f.tab (ko ("그룹")), "show first timeline group")) return;
        auto* timeline = child0923<juce::Component> (f.inspector(), [] (const auto& c)
        {
            return c.getProperties().contains ("shortcutScope")
                && static_cast<int> (c.getProperties()["shortcutScope"]) == static_cast<int> (ShortcutScope::groupTimeline);
        });
        if (! require (timeline != nullptr && timeline->getHeight() > 60, "find timeline component")) return;
        std::vector<std::pair<juce::Uuid, double>> before;
        f.document().forEachList ([&] (const CueList& list)
        {
            for (const auto& c : list.getAll()) before.emplace_back (c.id, c.preWaitSeconds);
        });
        const int rowHeight = juce::jlimit (16, 26, (timeline->getHeight() - 14) / 3);
        const juce::Point<int> start (220, 14 + 2 * rowHeight + rowHeight / 2);
        timeline->mouseDown (mouse0923 (*timeline, start, start)); // child at index 3
        if (! require (ReopenLastProjectTestAccess::keyboard (*f.main).keyPressed (
                juce::KeyPress (juce::KeyPress::pageDownKey, juce::ModifierKeys::ctrlModifier, 0), timeline),
                "Ctrl+PageDown through ShortcutRouter")) return;
        if (! require (f.document().getActiveContainer() == 1 && f.document().cues.getSelectedIndex() == 0,
                       "other list's group is selected before mouse release")) return;
        const auto end = start.translated (100, 0);
        timeline->mouseDrag (mouse0923 (*timeline, end, start, true));
        timeline->mouseUp (mouse0923 (*timeline, end, start, true));
        for (const auto& [id, wait] : before)
        {
            const auto* cue = f.document().findCueAnywhere (id);
            expect (cue != nullptr);
            if (cue != nullptr) expectWithinAbsoluteError (cue->preWaitSeconds, wait, 1.0e-9, "every cue keeps its pre-wait");
        }
        expect (! f.document().isDirty(), "cancelled drag must not dirty the project");
        expect (! f.document().canUndo(), "cancelled drag must not create history");

        beginTest ("audit0923 EE-1: switching groups cancels drag");
        f.document().setActiveContainer (0);
        f.row (0);
        timeline->mouseDown (mouse0923 (*timeline, start, start));
        if (! require (f.row (4), "select another group in the same list")) return;
        timeline->mouseDrag (mouse0923 (*timeline, end, start, true));
        timeline->mouseUp (mouse0923 (*timeline, end, start, true));
        for (const auto& [id, wait] : before)
            expectWithinAbsoluteError (f.document().findCueAnywhere (id)->preWaitSeconds, wait, 1.0e-9, "group switch preserves every wait");
        expect (! f.document().isDirty() && ! f.document().canUndo(), "group switch creates no edit");

        beginTest ("audit0923 EE-1: ordinary drag and keyboard nudge remain undoable");
        f.row (0);
        timeline->mouseDown (mouse0923 (*timeline, start, start));
        timeline->mouseDrag (mouse0923 (*timeline, end, start, true));
        timeline->mouseUp (mouse0923 (*timeline, end, start, true));
        expectGreaterThan (f.document().cues.get (3).preWaitSeconds, 3.0, "normal drag changes the intended child");
        if (! require (f.command (CommandIDs::undo), "undo timeline drag")) return;
        expectWithinAbsoluteError (f.document().cues.get (3).preWaitSeconds, 3.0, 1.0e-9, "undo restores original child wait");
        expect (! f.document().canUndo(), "one drag creates one history step");
        if (! require (timeline->keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0)), "Alt+Right nudge")) return;
        expectWithinAbsoluteError (f.document().cues.get (3).preWaitSeconds, 3.1, 1.0e-9, "normal keyboard nudge still works");
        f.command (CommandIDs::undo);
        expectWithinAbsoluteError (f.document().cues.get (3).preWaitSeconds, 3.0, 1.0e-9, "keyboard nudge remains undoable");
    }
};
static AuditFix0923UiTests auditFix0923UiTests;
}
}
