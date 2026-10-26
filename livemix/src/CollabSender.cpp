#include "CollabSender.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>

namespace gocue::livemix::collab
{
CollabSender::CollabSender() : event (CreateEventW (nullptr, FALSE, FALSE, nullptr)) {}
CollabSender::~CollabSender() { if (event != nullptr) CloseHandle (event); }

void CollabSender::prepare (double rate, int maxBlock, uint32_t wireRate)
{
    if (! isValidWireRate (wireRate)) return;
    capacity = 1;
    while (capacity < (uint64_t) std::max (rate * 2.0, maxBlock * 2.0)) capacity *= 2;
    fifo.assign ((size_t) capacity * channels, 0.0f);
    read.store (0); write.store (0); packetFill = 0;
    asrc = nullptr;
    if (rate != wireRate)
    {
        asrcStorage.resize ((lm_asrc_size (channels) + sizeof (double) - 1) / sizeof (double));
        asrc = reinterpret_cast<lm_asrc*> (asrcStorage.data());
        lm_asrc_init (asrc, channels, rate, wireRate);
    }
}

void CollabSender::resetSession() noexcept
{
    nextFrame = 0; publishedFrame.store (0); packetFill = 0;
    read.store (0); write.store (0); overruns.store (0);
    if (asrc != nullptr) lm_asrc_reset (asrc);
}

void CollabSender::push (const float* left, const float* right, int frames) noexcept
{
    if (frames <= 0 || capacity == 0) return;
    const auto w = write.load (std::memory_order_relaxed);
    const auto r = read.load (std::memory_order_acquire);
    if ((uint64_t) frames > capacity - (w - r)) { overruns.fetch_add (1); return; }
    for (int i = 0; i < frames; ++i)
    {
        const auto index = (size_t) ((w + i) & (capacity - 1)) * channels;
        fifo[index] = left != nullptr ? left[i] : 0.0f;
        fifo[index + 1] = right != nullptr ? right[i] : 0.0f;
    }
    write.store (w + frames, std::memory_order_release);
    if (event != nullptr) SetEvent (event);
}

void CollabSender::drain (const Sink& sink)
{
    auto r = read.load (std::memory_order_relaxed);
    const auto w = write.load (std::memory_order_acquire);
    if (capacity == 0) return;
    for (;;)
    {
        const auto index = r & (capacity - 1);
        const int available = (int) std::min (w - r, capacity - index);
        const auto* input = fifo.data() + (size_t) index * channels;
        int used = 0, made = 0;
        const int wanted = framesPerPacket - packetFill;
        if (asrc != nullptr)
            made = lm_asrc_process (asrc, input, available, &used, converted.data(), wanted);
        else
        {
            made = used = std::min (available, wanted);
            std::copy_n (input, made * channels, converted.data());
        }
        r += used;
        for (int i = 0; i < made; ++i)
        {
            packetL[(size_t) packetFill] = converted[(size_t) i * 2];
            packetR[(size_t) packetFill++] = converted[(size_t) i * 2 + 1];
        }
        if (packetFill == framesPerPacket)
        {
            std::array<uint8_t, audioBytes> payload;
            packPcm24 (payload.data(), packetL.data(), packetR.data(), framesPerPacket);
            Header h; h.firstFrame = nextFrame; h.frames = framesPerPacket; h.payloadBytes = audioBytes;
            h.seq = (uint32_t) (nextFrame / framesPerPacket);
            sink (h, payload.data());
            nextFrame += framesPerPacket; publishedFrame.store (nextFrame, std::memory_order_relaxed);
            packetFill = 0;
        }
        if (used == 0 && made == 0) break;
    }
    read.store (r, std::memory_order_release);
}
}
