#pragma once
#include "CollabProtocol.h"
#include "lm_asrc.h"
#include <atomic>
#include <functional>
#include <vector>

namespace gocue::livemix::collab
{
class CollabSender
{
public:
    using Sink = std::function<void (const Header&, const uint8_t*)>;
    CollabSender();
    ~CollabSender();
    // Lifecycle thread, with producer and consumer excluded by CollabAudio.
    void prepare (double deviceRate, int maxBlock);
    void resetSession() noexcept;
    void push (const float* left, const float* right, int frames) noexcept; // audio producer
    void drain (const Sink&); // one sender thread (or an offline test)
    void* getEvent() const noexcept { return event; }
    uint64_t getNextFrame() const noexcept { return publishedFrame.load(); }
    uint64_t getOverruns() const noexcept { return overruns.load(); }

private:
    void* event = nullptr;
    std::vector<float> fifo;
    std::vector<double> asrcStorage;
    lm_asrc* asrc = nullptr;
    uint64_t capacity = 0, nextFrame = 0;
    std::atomic<uint64_t> read { 0 }, write { 0 }, publishedFrame { 0 }, overruns { 0 };
    std::array<float, framesPerPacket * channels> converted {};
    std::array<float, framesPerPacket> packetL {}, packetR {};
    int packetFill = 0;
};
}
