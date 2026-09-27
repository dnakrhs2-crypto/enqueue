/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 LiveMix contributors
 * Licensed under the MIT license; see the full notice in lm_asrc.h.
 */
#include "lm_asrc.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define LM_TAPS 32
#define LM_PHASES 512
#define LM_DELAY 16.0
#define LM_PI 3.14159265358979323846264338327950288

struct lm_asrc {
    int channels, newest;
    double nominal_ratio, ratio, phase, pending;
    float kernel[LM_PHASES + 1][LM_TAPS];
    /* History follows the struct in the caller's storage (32 interleaved frames). */
};

static double lm_clamp(double value, double low, double high)
{
    return value < low ? low : (value > high ? high : value);
}

static double lm_bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 64; ++k) {
        term *= (x * x * 0.25) / ((double) k * k);
        sum += term;
        if (term < sum * 1.0e-16)
            break;
    }
    return sum;
}

size_t lm_asrc_size(int channels)
{
    if (channels <= 0 || (size_t) channels > (SIZE_MAX - sizeof(lm_asrc)) / (LM_TAPS * sizeof(float)))
        return 0;
    return sizeof(lm_asrc) + (size_t) channels * LM_TAPS * sizeof(float);
}

void lm_asrc_reset(lm_asrc* s)
{
    s->newest = LM_TAPS - 1;
    s->phase = 0.0;
    s->pending = 1.0;
    memset(s + 1, 0, (size_t) s->channels * LM_TAPS * sizeof(float));
}

void lm_asrc_init(lm_asrc* s, int channels, double fs_in, double fs_out)
{
    const double cutoff = 0.45 * fmin(1.0, fs_out / fs_in);
    const double window_scale = 1.0 / lm_bessel_i0(8.6);
    int phase, tap;
    s->channels = channels;
    s->nominal_ratio = fs_in / fs_out;
    s->ratio = s->nominal_ratio;

    for (phase = 0; phase <= LM_PHASES; ++phase) {
        double weights[LM_TAPS], total = 0.0;
        for (tap = 0; tap < LM_TAPS; ++tap) {
            const double distance = tap + (double) phase / LM_PHASES - LM_DELAY;
            const double position = distance / (LM_TAPS * 0.5);
            const double window = lm_bessel_i0(8.6 * sqrt(fmax(0.0, 1.0 - position * position))) * window_scale;
            const double sinc = fabs(distance) < 1.0e-12 ? 2.0 * cutoff
                                : sin(2.0 * LM_PI * cutoff * distance) / (LM_PI * distance);
            weights[tap] = sinc * window;
            total += weights[tap];
        }
        for (tap = 0; tap < LM_TAPS; ++tap)
            s->kernel[phase][tap] = (float) (weights[tap] / total);
    }
    lm_asrc_reset(s);
}

void lm_asrc_set_correction_ppm(lm_asrc* s, double ppm)
{
    const double ratio = s->nominal_ratio * (1.0 + ppm * 1.0e-6);
    if (isfinite(ratio) && ratio > 0.0)
        s->ratio = ratio;
}

int lm_asrc_process(lm_asrc* s, const float* in, int in_frames, int* in_used, float* out, int out_frames)
{
    float* history = (float*) (s + 1);
    int used = 0, made = 0;
    *in_used = 0;

    while (made < out_frames) {
        int channel, tap, phase;
        double table_position, blend;

        while (s->pending >= 1.0) {
            if (used >= in_frames) {
                *in_used = used;
                return made;
            }
            s->newest = (s->newest + 1) & (LM_TAPS - 1);
            memcpy(history + (size_t) s->newest * s->channels, in + (size_t) used * s->channels,
                   (size_t) s->channels * sizeof(float));
            ++used;
            s->pending -= 1.0;
        }

        table_position = s->phase * LM_PHASES;
        phase = (int) table_position;
        blend = table_position - phase;
        for (channel = 0; channel < s->channels; ++channel) {
            double sample = 0.0;
            for (tap = 0; tap < LM_TAPS; ++tap) {
                const double a = s->kernel[phase][tap];
                const double weight = a + blend * (s->kernel[phase + 1][tap] - a);
                const int index = (s->newest - tap) & (LM_TAPS - 1);
                sample += history[(size_t) index * s->channels + channel] * weight;
            }
            out[(size_t) made * s->channels + channel] = (float) sample;
        }
        ++made;
        s->phase += s->ratio;
        s->pending = floor(s->phase);
        s->phase -= s->pending;
    }
    *in_used = used;
    return made;
}

double lm_asrc_latency_input_frames(const lm_asrc* s)
{
    (void) s;
    return LM_DELAY;
}

void lm_drift_init(lm_drift* d, double target_frames, double update_hz)
{
    d->target = isfinite(target_frames) && target_frames > 0.0 ? target_frames : 1.0;
    /* Scale gains with the target so the same loop works at different rates. */
    d->kp = 4320.0 / d->target;
    d->ki = 144.0 / d->target;
    d->integ = 0.0;
    d->lp = 0.0;
    d->lp_alpha = isfinite(update_hz) && update_hz > 0.0 ? -expm1(-1.0 / update_hz) : 0.0;
    d->ppm = 0.0;
    d->max_ppm = 1000.0;
    d->slew_per_s = 20.0;
    d->primed = 0;
    d->elapsed = 0.0;
    d->acquire_s = 10.0;
    d->acquire_slew_per_s = 200.0;
}

double lm_drift_update(lm_drift* d, double fill_frames, double dt_s)
{
    double error, wanted, limited, applied, integral_step, slew;
    if (!isfinite(fill_frames) || !isfinite(dt_s) || dt_s <= 0.0)
        return d->ppm;

    error = fill_frames - d->target;
    d->lp_alpha = -expm1(-dt_s); /* one second, including variable callback periods */
    if (!d->primed) {
        d->lp = error;
        d->primed = 1;
    } else {
        d->lp += d->lp_alpha * (error - d->lp);
    }

    /* the tolerance keeps a sum of many small time steps (0.01 s x 1000 = 9.99999...) from buying one more fast step */
    slew = d->elapsed < d->acquire_s - 1.0e-9 ? d->acquire_slew_per_s : d->slew_per_s;
    d->elapsed += dt_s;
    wanted = d->kp * d->lp + d->integ;
    limited = lm_clamp(wanted, -d->max_ppm, d->max_ppm);
    applied = lm_clamp(limited, d->ppm - slew * dt_s, d->ppm + slew * dt_s);
    integral_step = d->ki * d->lp * dt_s;
    /* Integrate only when it can move the requested correction towards the
     * applied correction, or when neither actuator limit is active. */
    if ((wanted - applied) * integral_step <= 0.0)
        d->integ = lm_clamp(d->integ + integral_step, -d->max_ppm, d->max_ppm);
    d->ppm = applied;
    return applied;
}
