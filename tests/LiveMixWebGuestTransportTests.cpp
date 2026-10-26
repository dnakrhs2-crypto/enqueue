#include "../livemix/src/WebGuestTransport.h"
#include "livemix017/r1/Packets.h"
#include <rtc/rtc.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

namespace gocue::livemix::collab
{
namespace fixture = livemix_webguest_fixture;
namespace
{
struct Peers
{
    WebGuestTransport& host;
    contracts::WebConnection context;
    std::shared_ptr<rtc::PeerConnection> browser;
    std::shared_ptr<rtc::DataChannel> channel;
    std::mutex mutex;
    std::deque<std::function<void()>> work;
    std::vector<std::vector<uint8_t>> atHost, atBrowser;
    std::vector<uint32_t> receivedGenerations;
    std::atomic<bool> browserOpen { false }, negotiated { false }, callbackError { false };
    std::atomic<int> gatheringComplete { 0 };
    int earlyCandidates = 0;
    bool answerApplied = false, hostOnlyCandidates = true, applicationOnly = false;

    Peers(WebGuestTransport& transport, uint32_t generation) : host(transport), context(fixture::connection(generation))
    {
        rtc::Configuration configuration;
        configuration.disableAutoNegotiation = true;
        configuration.maxMessageSize = 1200;
        browser = std::make_shared<rtc::PeerConnection>(configuration);
        browser->onLocalCandidate([this](rtc::Candidate candidate)
        {
            post([this, candidate]
            {
                if (!answerApplied) ++earlyCandidates;
                hostOnlyCandidates &= candidate.type() == rtc::Candidate::Type::Host;
                host.addRemoteCandidate(context.generation, candidate.candidate(), candidate.mid(), 0);
            });
        });
        browser->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state)
        {
            if (state != rtc::PeerConnection::GatheringState::Complete) return;
            post([this]
            {
                auto answer = browser->localDescription().value();
                answer.extractCandidates(); // connectivity must use the early trickled candidates
                host.endRemoteCandidates(context.generation);
                host.applyRemoteDescription(context.generation, "answer", std::string(answer));
                answerApplied = true;
            });
        });
        browser->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                channel = dc;
            }
            const std::weak_ptr<rtc::DataChannel> weak(dc);
            dc->onOpen([this, weak]
            {
                const auto opened = weak.lock();
                if (!opened) return;
                const auto reliability = opened->reliability();
                negotiated.store(opened->label() == contracts::dataChannelLabel
                    && opened->protocol() == contracts::dataChannelProtocol && reliability.unordered
                    && reliability.maxRetransmits == 0u && !reliability.maxPacketLifeTime);
                browserOpen.store(true);
            });
            dc->onMessage([this](rtc::message_variant message)
            {
                const auto* bytes = std::get_if<rtc::binary>(&message);
                if (!bytes) { callbackError.store(true); return; }
                std::lock_guard<std::mutex> lock(mutex);
                const auto* data = reinterpret_cast<const uint8_t*>(bytes->data());
                atBrowser.emplace_back(data, data + bytes->size());
            });
        });
        WebGuestTransport::Callbacks callbacks;
        callbacks.onLocalDescription = [this](uint32_t gen, const juce::String& type, const juce::String& sdp)
        {
            post([this, gen, type, sdp]
            {
                if (gen != context.generation) { callbackError.store(true); return; }
                rtc::Description offer(sdp.toStdString(), type.toStdString());
                applicationOnly = offer.hasApplication() && offer.mediaCount() == 1 && type == "offer";
                browser->setRemoteDescription(offer);
                browser->setLocalDescription(rtc::Description::Type::Answer);
            });
        };
        callbacks.onLocalCandidate = [this](uint32_t gen, const juce::String& text, const juce::String& mid, int index)
        {
            post([this, gen, text, mid, index]
            {
                if (gen != context.generation || index != 0) { callbackError.store(true); return; }
                rtc::Candidate candidate(text.toStdString(), mid.toStdString());
                hostOnlyCandidates &= candidate.type() == rtc::Candidate::Type::Host;
                browser->addRemoteCandidate(candidate);
            });
        };
        callbacks.onGatheringComplete = [this](uint32_t gen)
        {
            if (gen != context.generation) callbackError.store(true);
            gatheringComplete.fetch_add(1);
        };
        callbacks.onPacket = [this](uint32_t gen, const uint8_t* bytes, size_t size, uint64_t timestamp)
        {
            std::lock_guard<std::mutex> lock(mutex);
            atHost.emplace_back(bytes, bytes + size);
            receivedGenerations.push_back(gen);
            if (timestamp == 0) callbackError.store(true);
        };
        host.startAcceptedGuest(context, { true }, std::move(callbacks));
    }
    ~Peers()
    {
        host.close();
        browser->resetCallbacks();
        auto dc = getChannel();
        if (dc) { dc->resetCallbacks(); dc->close(); }
        browser->close();
    }
    void post(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        work.push_back(std::move(fn));
    }
    std::shared_ptr<rtc::DataChannel> getChannel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return channel;
    }
    bool wait(const std::function<bool()>& ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do
        {
            std::deque<std::function<void()>> jobs;
            { std::lock_guard<std::mutex> lock(mutex); jobs.swap(work); }
            try { for (auto& job : jobs) job(); }
            catch (const std::exception&) { callbackError.store(true); return false; }
            if (ready()) return true;
            if (callbackError.load() || host.snapshot().state == WebGuestTransport::State::failed) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
    bool connect()
    {
        return wait([&] { return browserOpen.load() && gatheringComplete.load() == 1
            && host.snapshot().state == WebGuestTransport::State::open; });
    }
    bool packets(size_t hostCount, size_t browserCount)
    {
        return wait([&]
        {
            std::lock_guard<std::mutex> lock(mutex);
            return atHost.size() >= hostCount && atBrowser.size() >= browserCount;
        });
    }
    void send(const std::vector<uint8_t>& bytes)
    {
        getChannel()->send(reinterpret_cast<const rtc::byte*>(bytes.data()), bytes.size());
    }
};
}

class WebGuestTransportTests final : public juce::UnitTest
{
public:
    WebGuestTransportTests() : UnitTest("LiveMix WebGuest transport", "LiveMix") {}
    void runTest() override
    {
        beginTest("20ms backlog keeps newest audio and counts old/expired packets");
        {
            using Queue = WebGuestTransport::AudioQueue;
            // 20 ms per downlink stream, two streams (host + one forwarded native participant).
            for (const auto rate : { 44100u, 48000u })
                expectEquals(int((rate * 20 + 120000 - 1) / 120000 * contracts::remoteSlots), int(Queue::capacity));
            constexpr int capacity = int(Queue::capacity);
            Queue queue;
            uint64_t dropped = 0;
            constexpr uint64_t start = 1'000'000'000;
            for (uint32_t seq = 0; seq < 40; ++seq)
            {
                const auto packet = fixture::audio(false, seq);
                dropped += queue.push(packet.data(), packet.size(), start);
            }
            expectEquals(int(dropped), 40 - capacity);
            expectEquals(int(queue.count), capacity);
            dropped += queue.trim((capacity - 2) * Queue::packetBytes, start);
            expectEquals(int(dropped), 38);
            expectEquals(int(queue.count), 2);
            expectEquals(int(fixture::sequence(queue.pop().data.data())), 38);
            expectEquals(int(fixture::sequence(queue.pop().data.data())), 39);
            const auto packet = fixture::audio(false, 40);
            queue.push(packet.data(), packet.size(), start);
            expectEquals(int(queue.trim(0, start + Queue::maxAgeNs)), 1);
            expectEquals(int(queue.count), 0);
            queue.push(packet.data(), packet.size(), start);
            expectEquals(int(queue.trim(capacity * Queue::packetBytes, start)), 1);
        }

        beginTest("in-band unordered rexmit=0, early ICE, mono16/stereo24 and control contents");
        {
            WebGuestTransport host;
            expect(host.snapshot().state == WebGuestTransport::State::off);
            Peers pair(host, 7);
            const bool connected = pair.connect();
            expect(connected);
            if (connected)
            {
                expect(pair.negotiated.load());
                expect(pair.applicationOnly && pair.hostOnlyCandidates);
                expect(pair.earlyCandidates > 0);
                expectEquals(pair.gatheringComplete.load(), 1);
                expect(!host.snapshot().ordered);
                expectEquals(host.snapshot().maxRetransmits, 0);
                for (uint32_t seq = 0; seq < 16; ++seq)
                {
                    const auto mono = fixture::audio(true, seq, pair.context);
                    const auto stereo = fixture::audio(false, seq, pair.context);
                    pair.send(mono);
                    expect(host.sendAudio(7, stereo.data(), stereo.size()));
                    expect(pair.packets(seq + 1, seq + 1));
                    std::lock_guard<std::mutex> lock(pair.mutex);
                    if (pair.atHost.size() > seq) expect(pair.atHost[seq] == mono);
                    if (pair.atBrowser.size() > seq) expect(pair.atBrowser[seq] == stereo);
                }
                auto control = fixture::control(false, pair.context);
                expect(host.sendControl(7, control.data(), control.size()));
                expect(pair.packets(16, 17));
                { std::lock_guard<std::mutex> lock(pair.mutex); expect(pair.atBrowser.back() == control); }

                beginTest("per-person downlink: a forwarded LiveMix speaker reaches the web, its own voice and foreign controls do not");
                auto forwarded = fixture::audio(false, 16, pair.context);
                fixture::put(forwarded, 16, 3, 4); // origin = another LiveMix participant; linkSender stays the host
                expect(host.sendAudio(7, forwarded.data(), forwarded.size()));
                expect(pair.packets(16, 18));
                { std::lock_guard<std::mutex> lock(pair.mutex); expect(pair.atBrowser.back() == forwarded); }
                auto echo = fixture::audio(false, 17, pair.context);
                fixture::put(echo, 16, pair.context.guestId, 4); // the web guest's own voice is never sent back
                expect(!host.sendAudio(7, echo.data(), echo.size()));
                auto foreignControl = fixture::control(false, pair.context);
                fixture::put(foreignControl, 16, 3, 4); // controls on this link are the host's own
                expect(!host.sendControl(7, foreignControl.data(), foreignControl.size()));

                beginTest("bad envelope, text, sender/session binding and obsolete generations are dropped");
                auto bad = fixture::audio(true, 99, pair.context);
                bad[0] ^= 0xff; pair.send(bad);
                bad = fixture::audio(true, 99, pair.context); bad[4] = 1; pair.send(bad);
                bad = fixture::audio(true, 99, pair.context); bad.pop_back(); pair.send(bad);
                bad = fixture::audio(true, 99, pair.context); bad[8] ^= 1; pair.send(bad);
                bad = fixture::audio(true, 99, pair.context); bad[16] = 3; pair.send(bad);
                bad = fixture::audio(true, 99, pair.context); bad[36] = 1; pair.send(bad);
                pair.send(std::vector<uint8_t>(12, 0));
                pair.getChannel()->send(std::string("not binary"));
                auto stereo = fixture::audio(false, 100, pair.context);
                expect(!host.sendAudio(6, stereo.data(), stereo.size()));
                host.applyRemoteDescription(6, "offer", "obsolete");
                host.addRemoteCandidate(6, "obsolete", "0", 0);
                host.endRemoteCandidates(6);
                expect(pair.wait([&] { return host.snapshot().dropped >= 14; }));
                expectEquals(int(host.snapshot().received), 16);
                expectEquals(int(host.snapshot().dropped), 14); // 12 bad/obsolete + echo + foreign control above
                expect(host.snapshot().state == WebGuestTransport::State::open);
                pair.send(fixture::audio(true, 100, pair.context));
                expect(host.sendAudio(7, stereo.data(), stereo.size()));
                expect(pair.packets(17, 19));
                expectEquals(int(host.snapshot().received), 17);
                expect(!pair.callbackError.load());
            }
        }

        beginTest("20 accepted generations connect, exchange packets, close and replace cleanly");
        {
            WebGuestTransport host;
            for (uint32_t generation = 1; generation <= 20; ++generation)
            {
                Peers pair(host, generation);
                if (generation > 1)
                    host.applyRemoteDescription(generation - 1, "answer", "closed generation");
                const bool connected = pair.connect();
                expect(connected, "generation " + juce::String(generation));
                if (!connected) break;
                const auto mono = fixture::audio(true, generation, pair.context);
                const auto stereo = fixture::audio(false, generation, pair.context);
                pair.send(mono);
                expect(host.sendAudio(generation, stereo.data(), stereo.size()));
                expect(pair.packets(1, 1));
                {
                    std::lock_guard<std::mutex> lock(pair.mutex);
                    expect(pair.atHost.size() == 1 && pair.atHost.front() == mono);
                    expect(pair.atBrowser.size() == 1 && pair.atBrowser.front() == stereo);
                    expect(pair.receivedGenerations.size() == 1 && pair.receivedGenerations.front() == generation);
                }
                host.close();
                expect(host.snapshot().state == WebGuestTransport::State::closed);
                expectEquals(int(host.snapshot().bufferedBytes), 0);
                expect(!host.sendAudio(generation, stereo.data(), stereo.size()));
                host.close();
            }
        }
        // The future completes only after every peer/transport init token is
        // released and the library's threads and SCTP/ICE resources are joined.
        beginTest("all peer resources are released after the repeated connections");
        expect(rtc::Cleanup().wait_for(std::chrono::seconds(15)) == std::future_status::ready);
    }
};
static WebGuestTransportTests webGuestTransportTests;
}
