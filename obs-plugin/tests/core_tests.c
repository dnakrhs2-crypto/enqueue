/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "receiver.h"
#include "lm_obs_protocol.h"

#include <math.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define TEST_RATE 48000
#define TEST_FRAMES 480
#define TEST_QPC INT64_C(1000000000)
#define RING_BYTES (sizeof(lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * 2u * sizeof(float))

enum diagnostic_event { CONNECT, DISCONNECT, EPOCH, RATE, UNDERRUN, OVERRUN, RESYNC, TARGET, EVENT_COUNT };

typedef struct fixture {
	wchar_t ring_name[128], readers_name[144];
	volatile int64_t now;
	lm_connection *connection;
	lm_receiver *receiver;
	HANDLE mapping;
	lm_obs_ring_header *header;
	float *pcm;
	double rate, fraction, ppm;
	float value, left[TEST_FRAMES], right[TEST_FRAMES];
	bool threaded, position_signal;
	bool capture_logs, in_audio, audio_logged;
	DWORD audio_thread;
	int events[2][EVENT_COUNT], summaries[2], log_calls, suppressed, repeats, previous_sum;
	const char *previous_format;
	bool repeated_format;
} fixture;

static unsigned name_serial;
static int tests_run, tests_failed;
static const char *test_filter;

#define CHECK(condition) do { \
	if (!(condition)) { \
		printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		ok = false; \
		goto done; \
	} \
} while (0)

static int64_t test_now(void *opaque)
{
	return lm_obs_load_acquire(&((fixture *)opaque)->now);
}

static void capture_log(void *opaque, enum lm_log_level level, const char *format, const char *message)
{
	fixture *f = opaque;
	(void)level;
	if (!f->capture_logs)
		return;
	if (GetCurrentThreadId() == f->audio_thread && f->in_audio)
		f->audio_logged = true;
	char rendered[1024];
	snprintf(rendered, sizeof(rendered), format, message);
	int sum = 0;
	for (const unsigned char *p = (const unsigned char *)rendered; *p; ++p)
		sum += *p;
	/* Reproduce the relevant OBS repeated-line predicate, including its
	 * format pointer check. Changing just the numbers/interval is insufficient. */
	if (format == f->previous_format && abs(sum - f->previous_sum) < 765) {
		if (f->repeats++ >= 30)
			++f->suppressed;
	} else {
		f->repeats = 0;
		f->previous_sum = sum;
	}
	if (format == f->previous_format)
		f->repeated_format = true;
	f->previous_format = format;
	++f->log_calls;
	int kind = strstr(message, "kind=source ") ? 0 : (strstr(message, "kind=filter ") ? 1 : -1);
	if (kind < 0)
		return;
	if (strstr(message, " summary "))
		++f->summaries[kind];
	const char *events[EVENT_COUNT] = {"event=connect ", "event=disconnect ", "event=epoch ", "event=rate ",
		"event=underrun ", "event=overrun ", "event=resync ", "event=target "};
	for (int i = 0; i < EVENT_COUNT; ++i)
		if (strstr(message, events[i]))
			++f->events[kind][i];
}

static void checked_pull(fixture *f)
{
	f->in_audio = true;
	lm_receiver_pull(f->receiver, f->left, f->right, TEST_FRAMES, TEST_RATE);
	f->in_audio = false;
}

static bool fixture_open(fixture *f)
{
	memset(f, 0, sizeof(*f));
	f->now = 20 * TEST_QPC;
	f->rate = TEST_RATE;
	f->value = 0.5f;
	f->audio_thread = GetCurrentThreadId();
	swprintf(f->ring_name, 128, L"Local\\LiveMix.ObsCoreTests.%lu.%u", GetCurrentProcessId(), ++name_serial);
	swprintf(f->readers_name, 144, L"%ls.Readers", f->ring_name);
	lm_connection_config config = {0};
	config.ring_name = f->ring_name;
	config.readers_name = f->readers_name;
	config.now = test_now;
	config.clock_context = f;
	config.qpc_frequency = TEST_QPC;
	config.log = capture_log;
	config.log_context = f;
	f->connection = lm_connection_create(&config);
	if (!f->connection)
		return false;
	f->receiver = lm_receiver_create(f->connection, LM_RECEIVER_SOURCE, TEST_RATE);
	return f->receiver != NULL;
}

static bool writer_open(fixture *f, DWORD bytes)
{
	f->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, bytes, f->ring_name);
	if (!f->mapping)
		return false;
	f->header = MapViewOfFile(f->mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
	if (!f->header)
		return false;
	memset(f->header, 0, bytes);
	lm_obs_ring_header *h = f->header;
	h->protocol_major = LM_OBS_PROTOCOL_MAJOR;
	h->header_bytes = sizeof(*h);
	h->mapping_bytes = (uint32_t)RING_BYTES;
	h->data_offset = sizeof(*h);
	h->channels = LM_OBS_CHANNELS;
	h->capacity_frames = LM_OBS_CAPACITY_FRAMES;
	lm_obs_store_release(&h->sample_rate, (int64_t)f->rate);
	lm_obs_store_release(&h->qpc_frequency, TEST_QPC);
	lm_obs_store_release(&h->writer_pid, GetCurrentProcessId());
	lm_obs_store_release(&h->epoch, 1);
	lm_obs_store_release(&h->send_enabled, 1);
	h->magic = LM_OBS_MAGIC;
	f->pcm = (float *)((unsigned char *)h + sizeof(*h));
	return true;
}

static void fixture_close(fixture *f)
{
	lm_receiver_destroy(f->receiver);
	lm_connection_destroy(f->connection);
	if (f->header)
		UnmapViewOfFile(f->header);
	if (f->mapping)
		CloseHandle(f->mapping);
}

static void writer_restart(fixture *f, double rate, float value)
{
	lm_obs_ring_header *h = f->header;
	lm_obs_store_release(&h->send_enabled, 0);
	lm_obs_store_release(&h->write_frames, 0);
	lm_obs_store_release((volatile int64_t *)h->reserved, 0);
	lm_obs_store_release(&h->sample_rate, (int64_t)rate);
	lm_obs_store_release(&h->epoch, lm_obs_load_acquire(&h->epoch) + 1);
	f->rate = rate;
	f->fraction = 0.0;
	f->value = value;
	lm_obs_store_release(&h->send_enabled, 1);
}

/* Both clocks are driven here, with fractional input frames carried between ticks.
 * Every receiver pull and every ASRC sample is real; no resampler is stubbed. */
static void tick(fixture *f, bool write_audio, bool read_audio)
{
	lm_obs_store_release(&f->now, lm_obs_load_acquire(&f->now) + TEST_QPC / 100);
	if (write_audio) {
		float samples[1024];
		f->fraction += f->rate * (1.0 + f->ppm * 1.0e-6) / 100.0;
		uint32_t frames = (uint32_t)f->fraction;
		f->fraction -= frames;
		for (size_t i = 0; i < 1024; ++i)
			samples[i] = f->value;
		while (frames) {
			uint32_t count = frames < 1024 ? frames : 1024;
			if (f->position_signal) {
				int64_t written = lm_obs_load_acquire(&f->header->write_frames);
				for (uint32_t i = 0; i < count; ++i)
					samples[i] = (float)((written + i) / 10000000.0);
			}
			lm_obs_write(f->header, f->pcm, samples, samples, count);
			frames -= count;
		}
		lm_obs_store_release(&f->header->heartbeat_qpc, f->now);
	}
	if (!f->threaded)
		lm_connection_poll(f->connection);
	if (read_audio)
		checked_pull(f);
}

static bool silent(const fixture *f)
{
	for (int i = 0; i < TEST_FRAMES; ++i)
		if (f->left[i] != 0.0f || f->right[i] != 0.0f)
			return false;
	return true;
}

static bool settled(const fixture *f, float value)
{
	for (int i = 0; i < TEST_FRAMES; ++i)
		if (!isfinite(f->left[i]) || fabsf(f->left[i] - value) > 0.002f ||
		    fabsf(f->right[i] - value) > 0.002f)
			return false;
	return true;
}

static bool warm_up(fixture *f)
{
	for (int i = 0; i < 300; ++i)
		tick(f, true, true);
	return settled(f, f->value);
}

static bool test_source_layout(void)
{
	bool ok = true;
	const float original_left[] = {1.0f, -1.0f, 0.75f, 0.5f, -0.25f};
	const float original_right[] = {1.0f, -1.0f, -0.75f, 0.0f, 0.75f};
	const size_t layouts[] = {1, 2, 6};
	for (size_t layout = 0; layout < 3; ++layout) {
		float left[5], right[5];
		memcpy(left, original_left, sizeof(left));
		memcpy(right, original_right, sizeof(right));
		size_t submitted = lm_mix_stereo_for_layout(left, right, 5, layouts[layout]);
		CHECK(submitted == (layouts[layout] == 1 ? 1 : 2));
		for (int i = 0; i < 5; ++i) {
			float expected = layouts[layout] == 1 ? (original_left[i] + original_right[i]) * 0.5f
							   : original_left[i];
			CHECK(left[i] == expected && right[i] == original_right[i]);
		}
		CHECK(left[0] == 1.0f && left[1] == -1.0f);
	}
done:
	return ok;
}

static bool test_late_connect(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	for (int i = 0; i < 120; ++i) {
		tick(&f, false, true);
		CHECK(silent(&f));
	}
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	lm_receiver_stats stats;
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.connected && stats.epoch == 1);
	CHECK(fabs(stats.fill_ms - 30.0) < 5.0);
done:
	fixture_close(&f);
	return ok;
}

static bool test_restart_fade(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	float previous = f.left[TEST_FRAMES - 1];
	/* Closing the writer's handles leaves the receiver's read-only view alive. */
	UnmapViewOfFile(f.header);
	CloseHandle(f.mapping);
	f.header = NULL;
	f.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, f.ring_name);
	CHECK(f.mapping != NULL);
	f.header = MapViewOfFile(f.mapping, FILE_MAP_ALL_ACCESS, 0, 0, RING_BYTES);
	CHECK(f.header != NULL);
	f.pcm = (float *)((unsigned char *)f.header + sizeof(*f.header));
	writer_restart(&f, TEST_RATE, -0.5f);
	for (int i = 0; i < 300; ++i) {
		tick(&f, true, true);
		for (int j = 0; j < TEST_FRAMES; ++j) {
			CHECK(isfinite(f.left[j]) && fabsf(f.left[j]) <= 0.51f);
			CHECK(fabsf(f.left[j] - previous) < 0.02f);
			previous = f.left[j];
		}
	}
	CHECK(settled(&f, -0.5f));
	lm_receiver_stats stats;
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.epoch == 2 && stats.resyncs >= 2);
done:
	fixture_close(&f);
	return ok;
}

static bool test_rate_change(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	writer_restart(&f, 44100.0, 0.25f);
	for (int i = 0; i < 1000; ++i)
		tick(&f, true, true);
	CHECK(settled(&f, 0.25f));
	lm_receiver_stats stats;
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.epoch == 2 && fabs(stats.fill_ms - 30.0) < 5.0);
	CHECK(stats.underruns == 0 && stats.overruns == 0);
	/* An OBS rate change uses the same asynchronous preparation path. */
	for (int i = 0; i < 500; ++i) {
		tick(&f, true, false);
		lm_receiver_pull(f.receiver, f.left, f.right, 441, 44100.0);
	}
	CHECK(fabsf(f.left[440] - 0.25f) < 0.002f);
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(fabs(stats.fill_ms - 30.0) < 5.0);
done:
	fixture_close(&f);
	return ok;
}

static bool test_stall_and_recovery(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = 100.0;
	for (int i = 0; i < 3000; ++i)
		tick(&f, true, true);
	lm_receiver_stats saved, before, after;
	lm_receiver_get_stats(f.receiver, &saved);
	CHECK(settled(&f, f.value) && saved.target_ms == 30.0);
	float previous = f.left[TEST_FRAMES - 1];
	for (int i = 0; i < 300; ++i) {
		tick(&f, false, true);
		lm_receiver_get_stats(f.receiver, &before);
		CHECK(before.target_ms <= saved.target_ms + 10.0);
		CHECK(fabs(before.ppm) <= 300.0);
		if (!before.connected)
			CHECK(before.target_ms == saved.target_ms);
		for (int j = 0; j < TEST_FRAMES; ++j) {
			CHECK(fabsf(f.left[j] - previous) < 0.02f);
			previous = f.left[j];
		}
		if (i >= 10)
			CHECK(silent(&f));
	}
	lm_receiver_get_stats(f.receiver, &before);
	CHECK(!before.connected && before.underruns == 1);
	f.value = -0.25f;
	for (int i = 0; i < 1000; ++i) {
		tick(&f, true, true);
		lm_receiver_get_stats(f.receiver, &after);
		CHECK(after.epoch == saved.epoch && after.target_ms == saved.target_ms);
		CHECK(fabs(after.ppm - f.ppm) <= 50.0);
		if (i >= 499) {
			CHECK(fabs(after.fill_ms - after.target_ms) <= 3.0);
			CHECK(settled(&f, f.value) && after.underruns == before.underruns);
		}
	}
	CHECK(settled(&f, -0.25f));
	lm_receiver_get_stats(f.receiver, &after);
	CHECK(after.connected && after.resyncs > before.resyncs);
	printf("  3 s stall: target %.3f -> %.3f ms, recovered fill=%.3f ms ppm=%.3f\n",
	       saved.target_ms, after.target_ms, after.fill_ms, after.ppm);
done:
	fixture_close(&f);
	return ok;
}

static bool test_clock_retention(double ppm)
{
	fixture f;
	bool ok = true;
	lm_receiver_stats before, previous, stats;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = ppm;
	for (int i = 0; i < 9000; ++i)
		tick(&f, true, true);
	lm_receiver_get_stats(f.receiver, &before);
	CHECK(fabs(before.ppm - ppm) <= 10.0);
	previous = before;
	/* The learned clock survives an underrun, stale heartbeat and re-prefill.
	 * Slow slew throughout recovery pins the acquisition clock as well. */
	for (int i = 0; i < 2300; ++i) {
		tick(&f, i >= 300, true);
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(stats.epoch == before.epoch && fabs(stats.ppm - ppm) <= 50.0);
		CHECK(fabs(stats.ppm - previous.ppm) <= 0.202);
		if (i >= 400)
			CHECK(settled(&f, f.value));
		previous = stats;
	}
	CHECK(stats.underruns == before.underruns + 1 && stats.overruns == before.overruns);
	/* A new epoch really does discard the old clock. */
	writer_restart(&f, TEST_RATE, f.value);
	for (int i = 0; i < 5; ++i)
		tick(&f, true, true);
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.epoch == before.epoch + 1 && fabs(stats.ppm) < 10.0);
done:
	fixture_close(&f);
	return ok;
}

static bool test_clock_positive(void) { return test_clock_retention(300.0); }
static bool test_clock_negative(void) { return test_clock_retention(-300.0); }

static bool test_recurring_stalls(void)
{
	fixture f;
	bool ok = true;
	lm_receiver_stats stats = {0};
	double max_target = 0.0, max_ppm = 0.0;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = 100.0;
	for (int i = 0; i < 30000; ++i) {
		bool stalled = i >= 2000 && i % 2000 < 100;
		tick(&f, !stalled, true);
		lm_receiver_get_stats(f.receiver, &stats);
		max_target = fmax(max_target, stats.target_ms);
		max_ppm = fmax(max_ppm, fabs(stats.ppm));
		CHECK(stats.target_ms <= 40.0 && fabs(stats.ppm) <= 300.0);
		CHECK(stats.overruns == 0);
		if (i >= 2000 && i % 2000 >= 600) {
			CHECK(stats.target_ms == 30.0 && fabs(stats.fill_ms - stats.target_ms) <= 3.0);
			CHECK(settled(&f, f.value));
		}
	}
	CHECK(stats.underruns == 14 && stats.epoch == 1);
done:
	printf("  1 s stalls every 20 s for 5 min: max target=%.3f ms max |ppm|=%.3f, under=%llu\n",
	       max_target, max_ppm, (unsigned long long)stats.underruns);
	fixture_close(&f);
	return ok;
}

static bool test_phase_locked_blocks(void)
{
	fixture f;
	bool ok = true;
	float samples[TEST_FRAMES];
	lm_receiver_stats previous = {0}, stats = {0};
	double min_ppm = 1000.0, max_ppm = -1000.0, max_step = 0.0;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	for (int i = 0; i < TEST_FRAMES; ++i)
		samples[i] = f.value;
	int64_t origin = f.now, block = 1;
	for (int i = 1; i <= 360000; ++i) {
		int64_t elapsed = i * (TEST_QPC / 100);
		int64_t write_time;
		/* 480 frames at 48000 * (1 + 20e-6) Hz, independent of the reader.
		 * The phase crosses a complete 10 ms block every 500 seconds. */
		while ((write_time = block * INT64_C(500000000000) / 50001) <= elapsed) {
			lm_obs_write(f.header, f.pcm, samples, samples, TEST_FRAMES);
			lm_obs_store_release(&f.header->heartbeat_qpc, origin + write_time);
			++block;
		}
		lm_obs_store_release(&f.now, origin + elapsed);
		lm_connection_poll(f.connection);
		lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(stats.underruns == 0 && stats.overruns == 0 && stats.target_ms == 30.0);
		if (i >= 6000) {
			min_ppm = fmin(min_ppm, stats.ppm);
			max_ppm = fmax(max_ppm, stats.ppm);
			max_step = fmax(max_step, fabs(stats.fill_ms - previous.fill_ms));
			CHECK(fabs(stats.ppm - 20.0) <= 30.0);
			CHECK(fabs(stats.fill_ms - previous.fill_ms) <= 0.05);
			CHECK(fabs(stats.fill_ms - stats.target_ms) <= 3.0 && settled(&f, f.value));
		}
		previous = stats;
	}
done:
	printf("  +20 ppm, 480-frame blocks for 1 h: correction %.3f..%.3f ppm, max fill step=%.3f ms\n",
	       min_ppm, max_ppm, max_step);
	fixture_close(&f);
	return ok;
}

static bool test_headroom_grace(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	/* The connection first sees the mapping at one second: playback has
	 * just started, although ample audio is already in the ring. */
	for (int i = 0; i < 100; ++i)
		tick(&f, true, true);
	for (int pass = 0; pass < 2; ++pass) {
		lm_receiver_stats before, after;
		lm_receiver_get_stats(f.receiver, &before);
		CHECK(before.connected && before.target_ms == (pass ? 40.0 : 30.0));
		/* Leave 12 ms for a 10 ms pull: safe PCM, but below the 3 ms
		 * headroom margin. A live heartbeat makes the fill unambiguous. */
		int missing = (int)llround((before.target_ms - 12.0) * TEST_RATE / 1000.0);
		int extra = missing - TEST_FRAMES;
		while (extra > 0) {
			int count = extra < TEST_FRAMES ? extra : TEST_FRAMES;
			f.now += count * TEST_QPC / TEST_RATE;
			lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
			lm_receiver_pull(f.receiver, f.left, f.right, count, TEST_RATE);
			extra -= count;
		}
		f.now += TEST_QPC / 100;
		lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
		lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
		lm_receiver_get_stats(f.receiver, &after);
		CHECK(after.target_ms == before.target_ms && after.underruns == before.underruns);
		CHECK(settled(&f, f.value));
		float samples[TEST_FRAMES];
		for (int i = 0; i < TEST_FRAMES; ++i)
			samples[i] = f.value;
		while (missing > 0) {
			int count = missing < TEST_FRAMES ? missing : TEST_FRAMES;
			lm_obs_write(f.header, f.pcm, samples, samples, (uint32_t)count);
			missing -= count;
		}
		if (pass == 0) {
			for (int i = 0; i < 8; ++i) {
				lm_obs_store_release(&f.header->heartbeat_qpc, f.now + TEST_QPC / 100);
				tick(&f, false, true);
			}
			for (int i = 0; i < 10; ++i)
				tick(&f, true, true);
		}
	}
done:
	fixture_close(&f);
	return ok;
}

static bool test_live_underrun_wait(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	lm_receiver_stats stats;
	/* This writer remains alive but provides no PCM. Prefill cannot grow
	 * the target, and the +10 ms becomes permanent after one live second. */
	for (int i = 0; i < 120; ++i) {
		lm_obs_store_release(&f.header->heartbeat_qpc, f.now + TEST_QPC / 100);
		tick(&f, false, true);
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(stats.connected && stats.target_ms <= 40.0);
		if (stats.underruns)
			CHECK(stats.underruns == 1 && stats.target_ms == 40.0);
	}
	CHECK(stats.underruns == 1);
	/* A later, unrelated stop must not undo that committed headroom. */
	for (int i = 0; i < 300; ++i) {
		tick(&f, false, true);
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(stats.target_ms == 40.0);
	}
	CHECK(!stats.connected);
	CHECK(warm_up(&f));
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.connected && stats.target_ms == 40.0 && stats.underruns == 1);
done:
	fixture_close(&f);
	return ok;
}

static bool test_overrun(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	f.value = -0.5f;
	for (int i = 0; i < 100; ++i)
		tick(&f, true, false);
	CHECK(warm_up(&f));
	lm_receiver_stats stats;
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.overruns == 1 && stats.underruns == 0);
	CHECK(stats.resyncs >= 2 && fabs(stats.fill_ms - 30.0) < 5.0);
done:
	fixture_close(&f);
	return ok;
}

static bool test_reader_resync(int pause_ticks, bool explicit_resync)
{
	fixture f;
	bool ok = true;
	lm_receiver_stats before, previous, stats;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = 300.0;
	f.position_signal = true;
	for (int i = 0; i < 9000; ++i)
		tick(&f, true, true);
	lm_receiver_get_stats(f.receiver, &before);
	CHECK(fabs(before.ppm - f.ppm) <= 10.0);
	float last = f.left[TEST_FRAMES - 1];
	for (int i = 0; i < pause_ticks; ++i)
		tick(&f, true, false);
	if (explicit_resync)
		lm_receiver_resync(f.receiver);
	previous = before;
	for (int pull = 0; pull < 20; ++pull) {
		if (pull == 0)
			lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
		else
			tick(&f, true, true);
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(stats.resyncs == before.resyncs + 1);
		CHECK(stats.overruns == before.overruns + (explicit_resync ? 0 : 1));
		CHECK(stats.underruns == before.underruns && fabs(stats.fill_ms - stats.target_ms) <= 5.0);
		CHECK(fabs(stats.ppm - f.ppm) <= 50.0 && fabs(stats.ppm - previous.ppm) <= 0.202);
		for (int i = 0; i < TEST_FRAMES; ++i) {
			CHECK(isfinite(f.left[i]) && fabsf(f.left[i] - last) < 0.02f);
			last = f.left[i];
		}
		if (pull > 0) {
			/* PCM encodes its absolute ring position. Once the <=10 ms fade
			 * ends, the first delivered sample must be target +/-5 ms old. */
			double position = f.left[0] * 10000000.0;
			double lag_ms = (lm_obs_load_acquire(&f.header->write_frames) - position) * 1000.0 / f.rate;
			CHECK(fabs(lag_ms - stats.target_ms) <= 5.0);
		}
		previous = stats;
	}
done:
	fixture_close(&f);
	return ok;
}

static bool test_late_worker_resync(void) { return test_reader_resync(20, true); }
static bool test_overrun_clock(void) { return test_reader_resync(100, false); }

static bool test_clock_rate_reset(void)
{
	fixture f;
	bool ok = true;
	lm_receiver_stats stats;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = 300.0;
	for (int pass = 0; pass < 2; ++pass) {
		for (int i = 0; i < 9000; ++i)
			tick(&f, true, true);
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(fabs(stats.ppm - f.ppm) <= 10.0);
		if (pass == 0) {
			f.rate = 44100.0;
			lm_obs_store_release(&f.header->sample_rate, 44100);
		}
		for (int i = 0; i < 5; ++i) {
			tick(&f, true, pass == 0);
			if (pass == 1)
				lm_receiver_pull(f.receiver, f.left, f.right, 441, 44100.0);
		}
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(stats.epoch == 1 && fabs(stats.ppm) < 10.0);
	}
done:
	fixture_close(&f);
	return ok;
}

static bool test_send_disabled(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	lm_obs_store_release(&f.header->send_enabled, 0);
	for (int i = 0; i < 100; ++i) {
		tick(&f, false, true);
		if (i > 0)
			CHECK(silent(&f));
	}
	/* Even an incorrectly resumed writer without an epoch bump cannot replay PCM. */
	lm_obs_store_release(&f.header->send_enabled, 1);
	lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
	for (int i = 0; i < 10; ++i) {
		tick(&f, false, true);
		CHECK(silent(&f));
	}
	writer_restart(&f, TEST_RATE, 0.2f);
	CHECK(warm_up(&f));
done:
	fixture_close(&f);
	return ok;
}

static bool test_invalid_mapping(void)
{
	bool ok = true;
	fixture f;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	uint32_t *fields[] = {&f.header->magic, &f.header->protocol_major, &f.header->header_bytes,
		&f.header->mapping_bytes, &f.header->data_offset, &f.header->channels, &f.header->capacity_frames};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		uint32_t saved = *fields[i];
		*fields[i] = saved + 1;
		f.now += 2 * TEST_QPC;
		lm_connection_poll(f.connection);
		lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
		CHECK(silent(&f));
		lm_receiver_stats stats;
		lm_receiver_get_stats(f.receiver, &stats);
		CHECK(!stats.connected);
		*fields[i] = saved;
	}
	CHECK(warm_up(&f));
	/* A formerly valid published header is also checked before PCM is read. */
	f.header->data_offset = UINT32_MAX;
	tick(&f, false, true);
	tick(&f, false, true);
	CHECK(silent(&f));
done:
	fixture_close(&f);
	return ok;
}

static bool test_short_mapping(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, sizeof(lm_obs_ring_header)));
	f.now += 2 * TEST_QPC;
	lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
	lm_obs_store_release(&f.header->write_frames, 10000);
	lm_connection_poll(f.connection);
	lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
	CHECK(silent(&f));
done:
	fixture_close(&f);
	return ok;
}

static bool test_reader_presence(void)
{
	fixture f;
	HANDLE mapping = NULL;
	lm_obs_readers *readers = NULL;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, f.readers_name);
	CHECK(mapping != NULL);
	readers = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*readers));
	CHECK(readers && readers->magic == LM_OBS_MAGIC && readers->slot_count == LM_OBS_MAX_READERS);
	lm_obs_reader_slot *slot = NULL;
	int claimed = 0;
	for (int i = 0; i < LM_OBS_MAX_READERS; ++i) {
		if (lm_obs_load_acquire(&readers->slot[i].pid) == GetCurrentProcessId()) {
			slot = &readers->slot[i];
			++claimed;
		}
	}
	CHECK(claimed == 1 && slot != NULL);
	CHECK(lm_obs_load_acquire(&slot->kind) == LM_RECEIVER_SOURCE);
	CHECK(f.now - lm_obs_load_acquire(&slot->heartbeat_qpc) <= TEST_QPC);
	for (int i = 0; i < 200; ++i)
		tick(&f, true, false);
	int64_t heartbeat = lm_obs_load_acquire(&slot->heartbeat_qpc);
	for (int i = 0; i < 200; ++i)
		tick(&f, true, false);
	CHECK(lm_obs_load_acquire(&slot->heartbeat_qpc) == heartbeat);
	/* Simulate a dead slot owner; the next management pass reclaims it. */
	lm_obs_store_release(&slot->pid, 123456789);
	lm_obs_store_release(&slot->heartbeat_qpc, f.now - 11 * TEST_QPC);
	for (int i = 0; i < 200; ++i)
		tick(&f, true, true);
	CHECK(lm_obs_load_acquire(&slot->pid) == GetCurrentProcessId());
	lm_receiver_destroy(f.receiver);
	f.receiver = NULL;
	lm_connection_destroy(f.connection);
	f.connection = NULL;
	CHECK(lm_obs_load_acquire(&slot->pid) == 0);
done:
	if (readers)
		UnmapViewOfFile(readers);
	if (mapping)
		CloseHandle(mapping);
	fixture_close(&f);
	return ok;
}

static bool test_presence_security(void)
{
	fixture f;
	HANDLE mapping = NULL;
	bool ok = true;
	BYTE security[4096], interactive[SECURITY_MAX_SID_SIZE];
	DWORD bytes = 0, sid_bytes = sizeof(interactive);
	PACL dacl = NULL;
	BOOL present = FALSE, defaulted = FALSE;
	CHECK(fixture_open(&f));
	mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, f.readers_name);
	CHECK(mapping != NULL);
	CHECK(GetKernelObjectSecurity(mapping, DACL_SECURITY_INFORMATION, security, sizeof(security), &bytes));
	CHECK(GetSecurityDescriptorDacl(security, &present, &dacl, &defaulted) && present && dacl);
	CHECK(CreateWellKnownSid(WinInteractiveSid, NULL, interactive, &sid_bytes));
	bool allowed = false;
	for (DWORD i = 0; i < dacl->AceCount; ++i) {
		ACCESS_ALLOWED_ACE *ace = NULL;
		CHECK(GetAce(dacl, i, (void **)&ace));
		if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && EqualSid(&ace->SidStart, interactive))
			allowed = (ace->Mask & GENERIC_ALL) || (ace->Mask & FILE_MAP_ALL_ACCESS) == FILE_MAP_ALL_ACCESS;
	}
	CHECK(allowed);
done:
	if (mapping)
		CloseHandle(mapping);
	fixture_close(&f);
	return ok;
}

static bool test_presence_optional(void)
{
	fixture f;
	HANDLE mapping = NULL, denied = NULL;
	PSECURITY_DESCRIPTOR security = NULL;
	bool ok = true;
	CHECK(fixture_open(&f));
	mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, f.readers_name);
	CHECK(mapping != NULL);
	lm_receiver_destroy(f.receiver);
	f.receiver = NULL;
	lm_connection_destroy(f.connection);
	f.connection = NULL;
	CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P", SDDL_REVISION_1, &security, NULL));
	CHECK(SetKernelObjectSecurity(mapping, DACL_SECURITY_INFORMATION, security));
	denied = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, f.readers_name);
	CHECK(denied == NULL && GetLastError() == ERROR_ACCESS_DENIED);
	lm_connection_config config = {0};
	config.ring_name = f.ring_name;
	config.readers_name = f.readers_name;
	config.now = test_now;
	config.clock_context = &f;
	config.qpc_frequency = TEST_QPC;
	f.connection = lm_connection_create(&config);
	CHECK(f.connection != NULL);
	f.receiver = lm_receiver_create(f.connection, LM_RECEIVER_SOURCE, TEST_RATE);
	CHECK(f.receiver != NULL);
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f) && lm_receiver_connected(f.receiver));
done:
	if (denied)
		CloseHandle(denied);
	if (security)
		LocalFree(security);
	if (mapping)
		CloseHandle(mapping);
	fixture_close(&f);
	return ok;
}

static bool test_heartbeat_boundaries(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	lm_receiver_stats before, after;
	lm_receiver_get_stats(f.receiver, &before);
	/* A writer callback can update the heartbeat after pull sampled QPC. */
	lm_obs_store_release(&f.header->heartbeat_qpc, f.now + TEST_QPC / 10000);
	lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
	lm_receiver_get_stats(f.receiver, &after);
	CHECK(settled(&f, 0.5f) && after.connected && after.resyncs == before.resyncs);
	/* Staleness drops even unconsumed, otherwise valid PCM from the FIFO. */
	f.now += TEST_QPC;
	lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
	lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
	CHECK(silent(&f) && !lm_receiver_connected(f.receiver));
done:
	fixture_close(&f);
	return ok;
}

static bool test_independent_receivers(void)
{
	fixture f;
	lm_receiver *second = NULL;
	HANDLE mapping = NULL;
	lm_obs_readers *readers = NULL;
	float left[441], right[441];
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	second = lm_receiver_create(f.connection, LM_RECEIVER_FILTER, 44100.0);
	CHECK(second != NULL);
	for (int i = 0; i < 300; ++i) {
		tick(&f, true, true);
		lm_receiver_pull(second, left, right, 441, 44100.0);
	}
	for (int i = 0; i < 100; ++i) {
		tick(&f, true, false);
		lm_receiver_pull(second, left, right, 441, 44100.0);
	}
	for (int i = 0; i < 300; ++i) {
		tick(&f, true, true);
		lm_receiver_pull(second, left, right, 441, 44100.0);
	}
	lm_receiver_stats first_stats, second_stats;
	lm_receiver_get_stats(f.receiver, &first_stats);
	lm_receiver_get_stats(second, &second_stats);
	CHECK(first_stats.overruns == 1 && second_stats.overruns == 0 && second_stats.underruns == 0);
	CHECK(settled(&f, 0.5f) && fabsf(left[440] - 0.5f) < 0.002f);
	CHECK(fabs(second_stats.fill_ms - 30.0) < 5.0);
	mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, f.readers_name);
	CHECK(mapping != NULL);
	readers = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(*readers));
	CHECK(readers != NULL);
	int claimed = 0;
	for (int i = 0; i < LM_OBS_MAX_READERS; ++i)
		if (lm_obs_load_acquire(&readers->slot[i].pid) == GetCurrentProcessId())
			++claimed;
	CHECK(claimed == 1);
done:
	if (readers)
		UnmapViewOfFile(readers);
	if (mapping)
		CloseHandle(mapping);
	lm_receiver_destroy(second);
	fixture_close(&f);
	return ok;
}

static bool test_large_pull(void)
{
	fixture f;
	float *left = NULL, *right = NULL;
	const int frames = 9001;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	left = malloc((size_t)(frames + 2) * sizeof(float));
	right = malloc((size_t)(frames + 2) * sizeof(float));
	CHECK(left && right);
	for (int i = 0; i < frames + 2; ++i)
		left[i] = right[i] = 123.0f;
	lm_receiver_pull(f.receiver, left + 1, right + 1, frames, TEST_RATE);
	CHECK(left[0] == 123.0f && left[frames + 1] == 123.0f);
	CHECK(right[0] == 123.0f && right[frames + 1] == 123.0f);
	for (int i = 1; i <= frames; ++i) {
		CHECK(isfinite(left[i]) && fabsf(left[i]) <= 0.51f);
		CHECK(isfinite(right[i]) && fabsf(right[i]) <= 0.51f);
		if (i > 2000)
			CHECK(left[i] == 0.0f && right[i] == 0.0f);
	}
	CHECK(warm_up(&f));
done:
	free(left);
	free(right);
	fixture_close(&f);
	return ok;
}

static bool test_threaded_format_handoff(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	CHECK(lm_connection_start(f.connection));
	f.threaded = true;
	/* Exercise pending and superseded preparations while the actual manager
	 * thread runs concurrently. Only this thread writes PCM and pulls audio. */
	for (int i = 0; i < 400; ++i) {
		if (i % 5 == 0)
			writer_restart(&f, i % 10 == 0 ? 44100.0 : 48000.0, 0.5f);
		tick(&f, true, true);
		for (int j = 0; j < TEST_FRAMES; ++j)
			CHECK(isfinite(f.left[j]) && fabsf(f.left[j]) < 0.6f);
		Sleep(1);
	}
	writer_restart(&f, 44100.0, 0.25f);
	for (int i = 0; i < 300; ++i) {
		tick(&f, true, true);
		Sleep(1);
	}
	CHECK(settled(&f, 0.25f));
	lm_receiver_stats stats;
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.connected && fabs(stats.fill_ms - 30.0) < 5.0);
done:
	fixture_close(&f);
	return ok;
}

static bool test_diagnostic_events(enum lm_receiver_kind kind)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	if (kind == LM_RECEIVER_FILTER) {
		lm_receiver_destroy(f.receiver);
		f.receiver = lm_receiver_create(f.connection, kind, TEST_RATE);
		CHECK(f.receiver != NULL);
	}
	f.capture_logs = true;
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	lm_connection_poll(f.connection);
	int index = (int)kind - 1;
	int calls = f.log_calls;
	int connects = f.events[index][CONNECT], disconnects = f.events[index][DISCONNECT];
	/* Several transitions between manager polls must all survive, and none
	 * may invoke the logger from pull. Snapshot-only logging loses this burst. */
	for (int i = 0; i < 2; ++i) {
		lm_obs_store_release(&f.header->send_enabled, 0);
		checked_pull(&f);
		lm_obs_store_release(&f.header->send_enabled, 1);
		checked_pull(&f);
	}
	CHECK(f.log_calls == calls && !f.audio_logged);
	lm_connection_poll(f.connection);
	CHECK(f.events[index][CONNECT] == connects + 2 && f.events[index][DISCONNECT] == disconnects + 2);
	CHECK(warm_up(&f));
	for (int i = 0; i < 8; ++i)
		tick(&f, false, true);
	CHECK(warm_up(&f));
	for (int i = 0; i < 100; ++i)
		tick(&f, true, false);
	checked_pull(&f);
	lm_connection_poll(f.connection);
	int resyncs = f.events[index][RESYNC];
	lm_receiver_resync(f.receiver);
	checked_pull(&f);
	lm_connection_poll(f.connection);
	CHECK(f.events[index][RESYNC] == resyncs + 1);
	writer_restart(&f, 44100.0, 0.25f);
	CHECK(warm_up(&f));
	lm_connection_poll(f.connection);
	for (int i = 0; i < EVENT_COUNT; ++i)
		CHECK(f.events[index][i] > 0);
	CHECK(f.events[index][EPOCH] == 2 && f.events[index][RATE] == 2);
	/* Inactive filter parents can stop pulling completely. The manager must
	 * still report both the stale writer and its return, in the same epoch. */
	connects = f.events[index][CONNECT];
	disconnects = f.events[index][DISCONNECT];
	f.now += TEST_QPC;
	lm_connection_poll(f.connection);
	CHECK(f.events[index][DISCONNECT] == disconnects + 1);
	lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
	lm_connection_poll(f.connection);
	CHECK(f.events[index][CONNECT] == connects + 1);
	CHECK(f.summaries[index] == 0 && !f.audio_logged);
done:
	fixture_close(&f);
	return ok;
}

static bool test_diagnostic_summaries(enum lm_receiver_kind kind)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	if (kind == LM_RECEIVER_FILTER) {
		lm_receiver_destroy(f.receiver);
		f.receiver = lm_receiver_create(f.connection, kind, TEST_RATE);
		CHECK(f.receiver != NULL);
	}
	int64_t created = f.now;
	f.capture_logs = true;
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	lm_connection_poll(f.connection);
	f.now = created + 600 * TEST_QPC - 1;
	lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
	lm_connection_poll(f.connection);
	int index = (int)kind - 1;
	CHECK(f.summaries[index] == 0);
	++f.now;
	int calls = f.log_calls;
	for (int i = 0; i < 65; ++i) {
		lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
		lm_connection_poll(f.connection);
		CHECK(f.summaries[index] == i + 1);
		f.now += 600 * TEST_QPC;
	}
	CHECK(f.log_calls == calls + 65 && f.suppressed == 0 && !f.repeated_format && !f.audio_logged);
done:
	fixture_close(&f);
	return ok;
}

static bool test_source_events(void) { return test_diagnostic_events(LM_RECEIVER_SOURCE); }
static bool test_filter_events(void) { return test_diagnostic_events(LM_RECEIVER_FILTER); }
static bool test_source_summaries(void) { return test_diagnostic_summaries(LM_RECEIVER_SOURCE); }
static bool test_filter_summaries(void) { return test_diagnostic_summaries(LM_RECEIVER_FILTER); }

/* Independent block clocks: publish only complete writer blocks, and catch up
 * all delayed blocks on the first writer callback after a starvation interval.
 * QPC is simulated, while the real ring, receiver and ASRC handle every sample. */
static bool simulate_blocks(fixture *f, int writer_frames, int reader_frames, int seconds,
			    int starvation_ms, lm_receiver_stats *result)
{
	float left[1024], right[1024], samples[2048];
	lm_receiver_stats previous = {0}, locked = {0}, stats = {0};
	bool ok = true;
	int64_t origin = f->now, last_resync = origin;
	int64_t writer_tick = 1, published_blocks = 0;
	int64_t input_rate = (int64_t)f->rate;
	CHECK(writer_frames <= 2048 && reader_frames <= 1024);
	for (int i = 0; i < writer_frames; ++i)
		samples[i] = f->value;
	lm_receiver_get_stats(f->receiver, &previous);
	for (int64_t pull = 1; pull * reader_frames <= (int64_t)seconds * TEST_RATE; ++pull) {
		int64_t elapsed = pull * reader_frames * TEST_QPC / TEST_RATE;
		int64_t write_time;
		while ((write_time = writer_tick * writer_frames * TEST_QPC / input_rate) <= elapsed) {
			lm_obs_store_release(&f->now, origin + write_time);
			bool starved = write_time >= 2 * TEST_QPC &&
				write_time % (2 * TEST_QPC) < starvation_ms * (TEST_QPC / 1000);
			if (!starved) {
				while (published_blocks < writer_tick) {
					lm_obs_write(f->header, f->pcm, samples, samples, (uint32_t)writer_frames);
					++published_blocks;
				}
				lm_obs_store_release(&f->header->heartbeat_qpc, f->now);
			}
			++writer_tick;
		}
		lm_obs_store_release(&f->now, origin + elapsed);
		lm_connection_poll(f->connection);
		lm_receiver_pull(f->receiver, left, right, reader_frames, TEST_RATE);
		lm_receiver_get_stats(f->receiver, &stats);
		CHECK(stats.fill_ms >= 0.0 && stats.fill_ms <= LM_OBS_CAPACITY_FRAMES * 1000.0 / f->rate);
		CHECK(stats.target_ms >= previous.target_ms && stats.target_ms <= 200.0);
		CHECK(stats.overruns == 0);
		if (stats.resyncs != previous.resyncs)
			last_resync = f->now;
		else {
			double slew = f->now - last_resync > 11 * TEST_QPC ? 20.0 : 200.0;
			CHECK(fabs(stats.ppm - previous.ppm) <= slew * reader_frames / TEST_RATE + 0.002);
		}
		int settle_seconds = starvation_ms ? 10 : 5;
		if (elapsed < settle_seconds * TEST_QPC) {
			locked = stats;
		} else {
			CHECK(stats.underruns == locked.underruns);
			for (int i = 0; i < reader_frames; ++i)
				CHECK(isfinite(left[i]) && fabsf(left[i] - f->value) < 0.002f &&
				      isfinite(right[i]) && fabsf(right[i] - f->value) < 0.002f);
		}
		previous = stats;
	}
done:
	*result = stats;
	printf("  %d-frame writer at %.0f Hz, %d-frame reader, %d ms starvation: "
	       "target=%.3f ms fill=%.3f ms under=%llu over=%llu\n",
	       writer_frames, f->rate, reader_frames, starvation_ms, stats.target_ms, stats.fill_ms,
	       (unsigned long long)stats.underruns, (unsigned long long)stats.overruns);
	return ok;
}

static bool test_adaptive_blocks(double input_rate, int writer_frames, int reader_frames, int starvation_ms)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	f.rate = input_rate;
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	if (reader_frames == 1024) {
		lm_receiver_destroy(f.receiver);
		f.receiver = lm_receiver_create(f.connection, LM_RECEIVER_FILTER, TEST_RATE);
		CHECK(f.receiver != NULL);
	}
	lm_receiver_stats stats;
	CHECK(simulate_blocks(&f, writer_frames, reader_frames, 60, starvation_ms, &stats));
	if (starvation_ms) {
		CHECK(stats.underruns <= 3 && stats.target_ms <= 200.0);
	} else if (reader_frames == 1024) {
		CHECK(stats.underruns == 0 && stats.target_ms <= 45.0);
	} else {
		CHECK(stats.target_ms >= 35.0 && stats.target_ms <= 60.0);
	}
done:
	fixture_close(&f);
	return ok;
}

static bool test_blocks_48000(void) { return test_adaptive_blocks(48000.0, 2048, 480, 0); }
static bool test_blocks_44100(void) { return test_adaptive_blocks(44100.0, 2048, 480, 0); }
static bool test_filter_blocks(void) { return test_adaptive_blocks(48000.0, 256, 1024, 0); }
static bool test_periodic_starvation(void) { return test_adaptive_blocks(48000.0, 480, 480, 80); }

static bool grow_target(fixture *f)
{
	lm_receiver_stats stats;
	for (int i = 0; i < 8; ++i)
		tick(f, false, true);
	for (int i = 0; i < 300; ++i)
		tick(f, true, true);
	lm_receiver_get_stats(f->receiver, &stats);
	return settled(f, f->value) && stats.target_ms > 30.0;
}

static bool test_target_resets(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	lm_receiver_stats stats;
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.target_ms == 30.0);
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	CHECK(grow_target(&f));
	writer_restart(&f, TEST_RATE, 0.25f);
	tick(&f, true, true);
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.epoch == 2 && stats.target_ms == 30.0);
	CHECK(warm_up(&f));
	CHECK(grow_target(&f));
	/* A same-epoch input format change must also reset the target. */
	f.rate = 44100.0;
	lm_obs_store_release(&f.header->sample_rate, 44100);
	tick(&f, true, true);
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.epoch == 2 && stats.target_ms == 30.0);
	CHECK(warm_up(&f));
	CHECK(grow_target(&f));
	lm_receiver_pull(f.receiver, f.left, f.right, 441, 44100.0);
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.target_ms == 30.0);
	CHECK(warm_up(&f));
	CHECK(grow_target(&f));
	/* No epoch bump: a stale heartbeat followed by fresh audio is a reconnect. */
	lm_receiver_get_stats(f.receiver, &stats);
	double saved_target = stats.target_ms;
	for (int i = 0; i < 60; ++i)
		tick(&f, false, true);
	CHECK(!lm_receiver_connected(f.receiver));
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.target_ms == saved_target);
	tick(&f, true, true);
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.connected && stats.epoch == 2 && stats.target_ms == saved_target);
	CHECK(warm_up(&f));
	lm_receiver_get_stats(f.receiver, &stats);
	CHECK(stats.target_ms == saved_target);
done:
	fixture_close(&f);
	return ok;
}

static bool test_target_slew(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = 200.0;
	for (int i = 0; i < 3000; ++i)
		tick(&f, true, true);
	/* Audio is 18 ms late while the writer still heartbeats. There is enough
	 * PCM left to keep playing. Acquisition has already finished. */
	f.now += 8 * TEST_QPC / 1000;
	lm_receiver_pull(f.receiver, f.left, f.right, 384, TEST_RATE);
	lm_receiver_stats before, after;
	lm_receiver_get_stats(f.receiver, &before);
	CHECK(before.target_ms == 30.0 && before.ppm > 100.0);
	f.now += TEST_QPC / 100;
	lm_obs_store_release(&f.header->heartbeat_qpc, f.now);
	lm_receiver_pull(f.receiver, f.left, f.right, TEST_FRAMES, TEST_RATE);
	lm_receiver_get_stats(f.receiver, &after);
	CHECK(after.target_ms == 35.0 && after.underruns == before.underruns);
	CHECK(after.resyncs == before.resyncs && settled(&f, f.value));
	CHECK(fabs(after.ppm - before.ppm) <= 20.0 / 100.0 + 0.002);
	for (int i = 0; i < 20; ++i) {
		tick(&f, true, true);
		lm_receiver_get_stats(f.receiver, &after);
		CHECK(after.target_ms == 35.0 && after.underruns == before.underruns);
	}
done:
	fixture_close(&f);
	return ok;
}

static bool test_target_cap(void)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	CHECK(warm_up(&f));
	double previous_target = 30.0;
	lm_receiver_stats stats = {0};
	for (int cycle = 0; cycle < 25; ++cycle) {
		for (int i = 0; i < 80; ++i) {
			tick(&f, i >= 30, true);
			lm_receiver_get_stats(f.receiver, &stats);
			CHECK(stats.target_ms >= previous_target && stats.target_ms <= 200.0);
			CHECK(stats.connected && stats.overruns == 0);
			previous_target = stats.target_ms;
		}
		CHECK(settled(&f, f.value));
	}
	CHECK(stats.target_ms == 200.0);
done:
	fixture_close(&f);
	return ok;
}

static bool test_soak(double ppm)
{
	fixture f;
	bool ok = true;
	CHECK(fixture_open(&f));
	CHECK(writer_open(&f, (DWORD)RING_BYTES));
	f.ppm = ppm;
	lm_receiver_stats locked = {0}, stats = {0};
	double low = 1000.0, high = 0.0;
	for (int i = 0; i < 720000; ++i) {
		tick(&f, true, true);
		if (i == 6000)
			lm_receiver_get_stats(f.receiver, &locked);
		if (i >= 6000 && i % 100 == 0) {
			lm_receiver_get_stats(f.receiver, &stats);
			/* The shared module's cold-start contract at the +/-1000 ppm
			 * clamp is +/-6.25 ms: there is no correction headroom left to
			 * recover the fill accumulated during acquisition (Task 1). */
			if (fabs(stats.fill_ms - 30.0) >= 6.25)
				printf("  t=%.2fs fill=%.3fms ppm=%.3f under=%llu over=%llu resync=%llu\n",
				       i / 100.0, stats.fill_ms, stats.ppm, (unsigned long long)stats.underruns,
				       (unsigned long long)stats.overruns, (unsigned long long)stats.resyncs);
			CHECK(stats.underruns == locked.underruns && stats.overruns == locked.overruns);
			CHECK(stats.resyncs == locked.resyncs);
			CHECK(fabs(stats.fill_ms - 30.0) < 6.25);
			CHECK(settled(&f, 0.5f));
			if (stats.fill_ms < low)
				low = stats.fill_ms;
			if (stats.fill_ms > high)
				high = stats.fill_ms;
		}
	}
	printf("  %+.0f ppm, 2 h at 48 kHz: fill %.3f..%.3f ms, correction %.2f ppm, under=%llu over=%llu\n",
	       ppm, low, high, stats.ppm, (unsigned long long)stats.underruns, (unsigned long long)stats.overruns);
done:
	fixture_close(&f);
	return ok;
}

static bool test_soak_positive(void) { return test_soak(1000.0); }
static bool test_soak_negative(void) { return test_soak(-1000.0); }

static void run_test(const char *name, bool (*test)(void))
{
	if (test_filter && !strstr(name, test_filter))
		return;
	++tests_run;
	printf("RUN %s\n", name);
	fflush(stdout);
	if (!test()) {
		++tests_failed;
		printf("FAIL %s\n", name);
	} else {
		printf("PASS %s\n", name);
	}
	fflush(stdout);
}

int main(int argc, char **argv)
{
	/* An optional name substring keeps focused regression runs inexpensive. */
	test_filter = argc > 1 ? argv[1] : NULL;
	run_test("source mixing: unity mono, stereo and six-channel OBS layouts", test_source_layout);
	run_test("connect after writer starts", test_late_connect);
	run_test("writer restart, retained mapping and bounded fade", test_restart_fade);
	run_test("input and output rate changes", test_rate_change);
	run_test("three-second writer stall and recovery without replay", test_stall_and_recovery);
	run_test("learned clock: +300 ppm survives a three-second stall", test_clock_positive);
	run_test("learned clock: -300 ppm survives a three-second stall", test_clock_negative);
	run_test("recurring one-second writer stalls for five simulated minutes", test_recurring_stalls);
	run_test("phase-locked 480-frame blocks at +20 ppm for one simulated hour", test_phase_locked_blocks);
	run_test("headroom grace after initial playback and underrun recovery", test_headroom_grace);
	run_test("live underrun wait and committed growth survives a later stop", test_live_underrun_wait);
	run_test("one-second reader pause and overrun resync", test_overrun);
	run_test("200 ms late worker resync follows current ring PCM within target +/-5 ms", test_late_worker_resync);
	run_test("learned clock and current ring PCM survive an overrun", test_overrun_clock);
	run_test("learned clock resets for same-epoch input and output rate changes", test_clock_rate_reset);
	run_test("send disabled and re-enabled", test_send_disabled);
	run_test("invalid mapping metadata", test_invalid_mapping);
	run_test("truncated mapping", test_short_mapping);
	run_test("reader slot activity, reclamation and unload", test_reader_presence);
	run_test("readers mapping grants interactive users full access", test_presence_security);
	run_test("audio continues when readers mapping access is denied", test_presence_optional);
	run_test("concurrent heartbeat and stale buffered audio", test_heartbeat_boundaries);
	run_test("independent receivers sharing one process slot", test_independent_receivers);
	run_test("large pull bounds, exact length and silence", test_large_pull);
	run_test("background manager and concurrent ASRC handoff", test_threaded_format_handoff);
	run_test("diagnostics: source events survive bursts outside audio pulls", test_source_events);
	run_test("diagnostics: filter events survive bursts outside audio pulls", test_filter_events);
	run_test("diagnostics: source ten-minute summaries survive OBS repeated-line filtering", test_source_summaries);
	run_test("diagnostics: filter ten-minute summaries survive OBS repeated-line filtering", test_filter_summaries);
	run_test("adaptive target: 2048-frame writer at 48 kHz", test_blocks_48000);
	run_test("adaptive target: 2048-frame writer at 44.1 kHz", test_blocks_44100);
	run_test("adaptive target: 1024-frame filter pulls and 256-frame writer", test_filter_blocks);
	run_test("adaptive target: recurring 80 ms writer starvation", test_periodic_starvation);
	run_test("adaptive target: epoch/rate resets and same-epoch stall retention", test_target_resets);
	run_test("adaptive target: headroom step preserves settled ppm slew", test_target_slew);
	run_test("adaptive target: monotonic growth capped at 200 ms", test_target_cap);
	run_test("+1000 ppm for two simulated hours", test_soak_positive);
	run_test("-1000 ppm for two simulated hours", test_soak_negative);
	printf("LiveMix OBS core tests: %d passed, %d failed (%d total)\n",
	       tests_run - tests_failed, tests_failed, tests_run);
	return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
