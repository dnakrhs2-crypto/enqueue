#include "ui/MainComponent.h"
#include "ui/ShortcutSettingsTab.h"
#include "ui/CueMidiPanel.h"
#include "ui/UiUtils.h"
#include "ui/GoCueLookAndFeel.h"
#include "app/Commands.h"

namespace gocue::tests
{
namespace
{
template <typename T> T* findChild (juce::Component& root)
{
    if (auto* found = dynamic_cast<T*> (&root)) return found;
    for (auto* child : root.getChildren()) if (auto* found = findChild<T> (*child)) return found;
    return nullptr;
}
bool accessibleContains (juce::AccessibilityHandler* root, const juce::Component& wanted)
{
    if (root == nullptr) return false;
    if (&root->getComponent() == &wanted) return true;
    for (auto* child : root->getChildren()) if (accessibleContains (child, wanted)) return true;
    return false;
}
struct Fixture
{
    Fixture() : storage (file, options()), settings (storage), main (std::make_unique<MainComponent> (engine, settings, commands))
    { main->setLookAndFeel (&theme); }
    ~Fixture() { main.reset(); storage.saveIfNeeded(); file.deleteFile(); }
    static juce::PropertiesFile::Options options()
    { juce::PropertiesFile::Options result; result.millisecondsBeforeSaving = -1; return result; }
    juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("EnqueueMidiMain", ".settings");
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine;
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
};
}
class MidiMainComponentTests : public juce::UnitTest
{
public:
    MidiMainComponentTests() : UnitTest ("MIDI main component layout and accessibility", "Enqueue") {}
    void runTest() override
    {
        const juce::ScopedValueSetter<std::function<bool()>> foregroundCheck (KeyCapture::foregroundProcessCheck, [] { return true; });
        Fixture f;
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::addMemoCue));
        auto* inspector = findChild<CueInspector> (*f.main);
        if (inspector != nullptr)
            if (auto* tabs = findChild<juce::TabbedComponent> (*inspector)) tabs->setCurrentTabIndex (0);
        auto* midi = findChild<CueMidiPanel> (*f.main);
        auto* hotkey = inspector != nullptr ? findChild<KeyCaptureButton> (*inspector) : nullptr;
        beginTest ("inspector MIDI stays beside hotkey in short wide panes and wraps with a usable memo");
        expect (inspector != nullptr && midi != nullptr && hotkey != nullptr);
        if (inspector == nullptr || midi == nullptr || hotkey == nullptr) return;
        expect (f.main->getShortcutService().setKeys ("transport.go", { juce::KeyPress ('A') }).wasOk());
        for (bool assigned : { false, true })
        {
            if (assigned)
            {
                MidiTrigger trigger; trigger.number = 60; trigger.channel = 1;
                expect (midi->apply (-1, trigger).wasOk());
            }
            for (int width : { 1541, 1100, 1099, 1000 })
            {
                beginTest ("inspector memo and controls remain usable with wrapped MIDI and hotkey conflict: "
                    + juce::String (width) + (assigned ? " assigned" : " empty"));
                f.main->setSize (width, 980);
                inspector->setSize (inspector->getWidth(), 230);
                auto* page = midi->getParentComponent();
                expectEquals (page->getHeight(), width >= 1100 ? Palette::inspectorBasicHeight : 236);
                if (width >= 1100) expectEquals (midi->getY(), hotkey->getY());
                else expect (midi->getY() > hotkey->getY());
                expect (inspector->getLocalBounds().contains (inspector->getLocalArea (midi, midi->getLocalBounds())));
                if (width == 1541) expect (midi->getWidth() >= 500);
                for (auto* child : midi->getChildren())
                    if (child->isVisible()) expect (midi->getLocalBounds().contains (child->getBounds()));
                for (bool conflict : { false, true, false })
                {
                    // A stored cue hotkey can conflict with a command on this PC.
                    hotkey->onHotkeyChanged (conflict ? "A" : juce::String());
                    inspector->setEditable (true); // flush the inspector refresh without a window resize
                    juce::TextEditor* memo = nullptr;
                    juce::Label* notice = nullptr;
                    for (auto* child : page->getChildren())
                    {
                        if (auto* editor = dynamic_cast<juce::TextEditor*> (child); editor != nullptr && editor->isMultiLine()) memo = editor;
                        if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr
                            && (label->getText().isEmpty() || label->getText().contains (ko ("단축키와 충돌")))) notice = label;
                    }
                    expect (notice != nullptr);
                    if (notice != nullptr)
                    {
                        expect (notice->getText().isNotEmpty() == conflict, "conflict notice matches the stored hotkey: " + hotkey->getButtonText());
                        expectEquals (notice->getHeight(), conflict ? 22 : 0);
                    }
                    expectEquals (page->getHeight(), width >= 1100 ? Palette::inspectorBasicHeight : conflict ? 258 : 236);
                    expect (memo != nullptr);
                    for (auto* child : page->getChildren())
                    {
                        if (child == notice && ! conflict) continue;
                        const auto bounds = child->getBounds();
                        expect (page->getLocalBounds().contains (bounds), "control outside page: " + bounds.toString());
                        if (child != midi && child != notice)
                            expect (child->getHeight() >= 30, "control below 30px: " + bounds.toString());
                        for (auto* other : page->getChildren())
                        {
                            if (other == child) break;
                            if (! other->getBounds().isEmpty())
                                expect (! bounds.intersects (other->getBounds()), "overlapping controls: " + bounds.toString() + " / " + other->getBounds().toString());
                        }
                    }
                    if (memo != nullptr)
                    {
                        expect (memo->getHeight() >= 30, "memo height=" + juce::String (memo->getHeight()));
                        auto* viewport = page->findParentComponentOfClass<juce::Viewport>();
                        expect (viewport != nullptr);
                        if (viewport != nullptr)
                        {
                            viewport->setViewPosition (0, memo->getBottom());
                            expect (viewport->getViewArea().contains (memo->getBounds()), "memo remains reachable by scrolling");
                            expect (memo->getHeight() >= 30);
                            viewport->setViewPosition (0, 0);
                        }
                    }
                }
                const auto output = juce::SystemStats::getEnvironmentVariable ("ENQUEUE_MIDI_REVIEW_IMAGES", {});
                if (output.isNotEmpty())
                {
                    auto file = juce::File (output).getChildFile ("inspector-" + juce::String (width) + (assigned ? "-assigned.png" : "-empty.png"));
                    auto stream = file.createOutputStream();
                    if (stream != nullptr) { stream->setPosition (0); stream->truncate(); juce::PNGImageFormat().writeImageToStream (inspector->createComponentSnapshot (inspector->getLocalBounds()), *stream); }
                }
                logMessage ("Inspector width " + juce::String (width) + ": page=" + page->getBounds().toString()
                    + " hotkey=" + hotkey->getBounds().toString() + " MIDI=" + midi->getBounds().toString()
                    + " in inspector=" + inspector->getLocalArea (midi, midi->getLocalBounds()).toString());
            }
        }
        beginTest ("inspector hotkey and MIDI add buttons are reachable through the native accessibility tree");
        f.main->setSize (1541, 980);
        f.main->addToDesktop (0);
        f.main->setVisible (true);
        expect (accessibleContains (f.main->getAccessibilityHandler(), *hotkey));
        auto* add = findChild<KeyCaptureButton> (*midi);
        expect (add != nullptr && accessibleContains (f.main->getAccessibilityHandler(), *add));
        f.main->removeFromDesktop();

        beginTest ("settings command-row learning buttons are reachable through the native accessibility tree");
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::workspaceSettings));
        auto* dialog = juce::Component::getCurrentlyModalComponent();
        auto* tabs = dialog != nullptr ? findChild<juce::TabbedComponent> (*dialog) : nullptr;
        expect (tabs != nullptr);
        if (tabs == nullptr) return;
        tabs->setCurrentTabIndex (3);
        auto* tab = findChild<ShortcutSettingsTab> (*dialog);
        juce::TextButton* learn = nullptr;
        const auto visit = [&] (auto&& self, juce::Component& component) -> void
        {
            if (learn != nullptr) return;
            if (auto* button = dynamic_cast<juce::TextButton*> (&component); button != nullptr && button->getButtonText() == ko ("학습")) learn = button;
            if (learn == nullptr) for (auto* child : component.getChildren()) self (self, *child);
        };
        if (tab != nullptr) visit (visit, *tab);
        expect (learn != nullptr && accessibleContains (dialog->getAccessibilityHandler(), *learn));
    }
};
class MidiShutdownTests : public juce::UnitTest
{
public:
    MidiShutdownTests() : UnitTest ("MIDI main component capture shutdown lifetime", "Enqueue") {}
    void runTest() override
    {
        const juce::ScopedValueSetter<std::function<bool()>> foregroundCheck (KeyCapture::foregroundProcessCheck, [] { return true; });
        beginTest ("closing MainComponent during settings learning cancels capture while MIDI services remain queryable");
        Fixture f;
        f.main->perform (juce::ApplicationCommandTarget::InvocationInfo (CommandIDs::workspaceSettings));
        auto* dialog = juce::Component::getCurrentlyModalComponent();
        expect (dialog != nullptr);
        if (dialog == nullptr) return;
        auto* tabs = findChild<juce::TabbedComponent> (*dialog);
        expect (tabs != nullptr);
        if (tabs == nullptr) return;
        tabs->setCurrentTabIndex (3);
        auto* tab = findChild<ShortcutSettingsTab> (*dialog);
        auto* capture = findChild<KeyCapture> (*dialog);
        expect (tab != nullptr && capture != nullptr);
        if (tab == nullptr || capture == nullptr) return;
        juce::Component::SafePointer<juce::Component> dialogLifetime (dialog);
        bool finished = false;
        auto finishedCallback = capture->onFinished;
        capture->onFinished = [&, finishedCallback, owner = f.main.get()]
        {
            finished = true;
            expectEquals (owner->getMidiInputService().counters().received, uint64_t (0));
            // Exercise the actual settings refresh: it reads both MIDI objects.
            finishedCallback();
        };
        capture->start ("shutdown");
        expect (f.main->getShortcutService().isCapturing());
        f.main.reset();
        expect (finished);
        expect (dialogLifetime == nullptr);
    }
};
static MidiMainComponentTests midiMainComponentTests;
static MidiShutdownTests midiShutdownTests;
}
