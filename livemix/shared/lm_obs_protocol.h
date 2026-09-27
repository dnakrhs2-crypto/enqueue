/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 LiveMix contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef LM_OBS_PROTOCOL_H
#define LM_OBS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef _WIN64
#error LiveMix shared memory requires Windows x64
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#define LM_OBS_STATIC_ASSERT static_assert
#else
#define LM_OBS_STATIC_ASSERT _Static_assert
#endif

#define LM_OBS_RING_NAME L"Local\\LiveMix.ObsAudio.v1"
#define LM_OBS_READERS_NAME L"Local\\LiveMix.ObsAudio.v1.Readers"
#define LM_OBS_MAGIC 0x424F4D4Cu
#define LM_OBS_PROTOCOL_MAJOR 1
#define LM_OBS_CHANNELS 2
#define LM_OBS_CAPACITY_FRAMES 32768u
#define LM_OBS_MAX_READERS 16

typedef struct lm_obs_ring_header {
    uint32_t magic, protocol_major, protocol_minor, header_bytes;
    uint32_t mapping_bytes, data_offset, channels, capacity_frames;
    volatile int64_t epoch;
    volatile int64_t sample_rate;
    volatile int64_t write_frames;
    volatile int64_t heartbeat_qpc;
    volatile int64_t qpc_frequency;
    volatile int64_t send_enabled;
    volatile int64_t writer_pid;
    volatile int64_t silent_frames;
    /* Bytes 0..7 hold the writer's in-flight end position (see below).
     * The remaining bytes stay zero. Initialize the entire mapping to zero. */
    uint8_t reserved[256 - 32 - 8 * 8];
} lm_obs_ring_header;

typedef struct lm_obs_reader_slot {
    volatile int64_t pid, heartbeat_qpc, kind /* 1 source, 2 filter */, fill_us;
} lm_obs_reader_slot;

typedef struct lm_obs_readers {
    uint32_t magic, protocol_major, slot_count, reserved;
    lm_obs_reader_slot slot[LM_OBS_MAX_READERS];
} lm_obs_readers;

LM_OBS_STATIC_ASSERT(sizeof(lm_obs_ring_header) == 256, "ring header ABI");
LM_OBS_STATIC_ASSERT(offsetof(lm_obs_ring_header, epoch) == 32, "counter alignment");
LM_OBS_STATIC_ASSERT(offsetof(lm_obs_ring_header, reserved) == 96, "reservation alignment");
LM_OBS_STATIC_ASSERT(sizeof(lm_obs_reader_slot) == 32, "reader slot ABI");
LM_OBS_STATIC_ASSERT(offsetof(lm_obs_readers, slot) == 16, "reader counter alignment");
LM_OBS_STATIC_ASSERT(sizeof(lm_obs_readers) == 528, "readers mapping ABI");

/* An aligned volatile load is atomic on Windows x64. Do not use a read/modify/
 * write interlocked operation for loads: the audio mapping is READ ONLY in OBS.
 * Full barriers also keep the PCM copy before the reader's final validation. */
static inline int64_t lm_obs_load_acquire(const volatile int64_t* value)
{
    int64_t result;
    _ReadWriteBarrier();
    result = *value;
    MemoryBarrier();
    _ReadWriteBarrier();
    return result;
}

static inline void lm_obs_store_release(volatile int64_t* value, int64_t next)
{
    _ReadWriteBarrier();
    InterlockedExchange64((volatile LONG64*) value, (LONG64) next);
}

/* One writer; any number of independent readers. The writer publishes the end
 * of an upcoming copy BEFORE touching PCM. Otherwise a reader near the back of
 * the ring could copy overwritten samples while write_frames still described
 * the previous block. Both reader checks include this in-flight reservation.
 * Epoch/format reset belongs to the owner: stop writing, disable send, reset
 * both positions (write_frames and reserved[0..7]), then publish the new epoch.
 * Ring capacity must be a power of two; PCM is interleaved stereo float32. */
static inline void lm_obs_write(lm_obs_ring_header* h, float* ring, const float* left, const float* right, uint32_t frames)
{
    const int64_t start = lm_obs_load_acquire(&h->write_frames);
    const int64_t end = start + frames;
    const uint32_t capacity = h->capacity_frames;
    uint32_t i = frames > capacity ? frames - capacity : 0;
    if (frames == 0)
        return;
    lm_obs_store_release((volatile int64_t*) h->reserved, end);
    for (; i < frames; ++i) {
        const size_t index = (size_t) ((uint64_t) (start + i) & (capacity - 1u)) * LM_OBS_CHANNELS;
        ring[index] = left[i];
        ring[index + 1] = right[i];
    }
    lm_obs_store_release(&h->write_frames, end);
}

static inline void lm_obs_write_silence(lm_obs_ring_header* h, float* ring, uint32_t frames)
{
    const int64_t start = lm_obs_load_acquire(&h->write_frames);
    const int64_t end = start + frames;
    const uint32_t capacity = h->capacity_frames;
    uint32_t i = frames > capacity ? frames - capacity : 0;
    if (frames == 0)
        return;
    lm_obs_store_release((volatile int64_t*) h->reserved, end);
    for (; i < frames; ++i) {
        const size_t index = (size_t) ((uint64_t) (start + i) & (capacity - 1u)) * LM_OBS_CHANNELS;
        ring[index] = 0.0f;
        ring[index + 1] = 0.0f;
    }
    lm_obs_store_release(&h->silent_frames, lm_obs_load_acquire(&h->silent_frames) + frames);
    lm_obs_store_release(&h->write_frames, end);
}

/* Returns frames copied, or -1 on overrun/epoch change/invalid position.
 * Failure leaves read_pos unchanged; discard dst and resync to write_frames
 * minus the chosen target. A reader exactly capacity behind is an overrun. */
static inline int32_t lm_obs_read(const lm_obs_ring_header* h, const float* ring, int64_t epoch,
                                int64_t* read_pos, float* dst_interleaved, uint32_t max_frames)
{
    const int64_t start = *read_pos;
    int64_t written, reserved;
    uint32_t count, index, first;
    const uint32_t capacity = h->capacity_frames;
    if (lm_obs_load_acquire(&h->epoch) != epoch)
        return -1;
    written = lm_obs_load_acquire(&h->write_frames);
    reserved = lm_obs_load_acquire((const volatile int64_t*) h->reserved);
    if (start < 0 || start > written || written - start >= capacity || reserved - start >= capacity)
        return -1;
    count = (uint32_t) (written - start);
    if (count > max_frames)
        count = max_frames;
    if (count == 0)
        return 0;
    index = (uint32_t) ((uint64_t) start & (capacity - 1u));
    first = count < capacity - index ? count : capacity - index;
    memcpy(dst_interleaved, ring + (size_t) index * LM_OBS_CHANNELS, (size_t) first * LM_OBS_CHANNELS * sizeof(float));
    if (first < count)
        memcpy(dst_interleaved + (size_t) first * LM_OBS_CHANNELS, ring, (size_t) (count - first) * LM_OBS_CHANNELS * sizeof(float));
    MemoryBarrier();
    _ReadWriteBarrier();
    written = lm_obs_load_acquire(&h->write_frames);
    reserved = lm_obs_load_acquire((const volatile int64_t*) h->reserved);
    if (written < start + count || written - start >= capacity || reserved - start >= capacity
        || lm_obs_load_acquire(&h->epoch) != epoch)
        return -1;
    *read_pos = start + count;
    return (int32_t) count;
}

#undef LM_OBS_STATIC_ASSERT
#ifdef __cplusplus
}
#endif
#endif
