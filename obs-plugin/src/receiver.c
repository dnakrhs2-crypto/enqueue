/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "receiver.h"
#include "lm_obs_protocol.h"
#include "lm_asrc.h"

#include <math.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define LM_OUTPUT_CHUNK 256
#define LM_MIN_RATE 8000
#define LM_MAX_RATE 384000
#define LM_TARGET_SECONDS 0.030
#define LM_MAX_TARGET_SECONDS 0.200
#define LM_HEADROOM_SECONDS 0.003
#define LM_HEADROOM_BUCKETS 201 /* Two seconds, in 10 ms buckets. */
#define LM_UNDERRUN_GROWTHS 20 /* At most 200 ms / 10 ms of provisional growth. */
#define LM_FADE_SECONDS 0.010
#define LM_RING_BYTES (sizeof(lm_obs_ring_header) + LM_OBS_CAPACITY_FRAMES * 2u * sizeof(float))

typedef struct lm_view {
	HANDLE mapping;
	const lm_obs_ring_header *header;
	const float *pcm;
	struct lm_view *next;
} lm_view;

typedef struct lm_asrc_bank {
	lm_asrc *state;
	int64_t format;
} lm_asrc_bank;

typedef struct lm_fill_minimum {
	int64_t bucket;
	double relative_fill;
} lm_fill_minimum;

typedef struct lm_target_growth {
	int64_t when;
	double frames;
} lm_target_growth;

struct lm_connection {
	lm_connection_config config;
	wchar_t *ring_name, *readers_name;
	int64_t frequency, next_probe;
	HANDLE thread, stop_event;
	SRWLOCK receivers_lock;
	lm_receiver *receivers;
	lm_view *views;
	PVOID volatile published_view;
	HANDLE readers_mapping;
	lm_obs_readers *presence;
	int slot;
	int64_t pid;
	volatile int64_t last_pull, last_kind, last_fill_us;
	bool rejected;
};

struct lm_receiver {
	lm_connection *connection;
	lm_receiver *next;
	enum lm_receiver_kind kind;
	lm_asrc_bank bank[2];
	volatile int64_t requested_format, ready_bank, active_bank;
	int64_t prepared_format; /* management thread only */
	float *fifo;
	int fifo_frames;
	float output[LM_OUTPUT_CHUNK * 2];
	lm_drift drift;
	const lm_view *view;
	int64_t epoch, read_pos;
	int input_rate, output_rate;
	double target;
	lm_fill_minimum fill_minima[LM_HEADROOM_BUCKETS];
	lm_target_growth underrun_growth[LM_UNDERRUN_GROWTHS];
	int64_t playing_since, last_written, last_block;
	int64_t last_write_heartbeat;
	double pending_writer_wait, writer_headroom;
	bool online, playing;
	int fade_frames, fade_in, tail_left;
	float tail[2], last[2];
	volatile int64_t stat_fill_us, stat_target_us, stat_ppm_milli;
	volatile int64_t stat_under, stat_over, stat_resync, stat_epoch, stat_connected;
};

static int64_t qpc_now(void *context)
{
	(void)context;
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}

static int64_t connection_now(const lm_connection *c)
{
	return c->config.now(c->config.clock_context);
}

static void connection_log(lm_connection *c, enum lm_log_level level, const char *message)
{
	if (c->config.log)
		c->config.log(c->config.log_context, level, message);
}

static int64_t compare_exchange(volatile int64_t *value, int64_t next, int64_t expected)
{
	return InterlockedCompareExchange64((volatile LONG64 *)value, next, expected);
}

static lm_view *current_view(const lm_connection *c)
{
	return InterlockedCompareExchangePointer((PVOID volatile *)&c->published_view, NULL, NULL);
}

/* V1 has a fixed layout. Never follow data_offset or touch PCM until every
 * bound is checked and MapViewOfFile has accepted the entire declared size. */
static bool valid_header(const lm_obs_ring_header *h)
{
	return h->magic == LM_OBS_MAGIC && h->protocol_major == LM_OBS_PROTOCOL_MAJOR &&
	       h->header_bytes == sizeof(*h) && h->mapping_bytes == LM_RING_BYTES &&
	       h->data_offset == sizeof(*h) && h->channels == LM_OBS_CHANNELS &&
	       h->capacity_frames == LM_OBS_CAPACITY_FRAMES;
}

static bool live_header(const lm_connection *c, const lm_obs_ring_header *h, int64_t now)
{
	if (!valid_header(h) || lm_obs_load_acquire(&h->send_enabled) != 1 ||
	    lm_obs_load_acquire(&h->qpc_frequency) != c->frequency)
		return false;
	int64_t heartbeat = lm_obs_load_acquire(&h->heartbeat_qpc);
	int64_t rate = lm_obs_load_acquire(&h->sample_rate);
	/* A concurrently running writer may publish a heartbeat just after this
	 * callback sampled now. That is fresh audio, not a disconnection. */
	bool fresh = heartbeat > 0 && (heartbeat >= now ? heartbeat - now <= c->frequency / 2
						      : now - heartbeat <= c->frequency / 2);
	return fresh &&
	       rate >= LM_MIN_RATE && rate <= LM_MAX_RATE && lm_obs_load_acquire(&h->write_frames) >= 0;
}

static void open_ring(lm_connection *c)
{
	/* Holding even one handle pins this session's named object across writer
	 * restarts. If it is temporarily invalid, retain it but stop publication. */
	if (c->views) {
		InterlockedExchangePointer(&c->published_view, valid_header(c->views->header) ? c->views : NULL);
		return;
	}
	HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, c->ring_name);
	if (!mapping)
		return;
	const lm_obs_ring_header *header = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(*header));
	bool valid = header && valid_header(header);
	if (header)
		UnmapViewOfFile(header); /* This inspection view was never published. */
	header = valid ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, LM_RING_BYTES) : NULL;
	if (!header || !valid_header(header)) {
		if (header)
			UnmapViewOfFile(header);
		CloseHandle(mapping);
		if (!c->rejected)
			connection_log(c, LM_LOG_WARNING, "Rejected an inconsistent audio mapping");
		c->rejected = true;
		return;
	}
	lm_view *view = calloc(1, sizeof(*view));
	if (!view) {
		UnmapViewOfFile(header);
		CloseHandle(mapping);
		return;
	}
	view->mapping = mapping;
	view->header = header;
	view->pcm = (const float *)((const unsigned char *)header + sizeof(*header));
	view->next = c->views;
	c->views = view;
	c->rejected = false;
	InterlockedExchangePointer(&c->published_view, view);
	connection_log(c, LM_LOG_INFO, "Connected to the LiveMix audio mapping");
}

static PSECURITY_DESCRIPTOR mapping_security(void)
{
	HANDLE token = NULL;
	TOKEN_USER *user = NULL;
	LPWSTR sid = NULL;
	PSECURITY_DESCRIPTOR descriptor = NULL;
	wchar_t sddl[256];
	DWORD bytes = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return NULL;
	GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
	user = malloc(bytes);
	if (user && GetTokenInformation(token, TokenUser, user, bytes, &bytes) &&
	    ConvertSidToStringSidW(user->User.Sid, &sid)) {
		int length = swprintf(sddl, 256, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%ls)S:(ML;;NW;;;ME)", sid);
		if (length > 0 && length < 256)
			ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL);
	}
	if (sid)
		LocalFree(sid);
	free(user);
	CloseHandle(token);
	return descriptor;
}

static void open_presence(lm_connection *c)
{
	if (c->presence)
		return;
	PSECURITY_DESCRIPTOR descriptor = mapping_security();
	if (!descriptor)
		return; /* Never fall back to a descriptor that blocks the other integrity level. */
	SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
	HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
					   sizeof(lm_obs_readers), c->readers_name);
	DWORD error = GetLastError();
	LocalFree(descriptor);
	if (!mapping)
		return;
	lm_obs_readers *readers = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(*readers));
	if (readers && error != ERROR_ALREADY_EXISTS) {
		memset(readers, 0, sizeof(*readers));
		readers->protocol_major = LM_OBS_PROTOCOL_MAJOR;
		readers->slot_count = LM_OBS_MAX_READERS;
		InterlockedExchange((volatile LONG *)&readers->magic, (LONG)LM_OBS_MAGIC);
	}
	MemoryBarrier();
	if (!readers || readers->magic != LM_OBS_MAGIC || readers->protocol_major != LM_OBS_PROTOCOL_MAJOR ||
	    readers->slot_count != LM_OBS_MAX_READERS) {
		if (readers)
			UnmapViewOfFile(readers);
		CloseHandle(mapping);
		return;
	}
	c->readers_mapping = mapping;
	c->presence = readers;
}

static bool slot_stale(const lm_connection *c, int64_t heartbeat, int64_t now)
{
	return now >= heartbeat && (double)(now - heartbeat) / (double)c->frequency > 10.0;
}

static void update_presence(lm_connection *c, int64_t now)
{
	open_presence(c);
	if (!c->presence)
		return;
	if (c->slot >= 0 && lm_obs_load_acquire(&c->presence->slot[c->slot].pid) != c->pid)
		c->slot = -1;
	if (c->slot < 0) {
		for (int i = 0; i < LM_OBS_MAX_READERS; ++i) {
			lm_obs_reader_slot *slot = &c->presence->slot[i];
			int64_t owner = lm_obs_load_acquire(&slot->pid);
			int64_t heartbeat = lm_obs_load_acquire(&slot->heartbeat_qpc);
			if (owner != 0 && !slot_stale(c, heartbeat, now))
				continue;
			if (compare_exchange(&slot->pid, -c->pid, owner) != owner)
				continue;
			/* Recheck after claiming the slot so a concurrent heartbeat wins. */
			if (owner != 0 && !slot_stale(c, lm_obs_load_acquire(&slot->heartbeat_qpc), now)) {
				lm_obs_store_release(&slot->pid, owner);
				continue;
			}
			lm_obs_store_release(&slot->heartbeat_qpc, 0);
			lm_obs_store_release(&slot->kind, 0);
			lm_obs_store_release(&slot->fill_us, 0);
			lm_obs_store_release(&slot->pid, c->pid);
			c->slot = i;
			break;
		}
	}
	int64_t pulled = lm_obs_load_acquire(&c->last_pull);
	if (c->slot >= 0 && pulled > 0 && now >= pulled && now - pulled <= c->frequency) {
		lm_obs_reader_slot *slot = &c->presence->slot[c->slot];
		if (compare_exchange(&slot->pid, -c->pid, c->pid) == c->pid) {
			lm_obs_store_release(&slot->kind, lm_obs_load_acquire(&c->last_kind));
			lm_obs_store_release(&slot->fill_us, lm_obs_load_acquire(&c->last_fill_us));
			lm_obs_store_release(&slot->heartbeat_qpc, pulled);
			lm_obs_store_release(&slot->pid, c->pid);
		}
	}
}

static int valid_output_rate(double rate)
{
	if (!isfinite(rate) || rate < LM_MIN_RATE || rate > LM_MAX_RATE || floor(rate) != rate)
		return 0;
	return (int)rate;
}

static int64_t format_key(int input_rate, int output_rate)
{
	return (int64_t)(((uint64_t)(uint32_t)input_rate << 32) | (uint32_t)output_rate);
}

static void prepare_formats(lm_connection *c)
{
	AcquireSRWLockExclusive(&c->receivers_lock);
	for (lm_receiver *r = c->receivers; r; r = r->next) {
		int64_t requested = lm_obs_load_acquire(&r->requested_format);
		if (requested == 0 || requested == r->prepared_format || lm_obs_load_acquire(&r->ready_bank) >= 0)
			continue;
		int spare = 1 - (int)lm_obs_load_acquire(&r->active_bank);
		int input_rate = (int)((uint64_t)requested >> 32);
		int output_rate = (int)((uint64_t)requested & UINT32_MAX);
		lm_asrc_init(r->bank[spare].state, 2, input_rate, output_rate);
		r->bank[spare].format = requested;
		r->prepared_format = requested;
		lm_obs_store_release(&r->ready_bank, spare);
	}
	ReleaseSRWLockExclusive(&c->receivers_lock);
}

void lm_connection_poll(lm_connection *c)
{
	if (!c)
		return;
	int64_t now = connection_now(c);
	if (now >= c->next_probe) {
		c->next_probe = now + c->frequency;
		open_ring(c);
		update_presence(c, now);
	}
	prepare_formats(c);
}

static DWORD WINAPI connection_thread(void *opaque)
{
	lm_connection *c = opaque;
	do {
		lm_connection_poll(c);
	} while (WaitForSingleObject(c->stop_event, 10) == WAIT_TIMEOUT);
	return 0;
}

lm_connection *lm_connection_create(const lm_connection_config *config)
{
	lm_connection *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	if (config)
		c->config = *config;
	c->slot = -1;
	c->pid = GetCurrentProcessId();
	InitializeSRWLock(&c->receivers_lock);
	if (!c->config.now) {
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		c->config.now = qpc_now;
		c->config.qpc_frequency = frequency.QuadPart;
	}
	c->frequency = c->config.qpc_frequency;
	c->ring_name = _wcsdup(c->config.ring_name ? c->config.ring_name : LM_OBS_RING_NAME);
	if (c->config.readers_name) {
		c->readers_name = _wcsdup(c->config.readers_name);
	} else if (c->ring_name) {
		size_t count = wcslen(c->ring_name) + 9;
		c->readers_name = calloc(count, sizeof(wchar_t));
		if (c->readers_name)
			swprintf(c->readers_name, count, L"%ls.Readers", c->ring_name);
	}
	c->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!c->ring_name || !c->readers_name || !c->stop_event || c->frequency <= 0 ||
	    c->frequency > INT64_C(1000000000000)) {
		lm_connection_destroy(c);
		return NULL;
	}
	lm_connection_poll(c);
	return c;
}

bool lm_connection_start(lm_connection *c)
{
	if (!c)
		return false;
	if (c->thread)
		return true;
	c->thread = CreateThread(NULL, 0, connection_thread, c, 0, NULL);
	return c->thread != NULL;
}

void lm_connection_destroy(lm_connection *c)
{
	if (!c)
		return;
	if (c->thread) {
		SetEvent(c->stop_event);
		WaitForSingleObject(c->thread, INFINITE);
		CloseHandle(c->thread);
	}
	if (c->presence) {
		if (c->slot >= 0) {
			lm_obs_reader_slot *slot = &c->presence->slot[c->slot];
			if (compare_exchange(&slot->pid, -c->pid, c->pid) == c->pid) {
				lm_obs_store_release(&slot->heartbeat_qpc, 0);
				lm_obs_store_release(&slot->kind, 0);
				lm_obs_store_release(&slot->fill_us, 0);
				lm_obs_store_release(&slot->pid, 0);
			}
		}
		UnmapViewOfFile(c->presence);
		CloseHandle(c->readers_mapping);
	}
	for (lm_view *view = c->views; view;) {
		lm_view *next = view->next;
		UnmapViewOfFile(view->header);
		CloseHandle(view->mapping);
		free(view);
		view = next;
	}
	if (c->stop_event)
		CloseHandle(c->stop_event);
	free(c->ring_name);
	free(c->readers_name);
	free(c);
}

lm_receiver *lm_receiver_create(lm_connection *c, enum lm_receiver_kind kind, double out_rate)
{
	int output_rate = valid_output_rate(out_rate);
	if (!c || !output_rate || (kind != LM_RECEIVER_SOURCE && kind != LM_RECEIVER_FILTER))
		return NULL;
	lm_receiver *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->connection = c;
	r->kind = kind;
	r->ready_bank = -1;
	r->fifo = malloc(LM_OBS_CAPACITY_FRAMES * 2u * sizeof(float));
	r->bank[0].state = malloc(lm_asrc_size(2));
	r->bank[1].state = malloc(lm_asrc_size(2));
	if (!r->fifo || !r->bank[0].state || !r->bank[1].state) {
		lm_receiver_destroy(r);
		return NULL;
	}
	int input_rate = 48000;
	lm_view *view = current_view(c);
	if (view && live_header(c, view->header, connection_now(c)))
		input_rate = (int)lm_obs_load_acquire(&view->header->sample_rate);
	lm_asrc_init(r->bank[0].state, 2, input_rate, output_rate);
	r->bank[0].format = format_key(input_rate, output_rate);
	r->prepared_format = r->bank[0].format;
	r->requested_format = r->bank[0].format;
	r->output_rate = output_rate;
	r->stat_target_us = 30000;
	r->fade_frames = (int)(out_rate * LM_FADE_SECONDS);
	AcquireSRWLockExclusive(&c->receivers_lock);
	r->next = c->receivers;
	c->receivers = r;
	ReleaseSRWLockExclusive(&c->receivers_lock);
	return r;
}

void lm_receiver_destroy(lm_receiver *r)
{
	if (!r)
		return;
	lm_connection *c = r->connection;
	AcquireSRWLockExclusive(&c->receivers_lock);
	lm_receiver **link = &c->receivers;
	while (*link && *link != r)
		link = &(*link)->next;
	if (*link)
		*link = r->next;
	ReleaseSRWLockExclusive(&c->receivers_lock);
	free(r->bank[0].state);
	free(r->bank[1].state);
	free(r->fifo);
	free(r);
}

static void start_tail(lm_receiver *r)
{
	r->tail[0] = r->last[0];
	r->tail[1] = r->last[1];
	r->tail_left = r->fade_frames;
	r->fade_in = 0;
}

static void begin_prefill(lm_receiver *r, int64_t written, bool fresh_only)
{
	if (r->playing)
		start_tail(r);
	r->playing = false;
	r->playing_since = 0;
	memset(r->fill_minima, 0, sizeof(r->fill_minima));
	r->fifo_frames = 0;
	lm_asrc *asrc = r->bank[(int)lm_obs_load_acquire(&r->active_bank)].state;
	int64_t queue_target = (int64_t)ceil(r->target - lm_asrc_latency_input_frames(asrc));
	r->read_pos = !fresh_only && written >= queue_target ? written - queue_target : written;
	/* At a new epoch, frames already written are fresh, even before target. */
	if (!fresh_only && written < queue_target)
		r->read_pos = 0;
	lm_obs_store_release(&r->stat_fill_us, 0);
	lm_obs_store_release(&r->stat_ppm_milli, 0);
}

static void emit(lm_receiver *r, float *left, float *right, const float *pcm, int frames)
{
	for (int i = 0; i < frames; ++i) {
		float gain = 0.0f;
		if (pcm) {
			if (r->fade_in < r->fade_frames)
				++r->fade_in;
			gain = (float)r->fade_in / (float)r->fade_frames;
		}
		float tail_gain = 0.0f;
		if (r->tail_left > 0) {
			--r->tail_left;
			tail_gain = (float)r->tail_left / (float)r->fade_frames;
		}
		r->last[0] = (pcm ? pcm[i * 2] * gain : 0.0f) + r->tail[0] * tail_gain;
		r->last[1] = (pcm ? pcm[i * 2 + 1] * gain : 0.0f) + r->tail[1] * tail_gain;
		left[i] = r->last[0];
		right[i] = r->last[1];
	}
}

static bool adopt_format(lm_receiver *r, int64_t format)
{
	lm_obs_store_release(&r->requested_format, format);
	int active = (int)lm_obs_load_acquire(&r->active_bank);
	int ready = (int)lm_obs_load_acquire(&r->ready_bank);
	if (ready >= 0) {
		if (r->bank[active].format != format && r->bank[ready].format == format)
			lm_obs_store_release(&r->active_bank, ready);
		/* Publishing the new active bank before releasing the ready bank keeps
		 * the manager from rebuilding an ASRC that the callback is using. */
		lm_obs_store_release(&r->ready_bank, -1);
	}
	active = (int)lm_obs_load_acquire(&r->active_bank);
	return r->bank[active].format == format;
}

static void record_fill(lm_receiver *r, double fill)
{
	int64_t fill_us = (int64_t)(fill * 1000000.0 / r->input_rate);
	lm_obs_store_release(&r->stat_fill_us, fill_us);
	lm_obs_store_release(&r->connection->last_fill_us, fill_us);
}

static void set_target(lm_receiver *r, double target)
{
	if (target == r->target)
		return;
	if (r->playing) {
		/* Keep the integrator, correction and acquisition clock. Rebase the
		 * filtered error and gain scaling; update still applies its usual slew. */
		r->drift.lp -= target - r->target;
		r->drift.kp *= r->target / target;
		r->drift.ki *= r->target / target;
		r->drift.target = target;
	}
	r->target = target;
	memset(r->fill_minima, 0, sizeof(r->fill_minima));
	lm_obs_store_release(&r->stat_target_us, (int64_t)llround(target * 1000000.0 / r->input_rate));
}

static void grow_target(lm_receiver *r, double seconds)
{
	/* At the highest supported rates the ring itself is smaller than 200 ms. */
	double limit = fmin(r->input_rate * LM_MAX_TARGET_SECONDS, LM_OBS_CAPACITY_FRAMES - 1.0);
	set_target(r, fmin(r->target + r->input_rate * seconds, limit));
}

static void settle_underrun_growth(lm_receiver *r, int64_t now)
{
	const lm_obs_ring_header *h = r->view ? r->view->header : NULL;
	if (!h || !valid_header(h) || lm_obs_load_acquire(&h->epoch) != r->epoch ||
	    lm_obs_load_acquire(&h->sample_rate) != r->input_rate)
		return;
	int64_t frequency = r->connection->frequency;
	int64_t heartbeat = lm_obs_load_acquire(&h->heartbeat_qpc);
	bool stale = heartbeat >= 0 && now > heartbeat && now - heartbeat > frequency / 2;
	double undo = 0.0;
	for (int i = 0; i < LM_UNDERRUN_GROWTHS; ++i) {
		lm_target_growth *growth = &r->underrun_growth[i];
		if (growth->frames == 0.0)
			continue;
		/* Check when the heartbeat became stale, even if pulls were paused.
		 * Each increment expires separately if several underruns occur. */
		if (stale && heartbeat - growth->when <= frequency / 2) {
			undo += growth->frames;
			growth->frames = 0.0;
		} else if (now >= growth->when && now - growth->when > frequency) {
			growth->frames = 0.0;
		}
	}
	if (undo > 0.0)
		set_target(r, r->target - undo);
}

static void grow_after_underrun(lm_receiver *r, int64_t now)
{
	double before = r->target;
	grow_target(r, 0.010);
	if (r->target == before)
		return;
	for (int i = 0; i < LM_UNDERRUN_GROWTHS; ++i) {
		if (r->underrun_growth[i].frames == 0.0) {
			r->underrun_growth[i].when = now;
			r->underrun_growth[i].frames = r->target - before;
			break;
		}
	}
}

static double writer_advance(lm_receiver *r, int64_t written, int64_t heartbeat, int64_t now)
{
	double age = now > heartbeat ? (double)(now - heartbeat) * r->input_rate / r->connection->frequency : 0.0;
	if (written > r->last_written) {
		int64_t block = written - r->last_written;
		r->last_block = block < LM_OBS_CAPACITY_FRAMES ? block : LM_OBS_CAPACITY_FRAMES - 1;
		/* Smoothing hides the time for which a block has not yet been
		 * published. Reserve that observed headroom only after more PCM
		 * confirms a live writer delay; an offline stall proves nothing
		 * about the buffer needed during normal playback. */
		if (heartbeat >= r->last_write_heartbeat &&
		    heartbeat - r->last_write_heartbeat <= r->connection->frequency / 2)
			r->writer_headroom = fmax(r->writer_headroom, r->pending_writer_wait);
		r->pending_writer_wait = 0.0;
		r->last_write_heartbeat = heartbeat;
	} else if (written < r->last_written) {
		r->last_block = 0;
		r->pending_writer_wait = r->writer_headroom = 0.0;
		r->last_write_heartbeat = heartbeat;
	}
	r->last_written = written;
	r->pending_writer_wait = fmax(r->pending_writer_wait, age);
	/* Only the controller's measurement is extrapolated. The ring reader,
	 * FIFO and prefill continue to use exclusively published audio frames. */
	return fmin(age, (double)r->last_block);
}

static void check_headroom(lm_receiver *r, double fill, int frames, int64_t now, int64_t heartbeat)
{
	if (!r->online || !r->playing || now < r->playing_since ||
	    now - r->playing_since < r->connection->frequency)
		return; /* Prefill, waits and the first second of playback are not steady audio. */
	if (now > heartbeat &&
	    (double)(now - heartbeat) * r->input_rate / r->connection->frequency > r->last_block)
		return; /* An overdue writer may be stopped; wait for confirmed live delay. */
	int64_t width = (r->connection->frequency + 99) / 100;
	int64_t bucket = now / width + 1; /* Zero denotes an unused entry. */
	lm_fill_minimum *sample = &r->fill_minima[bucket % LM_HEADROOM_BUCKETS];
	double relative_fill = fill - r->target - r->drift.lp;
	if (sample->bucket != bucket) {
		sample->bucket = bucket;
		sample->relative_fill = relative_fill;
	} else {
		sample->relative_fill = fmin(sample->relative_fill, relative_fill);
	}
	/* Project the observed minimum to the controller's target: subtract its
	 * filtered fill error while playing, and credit later target increases.
	 * This accounts for headroom already requested but not yet accumulated,
	 * and protects against the controller draining an initially fuller queue.
	 * Growth clears the window so an old minimum cannot request it again. The
	 * oldest bucket is retained until fully expired (at most 10 ms extra).
	 * Storage and work are bounded regardless of callback size or rate. */
	double minimum = relative_fill;
	for (int i = 0; i < LM_HEADROOM_BUCKETS; ++i) {
		const lm_fill_minimum *entry = &r->fill_minima[i];
		if (entry->bucket > 0 && bucket >= entry->bucket &&
		    bucket - entry->bucket <= 2 * r->connection->frequency / width)
			minimum = fmin(minimum, entry->relative_fill);
	}
	double required = r->input_rate * (frames / (double)r->output_rate + LM_HEADROOM_SECONDS);
	/* Unpublished block time and measured low fill are separate lower bounds,
	 * not additive: the same writer delay must not count twice. */
	if (fmin(minimum + r->target, r->target - r->writer_headroom) < required)
		grow_target(r, 0.005);
}

void lm_receiver_pull(lm_receiver *r, float *out_l, float *out_r, int frames, double out_rate)
{
	if (frames <= 0)
		return;
	int output_rate = valid_output_rate(out_rate);
	if (!r || !output_rate) {
		memset(out_l, 0, (size_t)frames * sizeof(float));
		memset(out_r, 0, (size_t)frames * sizeof(float));
		return;
	}
	lm_connection *c = r->connection;
	int64_t now = connection_now(c);
	lm_obs_store_release(&c->last_kind, r->kind);
	lm_obs_store_release(&c->last_pull, now);
	lm_view *view = current_view(c);
	settle_underrun_growth(r, now);
	if (!view || !live_header(c, view->header, now)) {
		if (r->online) {
			int64_t written = r->view ? lm_obs_load_acquire(&r->view->header->write_frames) : 0;
			begin_prefill(r, written >= 0 ? written : 0, true);
		}
		r->online = false;
		lm_obs_store_release(&r->stat_connected, 0);
		emit(r, out_l, out_r, NULL, frames);
		return;
	}
	const lm_obs_ring_header *h = view->header;
	int64_t epoch = lm_obs_load_acquire(&h->epoch);
	int input_rate = (int)lm_obs_load_acquire(&h->sample_rate);
	int64_t written = lm_obs_load_acquire(&h->write_frames);
	if (input_rate < LM_MIN_RATE || input_rate > LM_MAX_RATE || written < 0 ||
	    lm_obs_load_acquire(&h->epoch) != epoch || !live_header(c, h, now)) {
		if (r->online)
			begin_prefill(r, written >= 0 ? written : 0, true);
		r->online = false;
		lm_obs_store_release(&r->stat_connected, 0);
		emit(r, out_l, out_r, NULL, frames);
		return;
	}
	bool changed = view != r->view || epoch != r->epoch || input_rate != r->input_rate ||
		       output_rate != r->output_rate;
	if (changed || !r->online) {
		bool same_epoch = view == r->view && epoch == r->epoch;
		int64_t resume_position = r->read_pos;
		r->view = view;
		r->epoch = epoch;
		r->input_rate = input_rate;
		r->output_rate = output_rate;
		if (changed) {
			r->target = input_rate * LM_TARGET_SECONDS;
			r->last_written = written;
			r->last_block = 0;
			r->last_write_heartbeat = lm_obs_load_acquire(&h->heartbeat_qpc);
			r->pending_writer_wait = r->writer_headroom = 0.0;
			memset(r->underrun_growth, 0, sizeof(r->underrun_growth));
			lm_obs_store_release(&r->stat_target_us, 30000);
		}
		r->fade_frames = (int)(output_rate * LM_FADE_SECONDS);
		begin_prefill(r, written, false);
		if (!changed && same_epoch && !r->online)
			r->read_pos = resume_position; /* Reconnect cannot replay pre-stall/disabled audio. */
		r->online = true;
		lm_obs_store_release(&r->stat_epoch, epoch);
		lm_obs_store_release(&r->stat_resync, lm_obs_load_acquire(&r->stat_resync) + 1);
	}
	lm_obs_store_release(&r->stat_connected, 1);
	int64_t heartbeat = lm_obs_load_acquire(&h->heartbeat_qpc);
	double advance = writer_advance(r, written, heartbeat, now);
	if (!adopt_format(r, format_key(input_rate, output_rate))) {
		emit(r, out_l, out_r, NULL, frames);
		return;
	}
	lm_asrc *asrc = r->bank[(int)lm_obs_load_acquire(&r->active_bank)].state;
	int64_t reserved = lm_obs_load_acquire((const volatile int64_t *)h->reserved);
	if (written < r->read_pos || written - r->read_pos >= LM_OBS_CAPACITY_FRAMES ||
	    reserved - r->read_pos >= LM_OBS_CAPACITY_FRAMES) {
		lm_obs_store_release(&r->stat_over, lm_obs_load_acquire(&r->stat_over) + 1);
		lm_obs_store_release(&r->stat_resync, lm_obs_load_acquire(&r->stat_resync) + 1);
		begin_prefill(r, written, false);
	}
	if (!r->playing) {
		double available = (double)(written - r->read_pos);
		record_fill(r, available);
		if (available < ceil(r->target - lm_asrc_latency_input_frames(asrc))) {
			emit(r, out_l, out_r, NULL, frames);
			return;
		}
		r->read_pos = written - (int64_t)ceil(r->target - lm_asrc_latency_input_frames(asrc));
		lm_asrc_reset(asrc);
		lm_asrc_set_correction_ppm(asrc, 0.0);
		lm_drift_init(&r->drift, r->target, out_rate / frames);
		r->playing = true;
		r->playing_since = now;
		r->fade_in = 0;
	}
	double fill = (double)(written - r->read_pos) + advance + r->fifo_frames +
		      lm_asrc_latency_input_frames(asrc);
	record_fill(r, fill);
	double ppm = lm_drift_update(&r->drift, fill, frames / out_rate);
	lm_asrc_set_correction_ppm(asrc, ppm);
	lm_obs_store_release(&r->stat_ppm_milli, (int64_t)(ppm * 1000.0));
	int offset = 0;
	while (offset < frames) {
		if (!valid_header(h)) {
			begin_prefill(r, lm_obs_load_acquire(&h->write_frames), true);
			break;
		}
		int count = frames - offset;
		if (count > LM_OUTPUT_CHUNK)
			count = LM_OUTPUT_CHUNK;
		int32_t copied = lm_obs_read(h, view->pcm, epoch, &r->read_pos, r->fifo + r->fifo_frames * 2,
					    LM_OBS_CAPACITY_FRAMES - (uint32_t)r->fifo_frames);
		if (copied < 0) {
			if (lm_obs_load_acquire(&h->epoch) == epoch) {
				lm_obs_store_release(&r->stat_over, lm_obs_load_acquire(&r->stat_over) + 1);
				lm_obs_store_release(&r->stat_resync, lm_obs_load_acquire(&r->stat_resync) + 1);
			}
			begin_prefill(r, lm_obs_load_acquire(&h->write_frames), false);
			break;
		}
		r->fifo_frames += copied;
		int used = 0;
		int made = lm_asrc_process(asrc, r->fifo, r->fifo_frames, &used, r->output, count);
		r->fifo_frames -= used;
		memmove(r->fifo, r->fifo + used * 2, (size_t)r->fifo_frames * 2u * sizeof(float));
		/* An epoch/rate/send change during the copy or convolution invalidates
		 * this chunk, even when the local FIFO alone supplied its samples. */
		if (lm_obs_load_acquire(&h->epoch) != epoch || lm_obs_load_acquire(&h->sample_rate) != input_rate ||
		    !live_header(c, h, now)) {
			begin_prefill(r, lm_obs_load_acquire(&h->write_frames), true);
			break;
		}
		emit(r, out_l + offset, out_r + offset, r->output, made);
		offset += made;
		if (made < count) {
			lm_obs_store_release(&r->stat_under, lm_obs_load_acquire(&r->stat_under) + 1);
			lm_obs_store_release(&r->stat_resync, lm_obs_load_acquire(&r->stat_resync) + 1);
			grow_after_underrun(r, now);
			begin_prefill(r, lm_obs_load_acquire(&h->write_frames), true);
			break;
		}
	}
	if (offset < frames)
		emit(r, out_l + offset, out_r + offset, NULL, frames - offset);
	else
		check_headroom(r, fill, frames, now, heartbeat); /* Only a fully played pull establishes headroom. */
}

bool lm_receiver_connected(const lm_receiver *r)
{
	if (!r || !lm_obs_load_acquire(&r->stat_connected))
		return false;
	lm_view *view = current_view(r->connection);
	return view && live_header(r->connection, view->header, connection_now(r->connection));
}

void lm_receiver_get_stats(const lm_receiver *r, lm_receiver_stats *stats)
{
	memset(stats, 0, sizeof(*stats));
	if (!r)
		return;
	stats->fill_ms = (double)lm_obs_load_acquire(&r->stat_fill_us) / 1000.0;
	stats->target_ms = (double)lm_obs_load_acquire(&r->stat_target_us) / 1000.0;
	stats->ppm = (double)lm_obs_load_acquire(&r->stat_ppm_milli) / 1000.0;
	stats->underruns = (uint64_t)lm_obs_load_acquire(&r->stat_under);
	stats->overruns = (uint64_t)lm_obs_load_acquire(&r->stat_over);
	stats->resyncs = (uint64_t)lm_obs_load_acquire(&r->stat_resync);
	stats->epoch = lm_obs_load_acquire(&r->stat_epoch);
	stats->connected = lm_receiver_connected(r);
}
