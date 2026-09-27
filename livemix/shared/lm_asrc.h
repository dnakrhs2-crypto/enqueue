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
#ifndef LM_ASRC_H
#define LM_ASRC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lm_asrc lm_asrc;

/* Caller owns lm_asrc_size(channels) bytes, aligned for double/pointers.
 * Size is zero for invalid channel counts. Init requires finite positive rates
 * and runs off the audio thread (it builds the coefficient table). Instances
 * are independent and each instance is used by only one thread at a time. */
size_t lm_asrc_size(int channels);
void lm_asrc_init(lm_asrc* s, int channels, double fs_in, double fs_out);
/* Clears history/phase; preserves the format and current correction. */
void lm_asrc_reset(lm_asrc* s);
/* Input consumed per output = fs_in/fs_out * (1 + ppm/1e6).
 * Non-finite corrections and non-positive resulting ratios are ignored. */
void lm_asrc_set_correction_ppm(lm_asrc* s, double ppm);
/* Interleaved PCM, disjoint input/output. Produces out_frames unless input runs
 * out. Advance the input by *in_used frames, retaining any unconsumed frames.
 * Zero input can still produce an already buffered upsampling phase. Startup
 * history is silence; callers can feed silence to drain the filter at EOF. */
int lm_asrc_process(lm_asrc* s, const float* in, int in_frames, int* in_used, float* out, int out_frames);
double lm_asrc_latency_input_frames(const lm_asrc* s);

typedef struct lm_drift {
    double target, kp, ki, integ, lp, lp_alpha, ppm, max_ppm, slew_per_s;
    int primed;
    double elapsed, acquire_s, acquire_slew_per_s;   /* faster slew right after init: the clock is found in seconds */
} lm_drift;

/* Fill and target are input frames, measured at the same point in each callback.
 * Positive fill error increases consumption. One-second low pass, PI with
 * anti-windup at both the +/-1000 ppm clamp and the slew limit: 200 ppm/second
 * for the first 10 seconds after init (acquisition), 20 ppm/second after that
 * (0.35 and 0.035 cent per second - inaudible either way). dt_s is the elapsed
 * simulated/device time, not an internal wall-clock read. At a clock mismatch
 * equal to the clamp a cold start keeps the fill offset gathered while the
 * correction ramps (about 2.5 ms at 1000 ppm); real devices sit far inside it. */
void lm_drift_init(lm_drift* d, double target_frames, double update_hz);
double lm_drift_update(lm_drift* d, double fill_frames, double dt_s);

#ifdef __cplusplus
}
#endif
#endif
