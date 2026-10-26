/* Loads the built plugin in libobs without the OBS application, scenes UI or
 * audio hardware. Run with run.py after building the plugin windows-x64 preset. */
#include "lm_obs_protocol.h"
#include "lm_obs_metadata.h"
#include <obs.h>
#include <util/platform.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define RATE 48000
#define FRAMES 480
#define CAPTURE_FRAMES RATE
#define TAU 6.2831853071795864769
#define RING_BYTES (sizeof(lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * 2u * sizeof(float))

typedef struct peer {
	HANDLE ring_handle, metadata_handle;
	lm_obs_ring_header *ring;
	lm_obs_metadata *metadata;
	float *pcm;
	obs_source_t *source;
	double tone;
	volatile LONG capture_enabled, captured;
	float samples[CAPTURE_FRAMES];
	double channel_error;
} peer;

typedef struct test_state {
	peer peers[2];
	volatile LONG stop;
} test_state;

static unsigned checks, failures;
#define EXPECT(condition) do { ++checks; if (!(condition)) { \
	++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static void publish_name(peer *p, unsigned slot, const char *name)
{
	lm_obs_metadata next = {0};
	next.magic = LM_OBS_METADATA_MAGIC;
	next.version = LM_OBS_METADATA_VERSION;
	next.mapping_bytes = LM_OBS_METADATA_BYTES;
	next.slot = slot;
	next.writer_pid = GetCurrentProcessId();
	if (name && *name) {
		next.session = 77;
		next.participant_id = slot + 1;
		next.stream_epoch = 1;
		next.state = LM_OBS_PARTICIPANT_CONNECTED;
		next.kind = slot == 1 ? LM_OBS_PARTICIPANT_NATIVE : LM_OBS_PARTICIPANT_WEB;
		next.nickname_bytes = (uint32_t)strlen(name);
		memcpy(next.nickname_utf8, name, next.nickname_bytes);
	}
	int64_t odd = lm_obs_load_acquire(&p->metadata->sequence) + 1;
	lm_obs_store_release(&p->metadata->sequence, odd);
	memcpy(p->metadata, &next, offsetof(lm_obs_metadata, sequence));
	memcpy((char *)p->metadata + offsetof(lm_obs_metadata, session), (const char *)&next + offsetof(lm_obs_metadata, session),
	       sizeof(next) - offsetof(lm_obs_metadata, session));
	lm_obs_store_release(&p->metadata->sequence, odd + 1);
}

static bool open_peer(peer *p, const wchar_t *base, unsigned slot)
{
	wchar_t name[144];
	swprintf(name, 144, slot == 1 ? L"%ls.Peer" : L"%ls.Peer2", base);
	p->ring_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)RING_BYTES, name);
	if (!p->ring_handle)
		return false;
	p->ring = MapViewOfFile(p->ring_handle, FILE_MAP_ALL_ACCESS, 0, 0, RING_BYTES);
	if (!p->ring)
		return false;
	memset(p->ring, 0, RING_BYTES);
	p->ring->magic = LM_OBS_MAGIC;
	p->ring->protocol_major = LM_OBS_PROTOCOL_MAJOR;
	p->ring->header_bytes = sizeof(*p->ring);
	p->ring->mapping_bytes = (uint32_t)RING_BYTES;
	p->ring->data_offset = sizeof(*p->ring);
	p->ring->channels = 2;
	p->ring->capacity_frames = LM_OBS_CAPACITY_FRAMES;
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	lm_obs_store_release(&p->ring->qpc_frequency, frequency.QuadPart);
	lm_obs_store_release(&p->ring->sample_rate, RATE);
	lm_obs_store_release(&p->ring->writer_pid, GetCurrentProcessId());
	lm_obs_store_release(&p->ring->epoch, 1);
	lm_obs_store_release(&p->ring->send_enabled, 1);
	p->pcm = (float *)((char *)p->ring + sizeof(*p->ring));
	wcscat(name, LM_OBS_METADATA_SUFFIX);
	p->metadata_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, LM_OBS_METADATA_BYTES, name);
	if (!p->metadata_handle)
		return false;
	p->metadata = MapViewOfFile(p->metadata_handle, FILE_MAP_ALL_ACCESS, 0, 0, LM_OBS_METADATA_BYTES);
	if (!p->metadata)
		return false;
	memset(p->metadata, 0, LM_OBS_METADATA_BYTES);
	publish_name(p, slot, slot == 1 ? "Alpha" : "Beta");
	p->tone = slot == 1 ? 440.0 : 880.0;
	return true;
}

static DWORD WINAPI write_tones(void *opaque)
{
	test_state *state = opaque;
	uint64_t frame = 0, deadline = os_gettime_ns();
	while (!InterlockedCompareExchange(&state->stop, 0, 0)) {
		for (unsigned slot = 0; slot < 2; ++slot) {
			peer *p = &state->peers[slot];
			float tone[FRAMES];
			for (unsigned i = 0; i < FRAMES; ++i)
				tone[i] = (float)(0.25 * sin(TAU * p->tone * (double)(frame + i) / RATE));
			lm_obs_write(p->ring, p->pcm, tone, tone, FRAMES);
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			lm_obs_store_release(&p->ring->heartbeat_qpc, now.QuadPart);
		}
		frame += FRAMES;
		deadline += UINT64_C(10000000);
		os_sleepto_ns(deadline);
	}
	return 0;
}

static void capture_audio(void *opaque, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)source;
	(void)muted;
	peer *p = opaque;
	if (!InterlockedCompareExchange(&p->capture_enabled, 0, 0))
		return;
	LONG start = InterlockedCompareExchange(&p->captured, 0, 0);
	uint32_t count = audio->frames;
	if (count > (uint32_t)(CAPTURE_FRAMES - start))
		count = (uint32_t)(CAPTURE_FRAMES - start);
	const float *left = (const float *)audio->data[0], *right = (const float *)audio->data[1];
	for (uint32_t i = 0; i < count; ++i) {
		p->samples[start + i] = left[i];
		p->channel_error += fabs((double)left[i] - right[i]);
	}
	InterlockedExchange(&p->captured, start + (LONG)count);
}

static double amplitude(const peer *p, double hz)
{
	/* 100 ms contains whole periods of both tones. Average magnitudes so the
	 * receiver's normal clock correction cannot cancel phases over a full second. */
	const unsigned window = RATE / 10;
	double sum = 0;
	for (unsigned start = 0; start < CAPTURE_FRAMES; start += window) {
		double real = 0, imag = 0;
		for (unsigned i = 0; i < window; ++i) {
			double angle = TAU * hz * i / RATE;
			real += p->samples[start + i] * cos(angle);
			imag += p->samples[start + i] * sin(angle);
		}
		sum += 2.0 * sqrt(real * real + imag * imag) / window;
	}
	return sum / (CAPTURE_FRAMES / window);
}

static void expect_status(obs_source_t *source, const char *expected)
{
	obs_properties_t *properties = obs_source_properties(source);
	obs_property_t *status = obs_properties_get(properties, "status");
	EXPECT(status && strcmp(obs_property_description(status), expected) == 0);
	/* There is still just the original status property. */
	EXPECT(obs_properties_first(properties) == status);
	EXPECT(!obs_property_next(&status));
	obs_properties_destroy(properties);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: livemix-obs-peer-source-tests <plugin.dll> <plugin-data>\n");
		return 2;
	}
	test_state *state = calloc(1, sizeof(*state));
	if (!state)
		return 2;
	bool started = false;
	HANDLE writer = NULL;
	obs_data_t *saved = NULL;
	obs_module_t *module = NULL;
	wchar_t base[105];
	swprintf(base, 105, L"Local\\LiveMix.R7Sources.%lu", GetCurrentProcessId());
	SetEnvironmentVariableW(L"LIVEMIX_OBS_RING", base);
	SetEnvironmentVariableW(L"LIVEMIX_OBS_PEER_RING", NULL);
	SetEnvironmentVariableW(L"LIVEMIX_OBS_PEER2_RING", NULL);
	if (!open_peer(&state->peers[0], base, 1) || !open_peer(&state->peers[1], base, 2)) {
		EXPECT(false);
		goto done;
	}
	started = obs_startup("en-US", NULL, NULL);
	EXPECT(started);
	if (!started)
		goto done;
	struct obs_audio_info audio = {RATE, SPEAKERS_STEREO};
	EXPECT(obs_reset_audio(&audio));
	int opened = obs_open_module(&module, argv[1], argv[2]);
	EXPECT(opened == MODULE_SUCCESS);
	if (opened != MODULE_SUCCESS)
		goto done;
	bool loaded = obs_init_module(module);
	EXPECT(loaded);
	if (!loaded)
		goto done;
	EXPECT(strcmp(obs_source_get_display_name("livemix_peer_source"), "LiveMix Collab Partner 1") == 0);
	EXPECT(strcmp(obs_source_get_display_name("livemix_peer2_source"), "LiveMix Collab Partner 2") == 0);
	/* A saved 0.15/0.16 scene uses the old ID and keeps the user's source name. */
	saved = obs_data_create_from_json("{\"id\":\"livemix_peer_source\",\"name\":\"Saved partner name\",\"settings\":{}}");
	state->peers[0].source = obs_load_source(saved);
	state->peers[1].source = obs_source_create("livemix_peer2_source", "Second custom name", NULL, NULL);
	EXPECT(state->peers[0].source && state->peers[1].source);
	if (!state->peers[0].source || !state->peers[1].source)
		goto done;
	writer = CreateThread(NULL, 0, write_tones, state, 0, NULL);
	EXPECT(writer != NULL);
	if (!writer)
		goto done;
	for (unsigned i = 0; i < 2; ++i)
		obs_source_add_audio_capture_callback(state->peers[i].source, capture_audio, &state->peers[i]);
	os_sleep_ms(2000); // normal mapping retry/prefill, not a timing-boundary test
	for (unsigned i = 0; i < 2; ++i)
		InterlockedExchange(&state->peers[i].capture_enabled, 1);
	for (unsigned wait = 0; wait < 500; ++wait) {
		if (InterlockedCompareExchange(&state->peers[0].captured, 0, 0) == CAPTURE_FRAMES &&
		    InterlockedCompareExchange(&state->peers[1].captured, 0, 0) == CAPTURE_FRAMES)
			break;
		os_sleep_ms(10);
	}
	for (unsigned i = 0; i < 2; ++i) {
		peer *p = &state->peers[i];
		obs_source_remove_audio_capture_callback(p->source, capture_audio, p);
		EXPECT(p->captured == CAPTURE_FRAMES);
		double own = amplitude(p, p->tone), other = amplitude(p, state->peers[1 - i].tone);
		printf("slot %u: %.0f Hz amplitude %.6f, other tone %.6f\n", i + 1, p->tone, own, other);
		EXPECT(own > 0.20 && own < 0.28);
		EXPECT(other < 0.01);
		EXPECT(p->channel_error < 0.001);
	}
	expect_status(state->peers[0].source, "Connected \xc2\xb7 Alpha");
	expect_status(state->peers[1].source, "Connected \xc2\xb7 Beta");
	obs_set_locale("ko-KR");
	EXPECT(strcmp(obs_source_get_display_name("livemix_peer_source"), "LiveMix 합방 상대 1") == 0);
	EXPECT(strcmp(obs_source_get_display_name("livemix_peer2_source"), "LiveMix 합방 상대 2") == 0);
	expect_status(state->peers[0].source, "연결됨 · Alpha");
	expect_status(state->peers[1].source, "연결됨 · Beta");
	publish_name(&state->peers[0], 1, "");
	expect_status(state->peers[0].source, "연결됨");
	expect_status(state->peers[1].source, "연결됨 · Beta");
	publish_name(&state->peers[0], 1, "Gamma");
	expect_status(state->peers[0].source, "연결됨 · Gamma");
	EXPECT(strcmp(obs_source_get_name(state->peers[0].source), "Saved partner name") == 0);
	EXPECT(strcmp(obs_source_get_name(state->peers[1].source), "Second custom name") == 0);
	obs_data_t *resaved = obs_save_source(state->peers[0].source);
	EXPECT(strcmp(obs_data_get_string(resaved, "id"), "livemix_peer_source") == 0);
	EXPECT(strcmp(obs_data_get_string(resaved, "name"), "Saved partner name") == 0);
	obs_data_release(resaved);
done:
	if (writer) {
		InterlockedExchange(&state->stop, 1);
		WaitForSingleObject(writer, INFINITE);
		CloseHandle(writer);
	}
	for (unsigned i = 0; i < 2; ++i)
		if (state->peers[i].source)
			obs_source_release(state->peers[i].source);
	if (saved)
		obs_data_release(saved);
	if (started)
		obs_shutdown();
	for (unsigned i = 0; i < 2; ++i) {
		peer *p = &state->peers[i];
		if (p->metadata) UnmapViewOfFile(p->metadata);
		if (p->metadata_handle) CloseHandle(p->metadata_handle);
		if (p->ring) UnmapViewOfFile(p->ring);
		if (p->ring_handle) CloseHandle(p->ring_handle);
	}
	free(state);
	printf("LiveMix OBS peer source checks: %u passed, %u failed\n", checks - failures, failures);
	return failures ? 1 : 0;
}
