/* SPDX-License-Identifier: MIT
 * A fake LiveMix for the OBS plugin tests that behaves like the real one: the ring is written from an MMCSS "Pro Audio"
 * thread paced by a high-resolution waitable timer (LiveMix writes from its audio device callback), not from a Python
 * loop that a busy CPU can starve (27 Sep 2026: two Python writers got the CPU every ~2.5 s for 17 minutes).
 *
 *   livemix-fake-writer --ring NAME --seconds S --ppm P [--rate 48000] [--block 256] [--freq 997] [--level 0.25]
 *                       [--restart-at S] [--stall-at S:D] [--rate-change-at S:R]
 *
 * Every 5 s it prints: frames written vs due at the nominal clock, the largest gap between two wake-ups, and how many
 * blocks were written late (more than two blocks behind) - so a starved writer shows up in its own log.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lm_obs_protocol.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static double arg_double(int argc, char **argv, const char *name, double fallback)
{
	for (int i = 1; i + 1 < argc; ++i)
		if (strcmp(argv[i], name) == 0)
			return atof(argv[i + 1]);
	return fallback;
}

static const char *arg_text(int argc, char **argv, const char *name)
{
	for (int i = 1; i + 1 < argc; ++i)
		if (strcmp(argv[i], name) == 0)
			return argv[i + 1];
	return NULL;
}

static int64_t qpc(void)
{
	LARGE_INTEGER v;
	QueryPerformanceCounter(&v);
	return v.QuadPart;
}

static void owner_reset(lm_obs_ring_header *h, int64_t rate)
{
	lm_obs_store_release(&h->send_enabled, 0);
	int64_t next = lm_obs_load_acquire(&h->epoch) + 1;
	lm_obs_store_release(&h->write_frames, 0);
	lm_obs_store_release((volatile int64_t *)h->reserved, 0);
	lm_obs_store_release(&h->sample_rate, rate);
	lm_obs_store_release(&h->epoch, next);
	lm_obs_store_release(&h->send_enabled, 1);
}

int main(int argc, char **argv)
{
	const char *ring_arg = arg_text(argc, argv, "--ring");
	wchar_t ring[128];
	MultiByteToWideChar(CP_UTF8, 0, ring_arg ? ring_arg : "Local\\LiveMix.ObsAudio.test", -1, ring, 128);
	double seconds = arg_double(argc, argv, "--seconds", 60), ppm = arg_double(argc, argv, "--ppm", 0);
	int rate = (int)arg_double(argc, argv, "--rate", 48000), block = (int)arg_double(argc, argv, "--block", 256);
	double freq = arg_double(argc, argv, "--freq", 997), level = arg_double(argc, argv, "--level", 0.25);
	double restart_at = arg_double(argc, argv, "--restart-at", -1);
	double stall_at = -1, stall_len = 0, change_at = -1, change_rate = 0;
	const char *s = arg_text(argc, argv, "--stall-at");
	if (s)
		sscanf(s, "%lf:%lf", &stall_at, &stall_len);
	s = arg_text(argc, argv, "--rate-change-at");
	if (s)
		sscanf(s, "%lf:%lf", &change_at, &change_rate);
	if (block < 16 || block > 8192 || rate < 8000 || rate > 384000)
		return 2;

	const DWORD bytes = (DWORD)(sizeof(lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * 2u * sizeof(float));
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, ring);
	if (!mapping)
		return 3;
	lm_obs_ring_header *h = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
	if (!h)
		return 4;
	float *pcm = (float *)((unsigned char *)h + sizeof(*h));
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	h->magic = LM_OBS_MAGIC;
	h->protocol_major = LM_OBS_PROTOCOL_MAJOR;
	h->protocol_minor = 0;
	h->header_bytes = sizeof(*h);
	h->mapping_bytes = bytes;
	h->data_offset = sizeof(*h);
	h->channels = LM_OBS_CHANNELS;
	h->capacity_frames = LM_OBS_CAPACITY_FRAMES;
	lm_obs_store_release(&h->qpc_frequency, f.QuadPart);
	lm_obs_store_release(&h->writer_pid, GetCurrentProcessId());
	owner_reset(h, rate);

	DWORD task = 0;
	HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
	if (mmcss)
		AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	printf("native writer: %ls rate %d block %d ppm %+.1f mmcss=%s hr-timer=%s\n", ring, rate, block, ppm,
	       mmcss ? "yes" : "no", timer ? "yes" : "no");
	fflush(stdout);

	float *buf = malloc((size_t)block * 2 * sizeof(float));
	float *left = malloc((size_t)block * sizeof(float)), *right = malloc((size_t)block * sizeof(float));
	double phase = 0.0;
	const double freq_q = (double)f.QuadPart;
	int64_t t0 = qpc(), epoch_t0 = t0, written = 0, last_wake = t0, next_report = t0 + 5 * f.QuadPart;
	double max_gap_ms = 0.0;
	long late_blocks = 0;
	int restarted = 0, stalled = 0, changed = 0;
	for (;;) {
		int64_t now = qpc();
		double t = (now - t0) / freq_q;
		if (t >= seconds)
			break;
		double gap = (now - last_wake) * 1000.0 / freq_q;
		if (gap > max_gap_ms)
			max_gap_ms = gap;
		last_wake = now;
		if (stall_at >= 0 && !stalled && t >= stall_at) {
			stalled = 1;
			Sleep((DWORD)(stall_len * 1000));
			epoch_t0 += (int64_t)(stall_len * freq_q);
			last_wake = qpc();
			continue;
		}
		if (restart_at >= 0 && !restarted && t >= restart_at) {
			restarted = 1;
			owner_reset(h, rate);
			epoch_t0 = now;
			written = 0;
		}
		if (change_at >= 0 && !changed && t >= change_at) {
			changed = 1;
			rate = (int)change_rate;
			owner_reset(h, rate);
			epoch_t0 = now;
			written = 0;
		}
		int64_t due = (int64_t)((now - epoch_t0) / freq_q * rate * (1.0 + ppm * 1e-6));
		if (due - written >= 2 * block)
			++late_blocks;
		while (due - written >= block) {
			double step = 2.0 * 3.14159265358979323846 * freq / rate;
			for (int i = 0; i < block; ++i) {
				float v = (float)(level * sin(phase));
				left[i] = right[i] = v;
				phase += step;
				if (phase > 6.283185307179586)
					phase -= 6.283185307179586;
			}
			lm_obs_write(h, pcm, left, right, (uint32_t)block);
			lm_obs_store_release(&h->heartbeat_qpc, qpc());
			written += block;
		}
		if (now >= next_report) {
			printf("t=%.0fs written=%lld due=%lld max_wake_gap=%.2fms late_blocks=%ld\n", t, (long long)written,
			       (long long)due, max_gap_ms, late_blocks);
			fflush(stdout);
			max_gap_ms = 0.0;
			next_report = now + 5 * f.QuadPart;
		}
		/* sleep until the next block is due at the writer's clock (a quarter block early to absorb wake-up jitter) */
		double wait_s = ((written + block) / (rate * (1.0 + ppm * 1e-6))) - (now - epoch_t0) / freq_q - 0.25 * block / rate;
		if (wait_s > 0 && timer) {
			LARGE_INTEGER due_time;
			due_time.QuadPart = -(LONGLONG)(wait_s * 1e7);
			SetWaitableTimer(timer, &due_time, 0, NULL, NULL, FALSE);
			WaitForSingleObject(timer, 1000);
		} else if (wait_s > 0) {
			Sleep(1);
		}
	}
	lm_obs_store_release(&h->send_enabled, 0);
	printf("done: frames %lld\n", (long long)written);
	if (mmcss)
		AvRevertMmThreadCharacteristics(mmcss);
	return 0;
}
