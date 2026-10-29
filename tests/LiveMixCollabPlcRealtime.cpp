// Small Debug-CRT executable: exercise the production PLC without a GUI or an
// audio device, and count heap operations only inside the audio-thread methods.
#include "CollabPlc.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <crtdbg.h>

namespace
{
thread_local bool measuring = false;
thread_local unsigned allocations = 0;
int hook (int, void*, size_t, int, long, const unsigned char*, int)
{
    if (measuring) ++allocations;
    return 1;
}
}
int main()
{
#if ! defined (_DEBUG)
    std::puts ("Run this allocation check in Debug."); return 2;
#else
    const auto previous = _CrtSetAllocHook (hook);
    unsigned total = 0;
    for (int rate : { 44100, 48000, 96000, 192000 })
    {
        gocue::livemix::collab::CollabPlc plc; plc.prepare (rate);
        std::array<float, 256> audio {};
        allocations = 0; measuring = true;
        for (int episode = 0; episode < 20; ++episode)
        {
            for (int block = 0; block < rate / 128 / 4; ++block)
            {
                for (int i = 0; i < 128; ++i)
                    audio[(size_t) i * 2] = audio[(size_t) i * 2 + 1]
                        = 0.4f * (float) std::sin (6.283185307179586 * 137 * (block * 128 + i) / rate);
                plc.receive (audio.data(), 128);
            }
            // Short hole, recovery, then a refill long enough to reach silence.
            plc.conceal (audio.data(), 128); plc.receive (audio.data(), 128);
            for (int i = 0; i < rate / 128 / 10; ++i) plc.conceal (audio.data(), 128);
            plc.receive (audio.data(), 128);
            // Repeatedly interrupt recovery with real silence, then complete
            // the overlap with actual input before starting a new loss.
            for (int i = 0; i < 100; ++i)
            {
                plc.conceal (audio.data(), 128);
                audio.fill (0); plc.receive (audio.data(), 128);
            }
            for (int i = 0; i < rate / 128 / 20; ++i)
            {
                audio.fill (0); plc.receive (audio.data(), 128);
            }
            plc.conceal (audio.data(), 128); plc.reset();
        }
        // Continuing speech, short contiguous histories and post-budget joins.
        // Split both missing/received segments as ordinary device callbacks do.
        for (double goodMs : { 5.0, 10.0, 27.5 })
        {
            plc.reset(); int position = 0;
            const auto render = [&] (bool lost, int frames)
            {
                while (frames > 0)
                {
                    const int n = frames < 128 ? frames : 128;
                    if (lost) plc.conceal (audio.data(), n);
                    else
                    {
                        for (int i = 0; i < n; ++i)
                            audio[(size_t) i * 2] = audio[(size_t) i * 2 + 1]
                                = 0.4f * (float) std::sin (6.283185307179586 * 137 * (position + i) / rate);
                        plc.receive (audio.data(), n);
                    }
                    position += n; frames -= n;
                }
            };
            render (false, rate / 10);
            for (int i = 0; i < 30; ++i)
            {
                render (true, (int) std::lround (rate * 0.0025));
                render (false, (int) std::lround (rate * goodMs / 1000.0));
            }
        }
        measuring = false; total += allocations;
        std::printf ("PLC CRT rate=%d allocations_reallocations_frees=%u\n", rate, allocations);
    }
    _CrtSetAllocHook (previous);
    return total == 0 ? 0 : 1;
#endif
}
