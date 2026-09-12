#include "TestSupport.h"
#include "playback/TimelineAudioRenderer.h"
#include "playback/TimelineTransport.h"
#include "audio/RecorderAudioEngine.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <malloc.h>
#include <new>
#include <thread>

// Release-build allocation counter. It covers scalar/array/aligned/nothrow C++
// allocation and deallocation on the guarded callback thread, not worker threads.
namespace recorder_allocation_hook
{
thread_local bool enabled = false;
thread_local std::size_t allocations = 0, deletions = 0;
void allocated() noexcept { if (enabled) ++allocations; }
void deleted(void* p) noexcept { if (p && enabled) ++deletions; }
struct Guard
{
    Guard() { allocations = deletions = 0; enabled = true; }
    ~Guard() { enabled = false; }
};
}
void* operator new(std::size_t n) { recorder_allocation_hook::allocated(); if (auto* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { recorder_allocation_hook::deleted(p); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return ::operator new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept { return ::operator new(n, tag); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, std::align_val_t alignment)
{ recorder_allocation_hook::allocated(); if (auto* p = _aligned_malloc(n ? n : 1, static_cast<std::size_t>(alignment))) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { recorder_allocation_hook::deleted(p); _aligned_free(p); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return ::operator new(n, a); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t& tag) noexcept { return ::operator new(n, a, tag); }
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p, a); }

using namespace gocue::recorder;
using namespace recorder_test;
namespace
{
template<class F> void eventually(F f)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) { require(std::chrono::steady_clock::now() < deadline, "Worker timed out"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}
struct ConstantSource final : PlaybackAudioSource
{
    float left = .25f, right = -.5f;
    mutable std::atomic<unsigned> calls{0};
    std::atomic<bool> block{false}, release{false}, failRead{false};
    ConstantSource()
    { sampleRate = 48000; channels = 2; length = 48000; epoch = std::make_shared<MediaEpoch>(); }
    void read(Sample, unsigned frames, float* l, float* r) const override
    {
        ++calls;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (block.load() && !release.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (failRead.load()) throw std::runtime_error("injected source read failure");
        std::fill_n(l, frames, left); std::fill_n(r, frames, right);
    }
};
struct Prepared
{
    RecorderProject project;
    std::shared_ptr<ConstantSource> source = std::make_shared<ConstantSource>();
    std::vector<AudioSourceBinding> bindings;
    Prepared()
    {
        MediaAsset asset; asset.kind = AssetKind::importAudio; asset.relativePath = "media/imports/test.wav"; asset.contentIdentity = "synthetic";
        asset.mediaGeneration = 1; asset.logicalLength = source->length; asset.availableRanges = {{0, asset.logicalLength}};
        asset.originalFormat.codec = "pcm_f32le"; asset.originalFormat.sampleRate = 48000; asset.originalFormat.channels = 2; asset.originalFormat.bitsPerSample = 32;
        auto registry = std::make_shared<MediaRegistry>(); registry->assets.push_back(asset); project.media = registry;
        Track track; track.kind = TrackKind::importAudio; Clip clip; clip.trackId = track.trackId; clip.assetId = asset.assetId; clip.lengthSamples = source->length;
        track.clips.edit().push_back(clip); project.tracks.push_back(track); project.editRevision = 1;
        bindings.push_back({asset.assetId, source});
    }
    std::shared_ptr<const CompiledRenderPlan> plan() const { return RenderPlanCompiler::compile(project); }
};
BlockStamp stamp(Sample output, unsigned frames = 256, unsigned rate = 48000)
{
    BlockStamp s{}; s.flags = samplePositionValid | latenciesValid; s.numSamples = frames; s.sampleRate = rate;
    s.samplePosition = output; s.sequence = static_cast<std::uint64_t>(output / frames); s.callbackQpc = 1000000 + output * 1000000 / rate;
    return s;
}
void noAllocations() { require(recorder_allocation_hook::allocations == 0 && recorder_allocation_hook::deletions == 0, "Callback allocated or destroyed heap storage"); }
}
int runPlaybackQueueTests()
{
    Suite suite;
    suite.test("worker prefetch >=250ms, short take only valid tail, PCM matches offline in every block", []
    {
        Prepared f; const auto plan = f.plan(); TimelineAudioRenderer renderer(48000, 128); renderer.setPlan(plan, f.bindings); renderer.prepare(1000, 7);
        eventually([&] { return renderer.ready(); }); require(renderer.queue().prefetchedFrames() >= 12000, "Insufficient prefetch"); renderer.stopWorker();
        float l[128], r[128], expectedL[128], expectedR[128];
        for (Sample at = 1000; at < 1000 + 8192; at += 128)
        {
            renderer.renderAudio(at, 128, expectedL, expectedR);
            { recorder_allocation_hook::Guard guard; require(renderer.queue().consume(at, 7, l, r, 128), "Prepared queue consume"); }
            noAllocations(); require(std::equal(l, l + 128, expectedL) && std::equal(r, r + 128, expectedR), "Realtime/offline PCM differs");
        }
        renderer.prepare(47950, 8); eventually([&] { return renderer.ready(); }); renderer.stopWorker();
        require(renderer.queue().prefetchedFrames() == 50, "Short tail over-prefilled");
        require(renderer.queue().consume(47950, 8, l, r, 50), "Short final PCM block");
    });
    suite.test("revision cutover waits for prefill, fades both sides and cannot consume old PCM at boundary", []
    {
        PlaybackBlockQueue q(100, 8); float old[100], next[100], l[100], r[100]; std::fill_n(old, 100, 1.0f); std::fill_n(next, 100, .5f);
        const auto initial = q.begin(0, 7, 1, 1000, 250);
        for (Sample at = 0; at < 600; at += 100) require(q.pushPrepared(initial, at, old, old, 100), "Old prefetch");
        const auto replacement = q.begin(300, 7, 2, 1000, 250, 3, true);
        require(q.pushPrepared(replacement, 300, next, next, 100) && q.pushPrepared(replacement, 400, next, next, 100), "Partial new prefetch");
        require(!q.ready(), "Published before 250ms prefetch");
        for (Sample at = 0; at < 300; at += 100) require(q.consume(at, 7, l, r, 100), "Old revision before boundary");
        require(l[96] == 1 && l[97] == 1 && l[98] == .5f && l[99] == 0, "Old-side revision microfade");
        require(!q.consume(300, 7, l, r, 100) && q.state() == PlaybackQueueState::buffering && q.activeRevision() == 1, "Unready revision played stale PCM");
        require(std::all_of(l, l + 100, [](float x) { return x == 0; }), "Underrun silence");
        require(q.pushPrepared(replacement, 500, next, next, 100), "Complete new prefetch");
        { recorder_allocation_hook::Guard guard; require(q.consume(300, 7, l, r, 100), "Revision switch"); }
        noAllocations(); require(q.activeRevision() == 2 && l[0] == 0 && l[1] == .25f && l[2] == .5f && l[99] == .5f, "New-side microfade/revision identity");
    });
    suite.test("superseded seek/revision tickets cannot publish or reclaim callback-owned PCM", []
    {
        PlaybackBlockQueue q(64, 8); float a[64], b[64], l[64], r[64]; std::fill_n(a, 64, 1.0f); std::fill_n(b, 64, 2.0f);
        const auto first = q.begin(0, 10, 1, 4096, 64); require(q.pushPrepared(first, 0, a, a, 64), "Initial data");
        const auto stale = q.begin(64, 10, 2, 4096, 64, 0, true); require(q.pushPrepared(stale, 64, a, a, 64), "Stale revision data");
        const auto newest = q.begin(64, 10, 3, 4096, 64, 0, true);
        require(!q.pushPrepared(stale, 128, a, a, 64), "Invalidated generation published");
        require(q.pushPrepared(newest, 64, b, b, 64), "Newest revision data");
        require(q.consume(0, 10, l, r, 64) && q.consume(64, 10, l, r, 64) && l[20] == 2 && q.activeRevision() == 3, "Superseded plan adopted");
        q.reset(); const auto seek = q.begin(777, 11, 3, 4096, 64); require(q.pushPrepared(seek, 777, b, b, 64), "Seek prefill");
        require(!q.consume(777, 10, l, r, 64) && q.queuedFrames() == 64 && q.consume(777, 11, l, r, 64), "Seek generation fence");
    });
    suite.test("renderer mute/solo revision replacement uses new fixed mix at a callback boundary", []
    {
        Prepared f; TimelineAudioRenderer renderer(48000, 256); renderer.setPlan(f.plan(), f.bindings); renderer.prepare(1000, 5);
        eventually([&] { return renderer.ready(); });
        auto p = f.project; p.tracks[0].mute = true; ++p.editRevision;
        renderer.replacePlan(RenderPlanCompiler::compile(p), f.bindings, 1512, 5);
        eventually([&] { return renderer.ready(); }); float l[256], r[256];
        require(renderer.queue().consume(1000, 5, l, r, 256), "Old selection block");
        require(l[0] == .25f, "Old selection prematurely replaced");
        require(renderer.queue().consume(1256, 5, l, r, 256) && l[255] == 0 && l[254] > 0, "Mute transition did not ramp out");
        require(renderer.queue().consume(1512, 5, l, r, 256) && renderer.queue().activeRevision() == 2, "Mute revision not adopted");
        require(std::all_of(l, l + 256, [](float x) { return x == 0; }), "Muted revision contains old PCM");
    });
    suite.test("in-flight read is invalidated before newer seek can publish", []
    {
        Prepared f; f.source->block = true; TimelineAudioRenderer renderer(48000, 256); renderer.setPlan(f.plan(), f.bindings); renderer.prepare(0, 1);
        eventually([&] { return f.source->calls.load() != 0; });
        auto replacement = std::async(std::launch::async, [&] { renderer.prepare(5000, 2); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); f.source->release = true; replacement.get();
        eventually([&] { return renderer.ready(); }); float l[256], r[256];
        require(!renderer.queue().consume(0, 1, l, r, 256), "Stale in-flight PCM reached callback");
        require(renderer.queue().consume(5000, 2, l, r, 256) && l[0] == .25f, "New seek PCM missing");
    });
    suite.test("worker read error is exposed and prepared data cannot hide it", []
    {
        Prepared f; f.source->failRead = true; TimelineAudioRenderer renderer(48000, 256); renderer.setPlan(f.plan(), f.bindings); renderer.prepare(0, 1);
        eventually([&] { return renderer.status().failed(); });
        require(!renderer.ready() && renderer.queue().state() == PlaybackQueueState::failed, "Worker failure hidden by readiness");
        float l[256], r[256]; require(!renderer.queue().consume(0, 1, l, r, 256), "Failed queue returned PCM");
        require(std::all_of(l, l + 256, [](float x) { return x == 0; }), "Read failure output");
    });
    suite.test("media epoch invalidation rejects buffered PCM after worker join without RT destruction", []
    {
        Prepared f; TimelineAudioRenderer renderer(48000, 256); renderer.setPlan(f.plan(), f.bindings); renderer.prepare(1000, 1);
        eventually([&] { return renderer.ready(); }); renderer.stopWorker(); const auto before = renderer.queue().queuedFrames();
        ++f.source->epoch->value; float l[256], r[256]; bool consumed = true;
        { recorder_allocation_hook::Guard guard; consumed = renderer.queue().consume(1000, 1, l, r, 256); }
        noAllocations(); require(!consumed && renderer.status().failed() && !renderer.ready(), "Buffered obsolete media remained playable");
        require(renderer.queue().queuedFrames() == before && renderer.queue().state() == PlaybackQueueState::failed, "Stale media was consumed or hidden");
    });
    suite.test("engine queue adopts shortened/extended revision tails and distinguishes EOF from underrun", []
    {
        PlaybackBlockQueue q(64, 8); float pcm[64], l[64], r[64]; std::fill_n(pcm, 64, .5f);
        auto ticket = q.begin(0, 1, 1, 256, 64); require(q.pushPrepared(ticket, 0, pcm, pcm, 64), "Initial source");
        ticket = q.begin(64, 1, 2, 81, 64, 0, true); require(q.pushPrepared(ticket, 64, pcm, pcm, 17), "Shortened tail");
        require(q.consume(l, r, 64) == 64 && q.consume(l, r, 64) == 17 && q.submittedEnd() == 81 && q.activeRevision() == 2, "Revision tail did not use the new length");
        require(l[16] == .5f && l[17] == 0 && q.consume(l, r, 64) == 0 && q.state() == PlaybackQueueState::ended && q.underruns() == 0, "EOF counted as starvation");
        ticket = q.begin(81, 1, 3, 145, 64, 0, true); require(q.pushPrepared(ticket, 81, pcm, pcm, 64), "Extended tail");
        require(q.consume(l, r, 64) == 64 && q.activeRevision() == 3 && q.submittedEnd() == 145, "Old EOF blocked the new revision");
    });
    suite.test("all-or-nothing underrun triggers existing TimelineTransport buffering without allocation", []
    {
        PlaybackPcmQueue q(48000, 256); TimelineTransport transport(48000, 1000000, q, 48000); float l[256], r[256];
        transport.seek(0); transport.processOutput(stamp(0), l, r); transport.prepared(256, true);
        { recorder_allocation_hook::Guard guard; transport.processOutput(stamp(256), l, r); }
        noAllocations(); require(transport.snapshot().state == TransportState::buffering && transport.snapshot().underruns == 1 && q.underruns() == 1, "Queue starvation not linked to buffering");
        require(transport.snapshot().submittedEnd == 0 && std::all_of(l, l + 256, [](float x) { return x == 0; }), "Underrun consumed timeline");
    });
    suite.test("allocation hook self-check covers scalar, array, aligned and nothrow forms", []
    {
        { recorder_allocation_hook::Guard guard;
          auto* a = ::operator new(27); auto* b = ::operator new[](29, std::nothrow); auto* c = ::operator new(64, std::align_val_t(64));
          ::operator delete(a); ::operator delete[](b); ::operator delete(c, std::align_val_t(64)); }
        require(recorder_allocation_hook::allocations == 3 && recorder_allocation_hook::deletions == 3, "Allocation hook is inactive");
    });
    suite.test("RecorderAudioEngine consumes same queue without callback allocation and preserves stereo output mapping", []
    {
        RecorderAudioEngine engine; require(engine.openSynthetic(48000, 256, 0, 4).wasOk(), "Synthetic engine open");
        OutputMapping mapping; mapping.left = 1; mapping.right = 3; require(engine.setOutputMap(mapping).wasOk(), "Stereo output mapping");
        PlaybackPcmQueue q(48000, 256); std::array<float, 256> a{}, b{}; a.fill(.25f); b.fill(-.5f);
        require(q.push(0, 1, a.data(), b.data(), 256), "Queue engine PCM"); engine.setPlaybackQueue(&q);
        std::array<std::array<float, 256>, 4> pcm{}; float* outputs[]{pcm[0].data(), pcm[1].data(), pcm[2].data(), pcm[3].data()};
        { recorder_allocation_hook::Guard guard; engine.processBlock(stamp(0), nullptr, 0, nullptr, outputs, 4); }
        noAllocations(); require(pcm[1][0] == .25f && pcm[3][0] == -.5f && pcm[0][0] == 0 && pcm[2][0] == 0, "Engine changed stereo source/output channels");
        { recorder_allocation_hook::Guard guard; engine.processBlock(stamp(256), nullptr, 0, nullptr, outputs, 4); }
        noAllocations(); require(q.underruns() == 1 && pcm[1][0] == 0 && pcm[3][0] == 0, "Engine underrun handling"); engine.setPlaybackQueue(nullptr);
    });
    suite.test("SPSC repeated bank replacement/partial blocks never duplicate or overwrite PCM", []
    {
        PlaybackBlockQueue q(64, 8); std::atomic<int> consumed{-1}; std::atomic<bool> bad{false}, stop{false};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        const auto initial = q.begin(0, 9, 0, 64000, 64); float zero[64]{}; require(q.pushPrepared(initial, 0, zero, zero, 64), "Initial bank");
        std::thread producer([&]
        {
            for (int revision = 1; revision < 500; ++revision)
            {
                while (consumed.load(std::memory_order_acquire) < revision - 1 && !stop.load()) std::this_thread::yield();
                if (stop.load()) break;
                float pcm[64]; std::fill_n(pcm, 64, static_cast<float>(revision));
                const auto ticket = q.begin(revision * 64, 9, revision, 64000, 64, 0, true);
                if (!q.pushPrepared(ticket, revision * 64, pcm, pcm, 64)) bad = true;
            }
        });
        for (int revision = 0; revision < 500; ++revision)
        {
            float l[32], r[32];
            for (int half = 0; half < 2; ++half)
            {
                while (!q.consume(revision * 64 + half * 32, 9, l, r, 32))
                { if (std::chrono::steady_clock::now() >= deadline) { stop = true; bad = true; break; } std::this_thread::yield(); }
                if (stop.load()) break;
                for (int i = 0; i < 32; ++i) if (l[i] != revision || r[i] != revision) bad = true;
            }
            consumed.store(revision, std::memory_order_release);
            if (stop.load()) break;
        }
        producer.join(); require(!bad.load() && q.queuedFrames() == 0 && q.activeRevision() == 499, "Revision bank data race/overwrite");
    });
    suite.test("provider pump depth is time based even for small ASIO blocks", []
    {
        PlaybackProviderPump pump(48000, 32, std::make_unique<ToneBlockProvider>());
        require(pump.queue().ready() && pump.queue().prefetchedFrames() >= 12000, "Fixed block-count prefetch below 250ms");
    });
    return suite.result("audio-prefetch-underrun");
}
