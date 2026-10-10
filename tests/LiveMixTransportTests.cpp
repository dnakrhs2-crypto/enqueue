#include "HostTransport.h"
#include "MixDocument.h"
#include "LiveMixSettings.h"
#include "TestGainPlugin.h"
#include "ui/SettingsDialog.h"

#include <cstring>
#include <thread>

namespace gocue
{
struct TransportTestAccess
{
    static juce::CriticalSection& chainLock (PluginChain& chain) { return chain.lock; }
};
}
namespace gocue::livemix
{
struct MixTransportTestAccess
{
    static juce::CriticalSection& graphLock (MixEngine& engine) { return engine.lock; }
    static void stopped (MixEngine& engine) { engine.audioDeviceStopped(); }
};
}

namespace gocue::tests
{
using namespace gocue::livemix;

namespace
{
    class TransportGainFormat final : public juce::AudioPluginFormat
    {
    public:
        juce::String getName() const override { return "Test"; }
        void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
        bool fileMightContainThisPluginType (const juce::String& id) override { return id == "test://gain"; }
        juce::String getNameOfPluginFromIdentifier (const juce::String&) override { return "TestGain"; }
        bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
        bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
        bool canScanForPlugins() const override { return false; }
        bool isTrivialToScan() const override { return true; }
        juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
        juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
        bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
    private:
        void createPluginInstance (const juce::PluginDescription&, double, int, PluginCreationCallback callback) override
        {
            auto plugin = std::make_unique<TestGainPlugin> (1.0f);
            plugin->recordPositions = true;
            callback (std::move (plugin), {});
        }
    };

    TestGainPlugin& addProbe (PluginChain& chain, int latencySamples = 0)
    {
        auto plugin = std::make_unique<TestGainPlugin> (1.0f);
        auto& result = *plugin;
        result.recordPositions = true;
        result.latencySamples = latencySamples;
        chain.addPlugin (std::move (plugin));
        return result;
    }

    void renderTransport (MixEngine& engine, int n)
    {
        juce::AudioBuffer<float> output (2, n);
        engine.renderBlock (nullptr, 0, output.getArrayOfWritePointers(), 2, n);
    }

    template <typename Fn> void whileLocked (const juce::CriticalSection& lock, Fn fn)
    {
        juce::WaitableEvent entered, release;
        std::thread holder ([&] { const juce::ScopedLock sl (lock); entered.signal(); release.wait(); });
        entered.wait();
        fn();
        release.signal();
        holder.join();
    }
}

class LiveMixTransportTests : public juce::UnitTest
{
public:
    LiveMixTransportTests() : UnitTest ("LiveMix plugin transport", "LiveMix") {}

    void expectSamples (const TestGainPlugin& plugin, std::initializer_list<juce::int64> samples)
    {
        expectEquals (plugin.positionCount, (int) samples.size());
        int i = 0;
        for (const auto sample : samples)
        {
            const auto& seen = plugin.positions[(size_t) i++];
            expect (seen.hasPlayHead && seen.position.hasValue());
            if (seen.position)
                expectEquals (seen.position->getTimeInSamples().orFallback (-1), sample);
        }
    }

    void runTest() override
    {
        beginTest ("ON fields, PPQ and bar maths; OFF is a stopped position, including after ON");
        {
            HostTransport transport;
            transport.prepare (48000.0);
            transport.beginBlock();
            for (const juce::int64 sample : { 0LL, 95999LL, 96000LL, 123456LL })
            {
                const auto p = transport.getPositionAt (sample);
                expect (p.getIsPlaying() && ! p.getIsRecording() && ! p.getIsLooping());
                expectEquals (p.getTimeInSamples().orFallback (-1), sample);
                expectEquals (p.getTimeInSeconds().orFallback (-1), (double) sample / 48000.0);
                expectEquals (p.getBpm().orFallback (-1), 120.0);
                expectEquals (p.getPpqPosition().orFallback (-1), (double) sample / 24000.0);
                expectEquals (p.getPpqPositionOfLastBarStart().orFallback (-1), std::floor ((double) sample / 96000.0) * 4.0);
                expect (p.getTimeSignature().hasValue());
                if (p.getTimeSignature())
                {
                    expectEquals (p.getTimeSignature()->numerator, 4);
                    expectEquals (p.getTimeSignature()->denominator, 4);
                }
                expect (! p.getLoopPoints() && ! p.getHostTimeNs() && ! p.getFrameRate());
            }
            transport.setSendTransport (false);
            transport.beginBlock();
            const auto p = transport.getPositionAt (123456);
            juce::AudioPlayHead::PositionInfo stopped;
            stopped.setTimeInSamples (0);
            expect (p == stopped);
        }

        beginTest ("optional hook reaches existing, added and restored plugins before preparation; Enqueue has none");
        {
            HostTransport transport;
            transport.prepare (48000.0);
            transport.beginBlock();
            PluginChain chain;
            chain.prepare (48000.0, 32);
            auto& before = addProbe (chain);
            juce::AudioBuffer<float> buffer (2, 32);
            buffer.clear();
            chain.process (buffer, 32);
            expect (! before.positions[0].hasPlayHead);
            expect (before.getPlayHead() == nullptr && ! before.playHeadAtPrepare);
            chain.setTimingHook (&transport);
            chain.prepare (48000.0, 32);
            auto& after = addProbe (chain);
            before.positionCount = 0;
            chain.process (buffer, 32);
            expectSamples (before, { 0 });
            expectSamples (after, { 0 });
            expect (before.playHeadAtPrepare && after.playHeadAtPrepare);
            const auto states = chain.getStates();
            expect (chain.restore (states, [] (const PluginSlotState&, juce::String&)
            {
                auto plugin = std::make_unique<TestGainPlugin> (1.0f);
                plugin->recordPositions = true;
                return plugin;
            }).isEmpty());
            chain.process (buffer, 32);
            for (int i = 0; i < chain.getNumSlots(); ++i)
            {
                auto& p = static_cast<TestGainPlugin&> (*chain.getSlot (i).plugin);
                expect (p.playHeadAtPrepare);
                expectSamples (p, { 0 });
            }
        }

        beginTest ("one graph clock across variable callbacks and both chunkings, all chain kinds and session rebuild");
        {
            MixEngine engine;
            engine.prepare (48000.0, 64);
            expect (engine.getSendTransport());
            auto& initialMaster = addProbe (engine.getMasterChain());
            expect (initialMaster.playHeadAtPrepare);
            MixDocument document (engine);
            document.applyToEngine();
            auto* channel = engine.getChannelChain (document.getSession().channels[0].id);
            auto* fx = engine.getFxChain (document.getSession().fx[0].id);
            auto& mic = addProbe (*channel);
            auto& effect = addProbe (*fx);
            auto& master = addProbe (engine.getMasterChain());
            auto& mic2 = addProbe (*engine.getChannelChain (document.addChannel()));
            channel->prepare (48000.0, 24); // 64 engine chunks, then 24-sample chain chunks
            renderTransport (engine, 100);
            renderTransport (engine, 37);
            expectSamples (mic, { 0, 24, 48, 64, 88, 100, 124 });
            expectSamples (effect, { 0, 64, 100 });
            expectSamples (master, { 0, 64, 100 });
            expectSamples (mic2, { 0, 64, 100 });
            expectEquals (mic.positions[2].numSamples, 16);
            expectEquals (mic.positions[4].numSamples, 12);
            engine.setSendTransport (false);
            renderTransport (engine, 17);
            expect (master.positions[3].position.hasValue());
            if (master.positions[3].position)
            {
                juce::AudioPlayHead::PositionInfo stopped;
                stopped.setTimeInSamples (0);
                expect (*master.positions[3].position == stopped);
            }
            engine.setSendTransport (true);
            MixTransportTestAccess::stopped (engine);
            engine.prepare (48000.0, 64);
            renderTransport (engine, 11);
            expectEquals (master.positions[4].position->getTimeInSamples().orFallback (-1), (juce::int64) 154);
            engine.prepare (44100.0, 64);
            master.positionCount = 0;
            renderTransport (engine, 13);
            expectSamples (master, { 0 });
            const auto file = juce::File::createTempFile (".livemix");
            expect (document.save (file).wasOk());
            engine.getPluginHost().getFormatManager().addFormat (std::make_unique<TransportGainFormat>());
            expect (document.load (file).wasOk());
            engine.forEachChain ([&] (PluginChain& c)
            {
                auto* restored = dynamic_cast<TestGainPlugin*> (c.getSlot (0).plugin.get());
                expect (restored != nullptr && restored->playHeadAtPrepare);
                expect (addProbe (c).playHeadAtPrepare);
            });
            renderTransport (engine, 19);
            engine.forEachChain ([&] (PluginChain& c)
            {
                for (int i = 0; i < c.getNumSlots(); ++i)
                    if (auto* restored = dynamic_cast<TestGainPlugin*> (c.getSlot (i).plugin.get())) expectSamples (*restored, { 13 });
            });
            expect (file.deleteFile());
        }

        beginTest ("catch-up timestamps use backlog before decrement, including a backlog larger than the budget");
        {
            MixEngine engine;
            engine.prepare (48000.0, 32);
            auto& p = addProbe (engine.getMasterChain());
            p.suspendProcessing (true);
            renderTransport (engine, 96);
            p.suspendProcessing (false);
            renderTransport (engine, 32);
            renderTransport (engine, 32);
            expectSamples (p, { 0, 32, 64, 96, 128 });
            whileLocked (p.getCallbackLock(), [&] { renderTransport (engine, 17); });
            renderTransport (engine, 9);
            expectSamples (p, { 0, 32, 64, 96, 128, 160, 177 });
            p.suspendProcessing (true);
            renderTransport (engine, 32);
            engine.prepare (44100.0, 32);
            p.suspendProcessing (false);
            p.positionCount = 0;
            renderTransport (engine, 32);
            expectSamples (p, { 0 }); // old-rate backlog was discarded during preparation
        }

        beginTest ("a graph callback lost during a backlog freezes the clock; catch-up keeps preceding positions");
        {
            MixEngine engine;
            engine.prepare (48000.0, 32);
            auto& p = addProbe (engine.getMasterChain());
            p.suspendProcessing (true);
            renderTransport (engine, 32);
            whileLocked (MixTransportTestAccess::graphLock (engine), [&] { renderTransport (engine, 32); });
            p.suspendProcessing (false);
            renderTransport (engine, 65);
            expectSamples (p, { 0, 32, 64, 96 });
        }

        beginTest ("graph try-lock failure does not advance; catch-up precedes the current time after chain and OFF gaps");
        {
            MixEngine engine;
            engine.prepare (48000.0, 32);
            MixDocument document (engine);
            document.applyToEngine();
            const auto id = document.getSession().channels[0].id;
            auto& chain = *engine.getChannelChain (id);
            auto& p = addProbe (chain);
            renderTransport (engine, 32);
            whileLocked (MixTransportTestAccess::graphLock (engine), [&] { renderTransport (engine, 29); });
            renderTransport (engine, 32);
            p.suspendProcessing (true);
            renderTransport (engine, 32);
            p.suspendProcessing (false);
            whileLocked (gocue::TransportTestAccess::chainLock (chain), [&] { renderTransport (engine, 32); });
            renderTransport (engine, 32);
            expectSamples (p, { 0, 32, 96, 128 }); // older input is labelled just before now after the chain gap
            document.setChannelOn (id, false);
            p.suspendProcessing (true); // keep a backlog through the OFF ramp and skip
            renderTransport (engine, 2048);
            p.suspendProcessing (false);
            const int offCount = p.positionCount;
            renderTransport (engine, 32);
            expectEquals (p.positionCount, offCount);
            document.setChannelOn (id, true);
            renderTransport (engine, 32);
            expectSamples (p, { 0, 32, 96, 128, 1984, 2016 }); // 256 samples await catch-up at block start 2240
            renderTransport (engine, 192);
            expectSamples (p, { 0, 32, 96, 128, 1984, 2016, 2048, 2080, 2112, 2144, 2176, 2208,
                               2240, 2272, 2304, 2336, 2368, 2400, 2432 });
        }

        const juce::StringArray gaps { "callback catch-up", "chain try-lock failure", "graph try-lock failure", "OFF skip and resume" };
        for (int gap = 0; gap < gaps.size(); ++gap)
        {
            beginTest ("timing hook preserves bit-identical latency audio through " + gaps[gap]);
            for (const bool playing : { false, true })
            {
                constexpr int blockSize = 64;
                MixEngine timed, untimed;
                MixSession session;
                session.addChannel();
                session.channels[0].stereo = true;
                const auto id = session.channels[0].id;
                for (auto* engine : { &timed, &untimed })
                {
                    engine->prepare (48000.0, blockSize);
                    engine->setSkipChainWhenOff (true);
                    engine->setSendTransport (playing);
                    engine->applySession (session);
                }
                untimed.forEachChain ([] (PluginChain& chain) { chain.setTimingHook (nullptr); });
                auto& timedChain = *timed.getChannelChain (id);
                auto& untimedChain = *untimed.getChannelChain (id);
                auto& timedPlugin = addProbe (timedChain, 2 * blockSize);
                auto& untimedPlugin = addProbe (untimedChain, 2 * blockSize);
                expect (timedPlugin.getPlayHead() != nullptr && untimedPlugin.getPlayHead() == nullptr);
                juce::AudioBuffer<float> input (2, blockSize), timedOutput (2, blockSize), untimedOutput (2, blockSize);
                input.clear();
                int block = 0;
                const auto render = [&]
                {
                    timed.renderBlock (input.getArrayOfReadPointers(), 2, timedOutput.getArrayOfWritePointers(), 2, blockSize);
                    untimed.renderBlock (input.getArrayOfReadPointers(), 2, untimedOutput.getArrayOfWritePointers(), 2, blockSize);
                    ++block;
                    for (int ch = 0; ch < 2; ++ch)
                        expect (std::memcmp (timedOutput.getReadPointer (ch), untimedOutput.getReadPointer (ch),
                                             blockSize * sizeof (float)) == 0,
                                "block " + juce::String (block) + ", channel " + juce::String (ch)
                                    + (playing ? ", transport ON" : ", transport OFF"));
                };
                for (int i = 0; i < 4; ++i) render(); // settle the mic ramp before the impulse
                input.setSample (0, 17, 1.0f);
                input.setSample (1, 17, -0.5f);
                render();
                input.clear();
                render();
                whileLocked (timedPlugin.getCallbackLock(), [&]
                {
                    whileLocked (untimedPlugin.getCallbackLock(), [&]
                    {
                        render(); // the dry delay emits the impulse; catch-up must consume the plugin's stale copy
                        expectEquals (timedOutput.getSample (0, 17), 1.0f);
                        expectEquals (timedOutput.getSample (1, 17), -0.5f);
                        if (gap == 3)
                        {
                            timed.setChannelOn (id, false);
                            untimed.setChannelOn (id, false);
                            for (int i = 0; i < 4; ++i) render(); // busy throughout the fade, leaving a backlog while OFF
                        }
                    });
                });
                if (gap == 1)
                    whileLocked (TransportTestAccess::chainLock (timedChain), [&]
                    {
                        whileLocked (TransportTestAccess::chainLock (untimedChain), render);
                    });
                else if (gap == 2)
                    whileLocked (MixTransportTestAccess::graphLock (timed), [&]
                    {
                        whileLocked (MixTransportTestAccess::graphLock (untimed), render);
                    });
                else if (gap == 3)
                {
                    const int timedCount = timedPlugin.processCount, untimedCount = untimedPlugin.processCount;
                    for (int i = 0; i < 2; ++i) render();
                    expectEquals (timedPlugin.processCount, timedCount);
                    expectEquals (untimedPlugin.processCount, untimedCount);
                    timed.setChannelOn (id, true);
                    untimed.setChannelOn (id, true);
                }
                for (int i = 0; i < 12; ++i) render(); // drain the backlog and both latency buffers, then settle wetMix
                expectEquals (timedPlugin.processCount, untimedPlugin.processCount);
            }
        }

        beginTest ("settings default ON, persistence and the real dialog toggle update settings and engine");
        {
            const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lm-transport-" + juce::Uuid().toString());
            {
                MixEngine engine;
                LiveMixSettings settings (directory);
                expect (settings.getSendTransport());
                SettingsDialog::show (engine, settings, nullptr, {}, {}, {}, {}, {});
                juce::ToggleButton* found = nullptr;
                juce::ToggleButton* skip = nullptr;
                juce::Component* content = nullptr;
                auto& desktop = juce::Desktop::getInstance();
                for (int i = 0; i < desktop.getNumComponents(); ++i)
                    if (auto* window = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)))
                        if (auto* viewport = dynamic_cast<juce::Viewport*> (window->getContentComponent()))
                            for (auto* child : viewport->getViewedComponent()->getChildren())
                                if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
                                {
                                    if (toggle->getButtonText() == juce::String::fromUTF8 ("재생 신호 보내기 (학습 플러그인 사용)")) found = toggle;
                                    if (toggle->getButtonText() == juce::String::fromUTF8 ("꺼진 마이크는 플러그인도 멈춤")) skip = toggle;
                                    content = viewport->getViewedComponent();
                                }
                expect (found != nullptr && skip != nullptr);

                // a picture of the whole settings content for a manual look at the layout (never on the operator's screen)
                if (const auto snapshot = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_SETTINGS_SNAPSHOT", {}); snapshot.isNotEmpty() && content != nullptr)
                {
                    const juce::File out (snapshot);
                    out.deleteFile();
                    juce::FileOutputStream stream (out);
                    juce::PNGImageFormat().writeImageToStream (content->createComponentSnapshot (content->getLocalBounds()), stream);
                }
                if (found != nullptr && skip != nullptr)
                {
                    expect (found->getToggleState());
                    expectEquals (found->getY(), skip->getBottom());
                    expectEquals (found->getHeight(), 28);
                    found->setToggleState (false, juce::sendNotificationSync);
                    expect (! settings.getSendTransport() && ! engine.getSendTransport());
                    found->setToggleState (true, juce::sendNotificationSync);
                    expect (settings.getSendTransport() && engine.getSendTransport());
                    found->setToggleState (false, juce::sendNotificationSync);
                }
                SettingsDialog::closeIfOpen();
                settings.saveIfNeeded();
            }
            { LiveMixSettings settings (directory); expect (! settings.getSendTransport()); }
            expect (directory.deleteRecursively());
        }
    }
};
static LiveMixTransportTests liveMixTransportTests;
}
