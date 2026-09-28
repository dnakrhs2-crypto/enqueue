#pragma once

#include "MainComponentTestAccess.h"
#include "ui/GoCueLookAndFeel.h"
#include "app/Commands.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests::volume_ui
{
inline void dispatch()
{
   #if JUCE_WINDOWS
    MSG message {};
    for (int n = 0; n < 2000 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
   #endif
}

template <typename T, typename Predicate>
T* child (juce::Component& root, Predicate predicate)
{
    if (auto* found = dynamic_cast<T*> (&root); found != nullptr && predicate (*found)) return found;
    for (auto* c : root.getChildren()) if (auto* found = child<T> (*c, predicate)) return found;
    return nullptr;
}
template <typename T> T* child (juce::Component& root) { return child<T> (root, [] (const auto&) { return true; }); }

/** Logical JUCE focus and window geometry only: never creates or shows an OS window. */
class HiddenPeer final : public juce::ComponentPeer
{
public:
    HiddenPeer (juce::Component& c, int flags) : ComponentPeer (c, flags) {}
    ~HiddenPeer() override { if (focused == this) focused = nullptr; }
    void* getNativeHandle() const override { return nullptr; }
    void setVisible (bool) override {}
    void setTitle (const juce::String&) override {}
    void setBounds (const juce::Rectangle<int>& value, bool) override { bounds = value; }
    juce::Rectangle<int> getBounds() const override { return bounds; }
    juce::Point<float> localToGlobal (juce::Point<float> p) override { return p + bounds.getPosition().toFloat(); }
    juce::Point<float> globalToLocal (juce::Point<float> p) override { return p - bounds.getPosition().toFloat(); }
    void setMinimised (bool value) override { minimised = value; }
    bool isMinimised() const override { return minimised; }
    bool isShowing() const override { return false; }
    void setFullScreen (bool value) override { full = value; }
    bool isFullScreen() const override { return full; }
    void setIcon (const juce::Image&) override {}
    bool contains (juce::Point<int> p, bool) const override { return bounds.withZeroOrigin().contains (p); }
    OptionalBorderSize getFrameSizeIfPresent() const override { return OptionalBorderSize (juce::BorderSize<int>()); }
    juce::BorderSize<int> getFrameSize() const override { return {}; }
    bool setAlwaysOnTop (bool) override { return false; }
    void toFront (bool takeFocus) override { if (takeFocus) grabFocus(); }
    void toBehind (juce::ComponentPeer*) override {}
    bool isFocused() const override { return focused == this; }
    void grabFocus() override { focused = this; }
    void repaint (const juce::Rectangle<int>&) override {}
    void performAnyPendingRepaintsNow() override {}
    void setAlpha (float) override {}
    juce::StringArray getAvailableRenderingEngines() override { return {}; }
    void textInputRequired (juce::Point<int>, juce::TextInputTarget&) override {}
private:
    juce::Rectangle<int> bounds;
    bool minimised = false, full = false;
    inline static HiddenPeer* focused = nullptr;
};

class HiddenMain final : public MainComponent
{
public:
    using MainComponent::MainComponent;
private:
    juce::ComponentPeer* createNewPeer (int peerFlags, void*) override { return new HiddenPeer (*this, peerFlags); }
};

class HiddenWindow final : public ActiveCuesWindow
{
public:
    using ActiveCuesWindow::ActiveCuesWindow;
private:
    juce::ComponentPeer* createNewPeer (int peerFlags, void*) override { return new HiddenPeer (*this, peerFlags); }
};

struct Scratch
{
    Scratch() { folder.createDirectory(); }
    ~Scratch() { if (folder.isAChildOf (base) && folder.getFileName().startsWith ("EnqueueVolumeUi-")) folder.deleteRecursively(); }
    const juce::File base = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File folder = base.getChildFile ("EnqueueVolumeUi-" + juce::Uuid().toString());
};

struct Fixture
{
    static juce::PropertiesFile::Options options()
    {
        juce::PropertiesFile::Options o;
        o.millisecondsBeforeSaving = -1;
        return o;
    }
    Fixture() : storage (scratch.folder.getChildFile ("test.settings"), options()), settings (storage)
    {
        engine.prepare (44100, 512);
        main = std::make_unique<HiddenMain> (engine, settings, commands);
        main->setLookAndFeel (&theme);
        main->setSize (1541, 980);
        document().settings.autoBackup = false;
        document().settings.backupBeforeSave = false;
        document().settings.copyFilesIntoProject = false;
        document().settings.autoLoadNewCues = false;
        ReopenLastProjectTestAccess::controller (*main).clock = [this] { return now; };
    }
    ~Fixture()
    {
        main.reset();
        dispatch();
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        dispatch();
        engine.shutdown();
        storage.saveIfNeeded();
    }
    ProjectDocument& document() { return ReopenLastProjectTestAccess::document (*main); }
    CueInspector& inspector() { return *child<CueInspector> (*main); }
    CueTable& table() { return *child<CueTable> (*main); }
    ActiveCuesPanel& active() { return ReopenLastProjectTestAccess::activePanel (*main); }
    bool command (juce::CommandID id) { return main->perform (juce::ApplicationCommandTarget::InvocationInfo (id)); }
    ActiveCuesWindow& makeWindow()
    {
        auto window = std::make_unique<HiddenWindow> (engine, document().cues, settings);
        window->setLookAndFeel (&theme);
        ReopenLastProjectTestAccess::installBigView (*main, std::move (window));
        command (CommandIDs::showActiveCuesWindow);
        return *ReopenLastProjectTestAccess::bigView (*main);
    }
    void hiddenMain() { main->addToDesktop (0); main->setVisible (true); }
    bool key (const juce::KeyPress& key, juce::Component& origin, double ms = -1.0)
    {
        auto& router = ReopenLastProjectTestAccess::keyboard (*main);
        auto context = router.contextFor (&origin, key);
        context.applicationActive = true; // never reads the OS foreground
        if (const auto converted = PanicKeyHook::convert (key); converted.binding) context.nativeKey = converted.binding;
        return router.route (key, &origin, context, ms >= 0 ? ms : now * 1000);
    }
    void release (const juce::KeyPress& key)
    {
        auto& router = ReopenLastProjectTestAccess::keyboard (*main);
        if (const auto converted = PanicKeyHook::convert (key); converted.binding)
            router.prepareNativeEvent (converted.binding->virtualKey, converted.binding->modifiers, false, false, now * 1000);
        router.keyStateChanged (false, nullptr);
    }
    Cue addSound (const juce::String& name = "Music")
    {
        const auto file = scratch.folder.getChildFile ("tone.wav");
        if (! file.existsAsFile())
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (44100).withNumChannels (2).withBitsPerSample (16));
            if (writer != nullptr)
            {
                juce::AudioBuffer<float> data (2, 44100);
                for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill (data.getWritePointer (ch), 0.1f, data.getNumSamples());
                writer->writeFromAudioSampleBuffer (data, 0, data.getNumSamples());
            }
        }
        Cue cue;
        cue.name = name; cue.file = file; cue.durationSeconds = 1; cue.numChannels = 2;
        cue.audio.infiniteLoop = true; cue.autoLoad = false;
        cue.levels.resize (2, 2); cue.levels.setDefaults();
        document().cues.add (cue);
        return cue;
    }
    void settle()
    {
        juce::AudioBuffer<float> block (2, 512);
        for (int i = 0; i < 8; ++i) engine.renderBlock (block, 512);
        engine.reapIfNeeded();
    }
    Scratch scratch;
    juce::PropertiesFile storage;
    AppSettings settings;
    AudioEngine engine { 0 };
    juce::ApplicationCommandManager commands;
    GoCueLookAndFeel theme;
    std::unique_ptr<MainComponent> main;
    double now = 100.0;
};
}
