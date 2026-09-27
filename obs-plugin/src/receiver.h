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
	void (*log)(void *context, enum lm_log_level level, const char *message);
	void *log_context;
	/* Tests inject QPC ticks and their frequency. NULL uses the real QPC. */
	int64_t (*now)(void *context);
	void *clock_context;
	int64_t qpc_frequency;
} lm_connection_config;

typedef struct lm_receiver_stats {
	double fill_ms, target_ms, ppm;
	uint64_t underruns, overruns, resyncs;
	int64_t epoch;
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
/* Atomic diagnostic snapshots; safe from a UI/worker thread. */
void lm_receiver_get_stats(const lm_receiver *receiver, lm_receiver_stats *stats);
bool lm_receiver_connected(const lm_receiver *receiver);

#endif
