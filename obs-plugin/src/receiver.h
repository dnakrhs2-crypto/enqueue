/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LIVEMIX_OBS_RECEIVER_H
#define LIVEMIX_OBS_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lm_connection lm_connection;
typedef struct lm_receiver lm_receiver;

enum lm_receiver_kind { LM_RECEIVER_SOURCE = 1, LM_RECEIVER_FILTER = 2 };
enum lm_log_level { LM_LOG_INFO, LM_LOG_WARNING };

typedef struct lm_connection_config {
	/* NULL selects the protocol's public name. A custom ring name with no
	 * readers_name gets a private "<ring_name>.Readers" mapping automatically. */
	const wchar_t *ring_name;
	const wchar_t *readers_name;
	/* Management thread only (or explicit poll in tests). Pass format directly
	 * to the host logger with message as its single %s argument: its alternating
	 * pointer is part of the contract, to defeat OBS's repeated-line filter. */
	void (*log)(void *context, enum lm_log_level level, const char *format, const char *message);
	void *log_context;
	/* Tests inject QPC ticks and their frequency. NULL uses the real QPC. */
	int64_t (*now)(void *context);
	void *clock_context;
	int64_t qpc_frequency;
	/* Optional host maintenance on the manager's one-second probe, outside receiver locks. */
	void (*probe)(void *context);
	void *probe_context;
} lm_connection_config;

typedef struct lm_receiver_stats {
	double fill_ms, target_ms, ppm;
	uint64_t underruns, overruns, resyncs;
	int64_t epoch;
	int input_rate, output_rate;
	bool connected;
} lm_receiver_stats;

/* One connection per module. All create/destroy/start operations are off the
 * audio thread. Destroy receivers before their connection, and stop their
 * audio callbacks before destroying a receiver. Published views live until
 * connection destruction, including while the writer is stopped/restarted. */
lm_connection *lm_connection_create(const lm_connection_config *config);
bool lm_connection_start(lm_connection *connection);
void lm_connection_destroy(lm_connection *connection);
/* Deterministic off-audio-thread management for tests. Do not call poll while
 * the background manager is running. It retries mappings/presence every 1 s
 * and prepares any requested ASRC format on each call. */
void lm_connection_poll(lm_connection *connection);

lm_receiver *lm_receiver_create(lm_connection *connection, enum lm_receiver_kind kind, double out_rate);
void lm_receiver_destroy(lm_receiver *receiver);
/* Single audio caller per instance. Planar stereo, exactly frames written.
 * No allocation, locks, logging or waits. Integer Hz rates 8000..384000 are
 * supported; a format change is silent until the manager prepares the ASRC.
 * Discontinuities use a bounded 10 ms tail/fade, then exact digital silence. */
void lm_receiver_pull(lm_receiver *receiver, float *out_l, float *out_r, int frames, double out_rate);
/* Call from the same single audio caller when output periods are dropped.
 * The next pull skips to the current writer minus target, clears ASRC history
 * and crossfades for <=10 ms. Same-epoch/rate clock learning is retained. */
void lm_receiver_resync(lm_receiver *receiver);
/* Pure in-place layout conversion. Mono averages L/R into left; other OBS
 * layouts keep stereo. Returns the number of planes to submit (1 or 2). */
size_t lm_mix_stereo_for_layout(float *left, const float *right, int frames, size_t channels);
/* Atomic diagnostic snapshots; safe from a UI/worker thread. */
void lm_receiver_get_stats(const lm_receiver *receiver, lm_receiver_stats *stats);
bool lm_receiver_connected(const lm_receiver *receiver);
/* Pure filter activity decision. Zero means filter_audio has never run. */
bool lm_filter_is_idle(int64_t last_audio, int64_t now, int64_t frequency);

#endif
