#include "../src/WebGuestTransport.h"
#include "../../tests/livemix017/r1/Packets.h"
#include <rtc/rtc.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

using gocue::livemix::collab::WebGuestTransport;
namespace fixture = livemix_webguest_fixture;
namespace
{
using Properties = std::initializer_list<std::pair<const char*, juce::var>>;
juce::var object(Properties properties)
{
    auto* result = new juce::DynamicObject;
    for (const auto& item : properties) result->setProperty(item.first, item.second);
    return juce::var(result);
}
// Library callbacks enqueue output; only this writer can wait on stdout.
class JsonOutput
{
public:
    JsonOutput() : writer([this] { run(); }) {}
    ~JsonOutput()
    {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_one();
        writer.join();
    }
    void push(juce::var value)
    {
        { std::lock_guard<std::mutex> lock(mutex); queue.push_back(std::move(value)); }
        wake.notify_one();
    }
private:
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<juce::var> queue;
    bool stopping = false;
    std::thread writer;
    void run()
    {
        for (;;)
        {
            juce::var value;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) return;
                value = std::move(queue.front()); queue.pop_front();
            }
            std::cout << juce::JSON::toString(value, true).toStdString() << std::endl;
        }
    }
};
const char* stateName(WebGuestTransport::State state)
{
    switch (state)
    {
        case WebGuestTransport::State::off: return "off";
        case WebGuestTransport::State::connecting: return "connecting";
        case WebGuestTransport::State::open: return "open";
        case WebGuestTransport::State::closed: return "closed";
        case WebGuestTransport::State::failed: return "failed";
    }
    return "failed";
}
}

int main(int argc, char** argv)
{
    int count = 64;
    uint32_t generation = 1;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--packets" && i + 1 < argc) count = juce::String(argv[++i]).getIntValue();
        else if (std::string(argv[i]) == "--generation" && i + 1 < argc)
            generation = static_cast<uint32_t>(juce::String(argv[++i]).getLargeIntValue());
        else { std::cerr << "Usage: LiveMixWebGuestProbe [--packets 64] [--generation 1]\n"; return 1; }
    }
    if (count < 1 || count > 10000 || generation == 0) return 1;
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel, const std::string& message)
    {
        std::cerr << message << std::endl;
    });
    JsonOutput output;
    const auto context = fixture::connection(generation);
    std::atomic<bool> stop { false }, senderDone { false }, failed { false };
    std::mutex receivedMutex;
    std::vector<bool> seen(static_cast<size_t>(count));
    int received = 0, mismatches = 0;
    WebGuestTransport transport;
    WebGuestTransport::Callbacks callbacks;
    callbacks.onLocalDescription = [&](uint32_t gen, const juce::String& type, const juce::String& sdp)
    {
        output.push(object({ { "type", "description" }, { "generation", juce::int64(gen) }, { "sdpType", type }, { "sdp", sdp } }));
    };
    callbacks.onLocalCandidate = [&](uint32_t gen, const juce::String& candidate, const juce::String& mid, int index)
    {
        output.push(object({ { "type", "candidate" }, { "generation", juce::int64(gen) }, { "candidate", candidate },
                             { "mid", mid }, { "mLineIndex", index } }));
    };
    callbacks.onGatheringComplete = [&](uint32_t gen)
    {
        output.push(object({ { "type", "endOfCandidates" }, { "generation", juce::int64(gen) } }));
    };
    callbacks.onState = [&](uint32_t gen, WebGuestTransport::State state)
    {
        if (state == WebGuestTransport::State::failed) failed.store(true);
        const auto stats = transport.snapshot();
        output.push(object({ { "type", "state" }, { "generation", juce::int64(gen) }, { "state", stateName(state) },
                             { "ordered", stats.ordered }, { "maxRetransmits", stats.maxRetransmits } }));
    };
    callbacks.onPacket = [&](uint32_t gen, const uint8_t* bytes, size_t size, uint64_t)
    {
        std::lock_guard<std::mutex> lock(receivedMutex);
        const auto seq = fixture::sequence(bytes);
        const auto expected = fixture::audio(true, seq, context);
        if (gen != generation || seq >= uint32_t(count) || size != expected.size()
            || !std::equal(expected.begin(), expected.end(), bytes) || seen[seq]) ++mismatches;
        else { seen[seq] = true; ++received; }
        if (received == count)
            output.push(object({ { "type", "received" }, { "generation", juce::int64(generation) }, { "packets", received },
                                 { "mismatches", mismatches } }));
    };
    output.push(object({ { "type", "ready" }, { "generation", juce::int64(generation) }, { "packets", count },
                         { "session", "123456789abcdef0" }, { "loopbackOnly", true } }));
    transport.startAcceptedGuest(context, { true }, std::move(callbacks));
    std::thread sender([&]
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (!stop.load() && transport.snapshot().state == WebGuestTransport::State::connecting
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (transport.snapshot().state != WebGuestTransport::State::open) { failed.store(true); return; }
        for (int seq = 0; seq < count && !stop.load(); ++seq)
        {
            const auto packet = fixture::audio(false, uint32_t(seq), context);
            if (!transport.sendAudio(generation, packet.data(), packet.size())) failed.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        senderDone.store(true);
        output.push(object({ { "type", "sent" }, { "generation", juce::int64(generation) }, { "packets", count } }));
    });
    bool finish = false;
    try
    {
        std::string line;
        while (std::getline(std::cin, line))
        {
            auto message = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
            if (!message.isObject()) throw std::invalid_argument("Expected JSON object");
            const auto type = message["type"].toString();
            const auto gen = static_cast<uint32_t>(int64_t(message["generation"]));
            if (type == "finish") { finish = true; break; }
            if (type == "description") transport.applyRemoteDescription(gen, message["sdpType"].toString(), message["sdp"].toString());
            else if (type == "candidate") transport.addRemoteCandidate(gen, message["candidate"].toString(), message["mid"].toString(), int(message["mLineIndex"]));
            else if (type == "endOfCandidates") transport.endRemoteCandidates(gen);
            else throw std::invalid_argument("Unknown input message");
        }
    }
    catch (const std::exception& error)
    {
        failed.store(true);
        output.push(object({ { "type", "error" }, { "message", error.what() } }));
    }
    if (!finish) failed.store(true);
    if (!senderDone.load()) stop.store(true);
    sender.join();
    transport.close(); // quiesce callbacks before examining received/seen
    const auto stats = transport.snapshot();
    const bool ok = !failed.load() && senderDone.load() && received == count && mismatches == 0
        && stats.sent == uint64_t(count) && stats.dropped == 0;
    output.push(object({ { "type", "result" }, { "generation", juce::int64(generation) }, { "ok", ok },
                         { "sent", juce::int64(stats.sent) }, { "received", received }, { "mismatches", mismatches },
                         { "dropped", juce::int64(stats.dropped) }, { "ordered", stats.ordered },
                         { "maxRetransmits", stats.maxRetransmits } }));
    return ok ? 0 : 1;
}
