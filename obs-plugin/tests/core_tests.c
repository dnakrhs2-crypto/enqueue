/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "receiver.h"
#include "lm_obs_protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define TEST_RATE 48000
#define TEST_FRAMES 480
#define TEST_QPC INT64_C(1000000000)
#define RING_BYTES (sizeof(lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * 2u * sizeof(float))

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
	bool threaded;
} fixture;

static unsigned name_serial;
static int tests_run, tests_failed;

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

static bool fixture_open(fixture *f)
{
	memset(f, 0, sizeof(*f));
	f->now = 20 * TEST_QPC;
	f->rate = TEST_RATE;
	f->value = 0.5f;
	swprintf(f->ring_name, 128, L"Local\\LiveMix.ObsCoreTests.%lu.%u", GetCurrentProcessId(), ++name_serial);
	swprintf(f->readers_name, 144, L"%ls.Readers", f->ring_name);
	lm_connection_config config = {0};
	config.ring_name = f->ring_name;
	config.readers_name = f->readers_name;
	config.now = test_now;
	config.clock_context = f;
	config.qpc_frequency = TEST_QPC;
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
			lm_obs_write(f->header, f->pcm, samples, samples, count);
			frames -= count;
		}
		lm_obs_store_release(&f->header->heartbeat_qpc, f->now);
	}
	if (!f->threaded)
		lm_connection_poll(f->connection);
	if (read_audio)
		lm_receiver_pull(f->receiver, f->left, f->right, TEST_FRAMES, TEST_RATE);
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
	CHECK(warm_up(&f));
	float previous = f.left[TEST_FRAMES - 1];
	for (int i = 0; i < 300; ++i) {
		tick(&f, false, true);
		for (int j = 0; j < TEST_FRAMES; ++j) {
			CHECK(fabsf(f.left[j] - previous) < 0.02f);
			previous = f.left[j];
		}
		if (i >= 10)
			CHECK(silent(&f));
	}
	lm_receiver_stats before, after;
	lm_receiver_get_stats(f.receiver, &before);
	CHECK(!before.connected && before.underruns == 1);
	f.value = -0.25f;
	CHECK(warm_up(&f));
	CHECK(settled(&f, -0.25f));
	lm_receiver_get_stats(f.receiver, &after);
	CHECK(after.connected && after.resyncs > before.resyncs);
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

int main(void)
{
	run_test("connect after writer starts", test_late_connect);
	run_test("writer restart, retained mapping and bounded fade", test_restart_fade);
	run_test("input and output rate changes", test_rate_change);
	run_test("three-second writer stall and recovery without replay", test_stall_and_recovery);
	run_test("one-second reader pause and overrun resync", test_overrun);
	run_test("send disabled and re-enabled", test_send_disabled);
	run_test("invalid mapping metadata", test_invalid_mapping);
	run_test("truncated mapping", test_short_mapping);
	run_test("reader slot activity, reclamation and unload", test_reader_presence);
	run_test("concurrent heartbeat and stale buffered audio", test_heartbeat_boundaries);
	run_test("independent receivers sharing one process slot", test_independent_receivers);
	run_test("large pull bounds, exact length and silence", test_large_pull);
	run_test("background manager and concurrent ASRC handoff", test_threaded_format_handoff);
	run_test("+1000 ppm for two simulated hours", test_soak_positive);
	run_test("-1000 ppm for two simulated hours", test_soak_negative);
	printf("LiveMix OBS core tests: %d passed, %d failed (%d total)\n",
	       tests_run - tests_failed, tests_failed, tests_run);
	return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
