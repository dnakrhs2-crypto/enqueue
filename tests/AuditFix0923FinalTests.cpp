#include "MainComponentTestAccess.h"
#include "ui/GoCueLookAndFeel.h"

#include <algorithm>
#include <cmath>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// Only the EB-1 cart and EE-3 input/selection helpers from
// audit0923_ref/Audit0923EnqCueTests.cpp and Audit0923EnqTests.cpp are used here.
// As in AuditFix0923UiTests, the peer has no native window; visible-peer variants
// are omitted so the real JUCE event ordering cannot disturb another application.
namespace gocue::tests
{
namespace
{
constexpr double sampleRate = 44100.0;
constexpr int blockSize = 512;

template <class Tag, typename Tag::Type member>
struct MemberAccess { friend typename Tag::Type memberOf (Tag) { return member; } };
struct DocumentMember
{
    using Type = ProjectDocument MainComponent::*;
    friend Type memberOf (DocumentMember);
};
template struct MemberAccess<DocumentMember, &MainComponent::document>;

template <typename T, typename Predicate>
T* findChild (juce::Component& root, Predicate matches)
{
    if (auto* found = dynamic_cast<T*> (&root); found != nullptr && matches (*found)) return found;
    for (auto* child : root.getChildren())
        if (auto* found = findChild<T> (*child, matches)) return found;
    return nullptr;
}
template <typename T> T* findChild (juce::Component& root)
{
    return findChild<T> (root, [] (const T&) { return true; });
}

void drainMessages()
{
   #if JUCE_WINDOWS
    MSG message {};
    for (int count = 0; count < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++count)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}

void pumpTimers()
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 60.0;
    do
    {
        drainMessages();
        juce::Timer::callPendingTimersSynchronously();
        juce::Thread::sleep (1);
    }
    while (juce::Time::getMillisecondCounterHiRes() < until);
}

struct ScratchDirectory
{
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("AuditFix0923Final-" + juce::Uuid().toString());
    ScratchDirectory() { folder.createDirectory(); }
    ~ScratchDirectory()
    {
        if (folder.isAChildOf (base) && folder.getFileName().startsWith ("AuditFix0923Final-"))
            folder.deleteRecursively();
    }
};

bool writeTone (const juce::File& file)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (! stream) return false;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate)
                                         .withNumChannels (2).withBitsPerSample (16));
    if (! writer) return false;
    juce::AudioBuffer<float> block (2, 44100);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < block.getNumSamples(); ++i)
            block.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sampleRate));
    for (int second = 0; second < 30; ++second)
        if (! writer->writeFromAudioSampleBuffer (block, 0, block.getNumSamples())) return false;
    return true;
}

class HiddenFocusPeer final : public juce::ComponentPeer
{
public:
    HiddenFocusPeer (juce::Component& component, int flags) : ComponentPeer (component, flags) {}
    void* getNativeHandle() const override { return nullptr; }
    void setVisible (bool) override {}
    void setTitle (const juce::String&) override {}
    void setBounds (const juce::Rectangle<int>& value, bool) override { bounds = value; }
    juce::Rectangle<int> getBounds() const override { return bounds; }
    juce::Point<float> localToGlobal (juce::Point<float> p) override { return p + bounds.getPosition().toFloat(); }
    juce::Point<float> globalToLocal (juce::Point<float> p) override { return p - bounds.getPosition().toFloat(); }
    void setMinimised (bool) override {}
    bool isMinimised() const override { return false; }
    bool isShowing() const override { return false; }
    void setFullScreen (bool) override {}
    bool isFullScreen() const override { return false; }
    void setIcon (const juce::Image&) override {}
    bool contains (juce::Point<int> p, bool) const override { return bounds.withZeroOrigin().contains (p); }
    OptionalBorderSize getFrameSizeIfPresent() const override { return OptionalBorderSize (juce::BorderSize<int>()); }
    juce::BorderSize<int> getFrameSize() const override { return {}; }
    bool setAlwaysOnTop (bool) override { return false; }
    void toFront (bool) override {}
    void toBehind (juce::ComponentPeer*) override {}
    bool isFocused() const override { return focused; }
    void grabFocus() override { focused = true; }
    void repaint (const juce::Rectangle<int>&) override {}
    void performAnyPendingRepaintsNow() override {}
    void setAlpha (float) override {}
    juce::StringArray getAvailableRenderingEngines() override { return {}; }
    void textInputRequired (juce::Point<int>, juce::TextInputTarget&) override {}
private:
    juce::Rectangle<int> bounds;
    bool focused = false;
};

class HiddenMain final : public MainComponent
{
public:
    using MainComponent::MainComponent;
private:
    juce::ComponentPeer* createNewPeer (int peerFlags, void*) override { return new HiddenFocusPeer (*this, peerFlags); }
};

struct Fixture
{
    ScratchDirectory scratch;
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options result;
        result.millisecondsBeforeSaving = -1;
        return result;
    }
    juce::PropertiesFile storage { scratch.folder.getChildFile ("test.settings"), options() };
    AppSettings settings { storage };
    AudioEngine engine { 0 };
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    juce::AudioBuffer<float> out { 2, blockSize };

    Fixture()
    {
        engine.prepare (sampleRate, blockSize);
        main = std::make_unique<HiddenMain> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        main->setSize (1541, 980);
    }
    ~Fixture()
    {
        main.reset();
        drainMessages();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        drainMessages();
        engine.shutdown();
        storage.saveIfNeeded();
    }
    ProjectDocument& document() { return (*main).*memberOf (DocumentMember {}); }
    CueController& controller() { return ReopenLastProjectTestAccess::controller (*main); }
    CueInspector& inspector() { return *findChild<CueInspector> (*main); }
    CueCartView& cart() { return *findChild<CueCartView> (*main); }
    juce::TableListBox& table() { return *findChild<juce::TableListBox> (*findChild<CueTable> (*main)); }
    void render()
    {
        for (int i = 0; i < 8; ++i) engine.renderBlock (out, blockSize);
        engine.reapFinishedPlayers();
    }
    bool open (const Project& project)
    {
        const auto file = scratch.folder.getChildFile ("show.enqueue");
        if (ProjectSerializer::save (project, file).failed()) return false;
        main->openProjectFile (file, false);
        render();
        main->addToDesktop (0);
        main->setVisible (true); // logical visibility only: HiddenFocusPeer creates no HWND
        drainMessages();
        return main->getProjectFile() == file && ! document().isDirty();
    }
    void renderUntil (double until)
    {
        double renderedUntil = controller().clock();
        while (controller().clock() < until)
        {
            const double now = controller().clock();
            while (renderedUntil + blockSize / sampleRate <= now)
            {
                engine.renderBlock (out, blockSize);
                renderedUntil += blockSize / sampleRate;
                engine.reapFinishedPlayers();
            }
            drainMessages();
            juce::Timer::callPendingTimersSynchronously();
            juce::Thread::sleep (1);
        }
        juce::Timer::callPendingTimersSynchronously();
    }
};

Project projectWith (const juce::File& file, bool cart = true)
{
    Project p;
    p.name = "AuditFix0923 Final";
    p.settings.autoBackup = false;
    p.settings.backupBeforeSave = false;
    p.settings.copyFilesIntoProject = false;
    p.settings.autoLoadNewCues = false;
    auto& list = p.ensureMainList();
    list.isCart = cart;
    list.cartRows = 1;
    list.cartCols = 2;
    for (int i = 0; i < 2; ++i)
    {
        Cue cue;
        cue.name = i == 0 ? "A" : "B";
        cue.number = juce::String (i + 1);
        cue.file = file;
        cue.durationSeconds = cue.audio.endSeconds = 30.0;
        cue.numChannels = 2;
        cue.autoLoad = false;
        cue.fadeOutMs = 0;
        list.cues.push_back (cue);
    }
    return p;
}

int startsOf (const std::vector<CueController::RecordedStart>& starts, const juce::Uuid& id)
{
    return (int) std::count_if (starts.begin(), starts.end(), [&] (const auto& start) { return start.cueId == id; });
}

juce::Point<int> cartPoint (CueCartView& cart, int slot)
{
    return { cart.getWidth() * (2 * slot + 1) / 4, cart.getHeight() / 2 };
}

juce::MouseEvent mouse (juce::Component& component, juce::Point<int> point, bool right = false)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point.toFloat(),
             right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier,
             1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component, now, point.toFloat(), now, 1, false };
}

using SourceType = juce::MouseInputSource::InputSourceType;

// Reference click0923, split at down/up so queued focus loss and timers can run
// during a press. The windowless peer exercises JUCE hit testing, real source
// button state and the actual TableListBox row without OS input or an HWND.
class PointerPress
{
public:
    PointerPress (juce::Component& target, juce::Point<int> position,
                  int modifiers = juce::ModifierKeys::leftButtonModifier,
                  SourceType type = SourceType::mouse, int finger = 0)
        : modifierState (juce::ModifierKeys::currentModifiers, juce::ModifierKeys (modifiers).withoutMouseButtons()),
          top (target.getTopLevelComponent()), point (top->getLocalPoint (&target, position).toFloat()),
          source (type), touchIndex (finger)
    {
        static juce::int64 eventTime = juce::Time::currentTimeMillis();
        eventTime = juce::jmax (eventTime + 1000, juce::Time::currentTimeMillis());
        time = eventTime;
        send ({});
        send (modifiers);
    }
    ~PointerPress() { release(); }
    void buttons (int modifiers) { send (modifiers); }
    void release()
    {
        if (! released)
        {
            released = true;
            send ({});
        }
    }
    void releaseAt (juce::Component& target, juce::Point<int> position)
    {
        point = top->getLocalPoint (&target, position).toFloat();
        release();
    }
private:
    void send (juce::ModifierKeys modifiers)
    {
        if (top != nullptr && top->getPeer() != nullptr)
            top->getPeer()->handleMouseEvent (source, point, modifiers, 0.0f, 0.0f, time++, {}, touchIndex);
    }
    juce::ScopedValueSetter<juce::ModifierKeys> modifierState;
    juce::Component::SafePointer<juce::Component> top;
    juce::Point<float> point;
    SourceType source;
    int touchIndex;
    juce::int64 time = 0;
    bool released = false;
};

void clickRow (juce::TableListBox& table, int column, int row = 1, int modifiers = 0)
{
    PointerPress press (table, table.getCellPosition (column, row, true).getCentre(),
                        juce::ModifierKeys::leftButtonModifier | modifiers);
    press.release();
}

struct FocusWitness final : juce::TextEditor::Listener
{
    explicit FocusWitness (juce::TextEditor& e) : editor (e) { editor.addListener (this); }
    ~FocusWitness() override { editor.removeListener (this); }
    void textEditorFocusLost (juce::TextEditor&) override { ++losses; }
    juce::TextEditor& editor;
    int losses = 0;
};

struct QuietAlerts final : juce::LookAndFeel_V4
{
    QuietAlerts() : previous (&getDefaultLookAndFeel()) { setDefaultLookAndFeel (this); }
    ~QuietAlerts() override { setDefaultLookAndFeel (previous); }
    void playAlertSound() override { ++alerts; }
    juce::LookAndFeel* previous;
    int alerts = 0;
};

juce::Point<int> statusPoint (juce::TableListBox& table)
{
    return table.getCellPosition (CueTable::colStatus, 1, true).getCentre();
}

class AuditFix0923FinalTests final : public juce::UnitTest
{
public:
    AuditFix0923FinalTests() : UnitTest ("AuditFix0923 Final", "Enqueue") {}
    void runTest() override
    {
        beginTest ("temporary audio for cart and inspector integration");
        const juce::TemporaryFile tone (".wav");
        if (! require (writeTone (tone.getFile()), "write temporary tone")) return;
        cartStop (tone.getFile(), false);
        cartStop (tone.getFile(), true);
        cartRelease (tone.getFile(), 0);
        cartRelease (tone.getFile(), 1);
        cartRelease (tone.getFile(), 2);
        cartDeletedOnSelection();
        for (int column : { CueTable::colName, CueTable::colNumber, CueTable::colStatus, CueTable::colContinue })
            tableSelection (tone.getFile(), column);
        tableSelection (tone.getFile(), CueTable::colName, juce::ModifierKeys::ctrlModifier);
        tableSelection (tone.getFile(), CueTable::colName, juce::ModifierKeys::shiftModifier);
        for (const auto source : { SourceType::mouse, SourceType::touch })
            for (const bool processMessages : { false, true })
                delayedSelection (source, processMessages);
        keyboardCommit (false);
        keyboardCommit (true);
        enterWhilePressed();
        allPointersReleased (false);
        allPointersReleased (true);
        for (int mode = 0; mode < 4; ++mode)
            cancelledCommit (mode);
        conflictingCommit();
        inactiveListCommit();
        for (int mode = 0; mode < 5; ++mode)
            cartSelectionMutation (mode);
    }
private:
    bool require (bool condition, const juce::String& reason)
    {
        expect (condition, "Fixture prerequisite: " + reason);
        return condition;
    }
    juce::TextEditor* editNumber (Fixture& f)
    {
        auto* editor = findChild<juce::TextEditor> (f.inspector(), [] (const auto& e) { return e.isShowing() && e.getText() == "1"; });
        if (! require (editor != nullptr, "find A's actual number editor")) return nullptr;
        editor->grabKeyboardFocus();
        drainMessages();
        if (! require (editor->hasKeyboardFocus (false) && f.main->getWindowHandle() == nullptr
                       && ! f.main->getPeer()->isShowing(), "JUCE focus with no native window")) return nullptr;
        editor->selectAll();
        editor->insertTextAtCaret ("3");
        drainMessages();
        if (! require (editor->getText() == "3" && f.document().cues.get (0).number == "1"
                       && ! f.document().isDirty(), "number input is still uncommitted")) return nullptr;
        return editor;
    }
    void expectReordered (Fixture& f, const Project& p)
    {
        expect (f.document().cues.get (0).id == p.cues()[1].id && f.document().cues.get (1).id == p.cues()[0].id,
                "the real inspector commit must reorder A/B to B/A after release");
        expectEquals (f.document().cues.get (1).number, juce::String ("3"));
    }
    void expectHeld (Fixture& f, const Project& p)
    {
        expect (f.document().cues.get (0).id == p.cues()[0].id && f.document().cues.get (1).id == p.cues()[1].id,
                "holding a pointer must preserve A/B row order");
        expectEquals (f.document().cues.get (0).number, juce::String ("1"), "A number waits for every pointer to be released");
        expect (! f.document().canUndo(), "deferred input creates no history before release");
    }
    void cartStop (const juce::File& tone, bool pending)
    {
        beginTest (pending ? "cart right-click cancels B pre-wait across number reorder, leaving A playing"
                           : "cart right-click stops B across number reorder, leaving A playing");
        Fixture f;
        auto p = projectWith (tone);
        const juce::KeyPress hotkey (juce::KeyPress::F7Key);
        p.cues()[1].hotkey = hotkey.getTextDescription();
        p.cues()[1].preWaitSeconds = pending ? 10.0 : 0.0;
        if (! require (f.open (p), "open temporary cart")) return;
        const auto a = p.cues()[0].id, b = p.cues()[1].id;
        auto& controller = f.controller();
        if (! require (controller.fire (a) == CueController::GoResult::started, "start A")) return;
        f.render();
        const auto aOrder = f.engine.getStartOrder (a);
        controller.startRecording();
        const double triggeredAt = controller.clock();
        if (! require (controller.handleHotkey (hotkey), "start or schedule B through the real hotkey")) return;
        f.render();
        if (! require (pending ? controller.hasPendingFor (b) : f.engine.isPlaying (b), "B is pending or playing")) return;
        if (editNumber (f) == nullptr) return;
        auto& cart = f.cart();
        if (! require ((bool) cart.onStop && cart.isShowing(), "MainComponent's actual cart stop callback")) return;
        PointerPress press (cart, cartPoint (cart, 1), juce::ModifierKeys::rightButtonModifier);
        expect (! controller.hasPendingFor (b), "right-click must cancel B's pending start immediately");
        pumpTimers();
        expectHeld (f, p);
        press.release();
        pumpTimers();
        expectReordered (f, p);
        f.render();
        expect (! f.engine.isPlaying (b), "right-click must stop B");
        expect (f.engine.isPlaying (a) && f.engine.getStartOrder (a) == aOrder, "A must be unaffected");
        if (pending) f.renderUntil (triggeredAt + 10.3);
        const auto starts = controller.stopRecording();
        expectEquals (startsOf (starts, b), pending ? 0 : 1, "B must never start again after right-click");
        expectEquals (startsOf (starts, a), 0, "A must not restart");
        expect (! f.engine.isPlaying (b) && f.engine.isPlaying (a), "only A remains playing after B's deadline");
    }
    void cartRelease (const juce::File& tone, int mode)
    {
        beginTest (mode == 0 ? "cart release in the pressed slot fires B exactly once, then commits A number"
                   : mode == 1 ? "cart release in another slot does nothing"
                               : "cart release after pressed B is removed does nothing");
        Fixture f;
        const auto p = projectWith (tone);
        if (! require (f.open (p), "open temporary cart")) return;
        if (editNumber (f) == nullptr) return;
        auto& cart = f.cart();
        f.controller().startRecording();
        PointerPress press (cart, cartPoint (cart, 1));
        pumpTimers();
        expectHeld (f, p);
        if (mode == 2)
        {
            f.document().cues.remove (f.document().cues.indexOf (p.cues()[1].id));
            auto replacement = p.cues()[1].duplicated();
            replacement.name = "C";
            f.document().cues.add (replacement); // C occupies the old pressed slot
        }
        press.releaseAt (cart, cartPoint (cart, mode == 1 ? 0 : 1));
        cart.mouseUp (mouse (cart, cartPoint (cart, 0))); // no second trigger without another press
        pumpTimers();
        if (mode != 2) expectReordered (f, p);
        expectEquals (f.document().findCueAnywhere (p.cues()[0].id)->number, juce::String ("3"), "A commits after release");
        f.render();
        const auto starts = f.controller().stopRecording();
        expectEquals ((int) starts.size(), mode == 0 ? 1 : 0, "a replacement cue must not inherit the press");
        expectEquals (startsOf (starts, p.cues()[0].id), 0, "A must never fire");
        expectEquals (startsOf (starts, p.cues()[1].id), mode == 0 ? 1 : 0, "only release in the pressed slot fires the surviving B");
        expect (! f.engine.isPlaying (p.cues()[0].id), "A remains silent");
        expect (f.engine.isPlaying (p.cues()[1].id) == (mode == 0), "B plays only after its own valid release");
        expectEquals (f.controller().getNumPending(), 0, "cancelled clicks must not schedule anything");
    }
    void cartDeletedOnSelection()
    {
        beginTest ("cart right-click does nothing if selection notification removes the clicked cue");
        CueList cues;
        Cue a, b, c;
        for (const auto& cue : { a, b, c }) cues.add (cue);
        cues.setSelectedIndex (0);
        juce::AudioFormatManager formats;
        CueCartView cart (cues, formats);
        cart.setGrid (1, 2);
        cart.setSize (600, 200);
        struct RemoveClicked final : CueList::Listener
        {
            RemoveClicked (CueList& list, const juce::Uuid& cue) : model (list), id (cue) { model.addListener (this); }
            ~RemoveClicked() override { model.removeListener (this); }
            void cueSelectionChanged (int index) override
            {
                if (model.isValidIndex (index) && model.get (index).id == id) model.remove (index);
            }
            CueList& model;
            juce::Uuid id;
        } removeClicked (cues, b.id);
        int stops = 0, starts = 0;
        cart.onStop = [&] (const juce::Uuid&) { ++stops; };
        cart.onTrigger = [&] (const Cue&) { ++starts; };
        const auto click = mouse (cart, cartPoint (cart, 1), true);
        cart.mouseDown (click);
        cart.mouseUp (click);
        expect (cues.findById (b.id) == nullptr && cues.get (1).id == c.id, "C replaces B during selection");
        expectEquals (stops, 0, "no stop callback for a removed UUID or its replacement");
        expectEquals (starts, 0, "right-click cannot trigger the replacement");
    }
    void delayedSelection (SourceType source, bool processMessages)
    {
        beginTest ("selected B status row keeps selection/playhead through "
                   + juce::String (source == SourceType::touch ? "touch" : "mouse")
                   + (processMessages ? " down/messages/up" : " down/up without queued callbacks"));
        Fixture f;
        const auto p = projectWith ({}, false);
        if (! require (f.open (p), "open table")) return;
        auto& doc = f.document();
        doc.cues.setSelection ({ 0, 1 }, 0);
        auto* editor = editNumber (f);
        if (editor == nullptr) return;
        FocusWitness focus (*editor);
        PointerPress press (f.table(), statusPoint (f.table()), juce::ModifierKeys::leftButtonModifier, source);
        expectEquals (juce::Desktop::getInstance().getNumDraggingMouseSources(), 1, "real JUCE source holds the press");
        expectEquals (f.table().getNumSelectedRows(), 2, "JUCE postpones collapsing the selection until mouseUp");
        expect (doc.cues.getSelected()->id == p.cues()[0].id, "A is still primary before release");
        if (processMessages)
        {
            pumpTimers();
            expectEquals (focus.losses, 1, "queued TextEditor focus loss ran while still pressed");
        }
        expectHeld (f, p);
        press.release();
        pumpTimers();
        expectReordered (f, p);
        expect (doc.cues.getSelected()->id == p.cues()[1].id, "B is selected after delayed mouseUp selection");
        expect (doc.cues.getPlayhead()->id == p.cues()[1].id, "the status click sets B as playhead");
        expectEquals (f.table().getSelectedRow(), 0, "B's reordered row is highlighted");
        expectEquals (f.table().getNumSelectedRows(), 1, "mouseUp leaves exactly B selected");
        expectEquals (editor->getText(), juce::String ("2"), "B remains in the inspector");
        expectEquals (doc.getHistory().getUndoDepth(), 1, "deferred number is exactly one undo step");
        expect (doc.undo(), "undo the deferred number");
        pumpTimers();
        expectEquals (doc.findCueAnywhere (p.cues()[0].id)->number, juce::String ("1"));
        expect (! doc.canUndo() && doc.canRedo(), "late focus/timer notifications preserve redo");
        expect (doc.redo(), "redo the deferred number");
        expectReordered (f, p);
    }
    void keyboardCommit (bool enter)
    {
        beginTest (enter ? "number Enter commits without waiting for a pointer timer"
                         : "number Tab commits without waiting for a pointer timer");
        Fixture f;
        const auto p = projectWith ({}, false);
        if (! require (f.open (p), "open table")) return;
        auto* editor = editNumber (f);
        if (editor == nullptr) return;
        expectEquals (juce::Desktop::getInstance().getNumDraggingMouseSources(), 0);
        f.main->getPeer()->handleKeyPress (juce::KeyPress (enter ? juce::KeyPress::returnKey : juce::KeyPress::tabKey));
        drainMessages(); // TextEditor return/focus notifications only, no timer wait
        expectReordered (f, p);
        expect (! editor->hasKeyboardFocus (false), "keyboard completion leaves the number editor");
        expectEquals (f.document().getHistory().getUndoDepth(), 1);
    }
    void enterWhilePressed()
    {
        beginTest ("Enter while a pointer is held defers number movement");
        Fixture f;
        const auto p = projectWith ({}, false);
        if (! require (f.open (p), "open table")) return;
        auto* editor = editNumber (f);
        if (editor == nullptr) return;
        PointerPress press (*editor, editor->getLocalBounds().getCentre());
        f.main->getPeer()->handleKeyPress (juce::KeyPress (juce::KeyPress::returnKey));
        pumpTimers();
        expectHeld (f, p);
        press.release();
        pumpTimers();
        expectReordered (f, p);
        expectEquals (f.document().getHistory().getUndoDepth(), 1);
    }
    void allPointersReleased (bool twoButtons)
    {
        beginTest (twoButtons ? "number waits for the last mouse button"
                              : "number waits for a second touch after the mouse releases");
        Fixture f;
        const auto p = projectWith ({}, false);
        if (! require (f.open (p), "open table")) return;
        if (editNumber (f) == nullptr) return;
        PointerPress press (f.table(), statusPoint (f.table()));
        if (twoButtons)
        {
            press.buttons (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::rightButtonModifier);
            press.buttons (juce::ModifierKeys::rightButtonModifier);
            pumpTimers();
            expectHeld (f, p);
            press.release();
        }
        else
        {
            PointerPress touch (*f.main, { 2, 2 }, juce::ModifierKeys::leftButtonModifier, SourceType::touch, 1);
            expectEquals (juce::Desktop::getInstance().getNumDraggingMouseSources(), 2, "mouse and touch are both active");
            press.release();
            pumpTimers();
            expectHeld (f, p);
            touch.release();
        }
        pumpTimers();
        expectReordered (f, p);
        expectEquals (f.document().getHistory().getUndoDepth(), 1);
    }
    void cancelledCommit (int mode)
    {
        const char* names[] { "target deletion", "target deletion then undo", "property undo", "project replacement with identical UUIDs" };
        beginTest ("deferred number is discarded after " + juce::String (names[mode]));
        Fixture f;
        const auto p = projectWith ({}, false);
        if (! require (f.open (p), "open table")) return;
        if (editNumber (f) == nullptr) return;
        PointerPress press (f.table(), statusPoint (f.table()));
        pumpTimers();
        expectHeld (f, p);
        auto& doc = f.document();
        if (mode <= 1)
        {
            doc.perform ("Delete A", [&] { doc.cues.remove (doc.cues.indexOf (p.cues()[0].id)); });
            if (mode == 1) expect (doc.undo(), "restore A while still pressed");
        }
        else if (mode == 2)
        {
            doc.perform ("Rename B", [&] { doc.cues.update (1, [] (Cue& cue) { cue.name = "changed"; }); });
            expect (doc.undo(), "undo a property while A's number is deferred");
        }
        else
            doc.adopt (p, f.scratch.folder.getChildFile ("replacement.enqueue"));
        press.release();
        pumpTimers();
        const auto* a = doc.findCueAnywhere (p.cues()[0].id);
        if (mode == 0)
            expect (a == nullptr, "deleted A is not recreated");
        else
        {
            expect (a != nullptr, "restored A exists");
            if (a != nullptr) expectEquals (a->number, juce::String ("1"), "deferred number cannot overwrite restored A");
        }
        expectEquals (doc.findCueAnywhere (p.cues()[1].id)->number, juce::String ("2"), "B never receives A's deferred input");
        expectEquals (doc.getHistory().getUndoDepth(), mode == 0 ? 1 : 0, "no deferred edit is added");
        expectEquals (doc.getHistory().getRedoDepth(), mode == 1 || mode == 2 ? 1 : 0, "undo keeps redo available");
        if (mode == 3) expect (! doc.isDirty(), "replacement stays clean");
    }
    void conflictingCommit()
    {
        beginTest ("deferred number rechecks uniqueness at release and preserves the next editor");
        QuietAlerts alerts;
        Fixture f;
        const auto p = projectWith ({}, false);
        if (! require (f.open (p), "open table")) return;
        auto* editor = editNumber (f);
        if (editor == nullptr) return;
        PointerPress press (f.table(), statusPoint (f.table()));
        pumpTimers();
        expectHeld (f, p);
        f.document().setCueNumber (p.cues()[1].id, "3");
        press.release();
        pumpTimers();
        expectEquals (f.document().findCueAnywhere (p.cues()[0].id)->number, juce::String ("1"), "conflict keeps A's original number");
        expectEquals (f.document().findCueAnywhere (p.cues()[1].id)->number, juce::String ("3"), "existing number owner keeps it");
        expectEquals (editor->getText(), juce::String ("3"), "rejecting A never resets B's displayed number");
        expectEquals (alerts.alerts, 1, "one alert at deferred validation");
        expectEquals (f.document().getHistory().getUndoDepth(), 1, "only B's explicit edit is in history");
    }
    void inactiveListCommit()
    {
        beginTest ("deferred number follows its UUID to an inactive list");
        Fixture f;
        auto p = projectWith ({}, false);
        Cue c; c.name = "C"; c.number = "10";
        auto second = p.lists.front();
        second.id = juce::Uuid();
        second.name = "Second";
        second.cues = { c };
        p.lists.push_back (second);
        if (! require (f.open (p), "open two lists")) return;
        if (editNumber (f) == nullptr) return;
        PointerPress press (f.table(), statusPoint (f.table()));
        pumpTimers();
        expectHeld (f, p);
        f.document().setActiveContainer (1);
        press.release();
        pumpTimers();
        expectEquals (f.document().getActiveContainer(), 1);
        expectEquals (f.document().findCueAnywhere (p.cues()[0].id)->number, juce::String ("3"));
        expectEquals (f.document().findCueAnywhere (p.cues()[1].id)->number, juce::String ("2"));
        expectEquals (f.document().cues.get (0).number, juce::String ("10"), "new list's editor is never the commit target");
        expectEquals (f.document().getHistory().getUndoDepth(), 1);
    }
    void cartSelectionMutation (int mode)
    {
        const char* names[] { "same slot after reorder", "different slot after reorder", "changed grid", "switched away and back", "right-click after reorder" };
        beginTest ("cart preserves pressed position and UUID: " + juce::String (names[mode]));
        CueList cues;
        Cue a, b;
        cues.add (a); cues.add (b);
        cues.setSelectedIndex (0);
        juce::AudioFormatManager formats;
        CueCartView cart (cues, formats);
        cart.setGrid (1, 2);
        cart.setSize (600, 200);
        struct MoveSelected final : CueList::Listener
        {
            explicit MoveSelected (CueList& list) : model (list) { model.addListener (this); }
            ~MoveSelected() override { model.removeListener (this); }
            void cueSelectionChanged (int index) override
            {
                if (index == 1 && ! moved) { moved = true; model.move (1, 0); }
            }
            CueList& model;
            bool moved = false;
        } moveSelected (cues);
        std::vector<juce::Uuid> starts, stops;
        cart.onTrigger = [&] (const Cue& cue) { starts.push_back (cue.id); };
        cart.onStop = [&] (const juce::Uuid& id) { stops.push_back (id); };
        cart.mouseDown (mouse (cart, cartPoint (cart, 1), mode == 4));
        expect (cues.get (0).id == b.id, "selection callback moved B");
        if (mode == 2) cart.setGrid (2, 1);
        if (mode == 3) { cues.replaceAll ({ a }); cues.replaceAll ({ b, a }); }
        const auto release = mode == 2 ? juce::Point<int> (300, 150) : cartPoint (cart, mode == 1 ? 0 : 1);
        cart.mouseUp (mouse (cart, release, mode == 4));
        cart.mouseUp (mouse (cart, release));
        expectEquals ((int) starts.size(), mode == 0 ? 1 : 0);
        if (! starts.empty()) expect (starts.front() == b.id, "only original B fires, despite A now occupying the slot");
        expectEquals ((int) stops.size(), mode == 4 ? 1 : 0);
        if (! stops.empty()) expect (stops.front() == b.id, "right-click stop still targets original B by UUID");
    }
    void tableSelection (const juce::File& tone, int column, int modifiers = 0)
    {
        beginTest ("table row click preserves B and its inspector across number reorder, column " + juce::String (column)
                   + ", modifiers " + juce::String (modifiers));
        Fixture f;
        const auto p = projectWith (tone, false);
        if (! require (f.open (p), "open temporary cue table")) return;
        auto* editor = editNumber (f);
        if (editor == nullptr) return;
        clickRow (f.table(), column, 1, modifiers);
        pumpTimers();
        expectReordered (f, p);
        expect (f.document().cues.getSelected() != nullptr && f.document().cues.getSelected()->id == p.cues()[1].id,
                "the clicked B must remain the model selection");
        expectEquals (f.table().getSelectedRow(), 0, "the table must highlight B's new row");
        expectEquals (editor->getText(), juce::String ("2"), "the inspector must show B immediately");
        drainMessages();
        expect (f.document().cues.getSelected() != nullptr && f.document().cues.getSelected()->id == p.cues()[1].id,
                "queued focus loss must keep B selected");
        expectEquals (editor->getText(), juce::String ("2"), "queued focus loss must keep B in the inspector");
        expectEquals (f.table().getNumSelectedRows(), modifiers == 0 ? 1 : 2, "modifier selection survives the reorder");
        if (column == CueTable::colContinue)
        {
            expect (f.document().cues.get (0).continueMode != p.cues()[1].continueMode, "continue click changes B");
            expect (f.document().cues.get (1).continueMode == p.cues()[0].continueMode, "continue click leaves A unchanged");
        }
        if (column == CueTable::colStatus)
        {
            // Leave the pointer over B, then select A from the keyboard path.
            // Clicking already-selected A produces no new selection callback.
            clickRow (f.table(), CueTable::colName, 0);
            f.table().selectRow (1);
            clickRow (f.table(), CueTable::colStatus, 1);
            expect (f.document().cues.getSelected() != nullptr && f.document().cues.getSelected()->id == p.cues()[0].id,
                    "keyboard selection must not leave a stale mouse target for the next click");
            expectEquals (f.table().getSelectedRow(), 1, "already-selected A remains selected");
        }
    }
};

AuditFix0923FinalTests auditFix0923FinalTests;
} // namespace
} // namespace gocue::tests
