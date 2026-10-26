#ifndef LM_OBS_METADATA_H
#define LM_OBS_METADATA_H

#include <stddef.h>
#include <stdint.h>

/* Separate mapping: never extend or change lm_obs_ring_header. Windows x64 LE.
 * R0 freezes this ABI; R7 implements its single control-thread writer/readers. */
#define LM_OBS_PEER_RING_NAME L"Local\\LiveMix.ObsAudio.v1.Peer"
#define LM_OBS_PEER2_RING_NAME L"Local\\LiveMix.ObsAudio.v1.Peer2"
#define LM_OBS_PEER_METADATA_NAME L"Local\\LiveMix.ObsAudio.v1.Peer.Metadata"
#define LM_OBS_PEER2_METADATA_NAME L"Local\\LiveMix.ObsAudio.v1.Peer2.Metadata"
#define LM_OBS_METADATA_SUFFIX L".Metadata"
#define LM_OBS_METADATA_MAGIC 0x444d4d4cu /* LMMD */
#define LM_OBS_METADATA_VERSION 1u
#define LM_OBS_METADATA_BYTES 256u
#define LM_OBS_METADATA_NICK_BYTES 160u

enum lm_obs_participant_kind { LM_OBS_PARTICIPANT_NONE = 0, LM_OBS_PARTICIPANT_NATIVE = 1, LM_OBS_PARTICIPANT_WEB = 2 };
enum lm_obs_participant_state { LM_OBS_PARTICIPANT_EMPTY = 0, LM_OBS_PARTICIPANT_CONNECTING = 1,
                                LM_OBS_PARTICIPANT_CONNECTED = 2, LM_OBS_PARTICIPANT_DISCONNECTED = 3 };

typedef struct lm_obs_metadata {
    uint32_t magic;                 /* 0 */
    uint32_t version;               /* 4 */
    uint32_t mapping_bytes;         /* 8 */
    uint32_t slot;                  /* 12: 1 or 2, local to this LiveMix */
    volatile int64_t sequence;      /* 16: odd while writing, even when published */
    uint64_t session;               /* 24 */
    uint32_t participant_id;        /* 32: 0 for an empty slot */
    uint32_t stream_epoch;          /* 36 */
    uint32_t state;                 /* 40: lm_obs_participant_state */
    uint32_t kind;                  /* 44: lm_obs_participant_kind */
    uint32_t nickname_bytes;        /* 48: excludes terminating NUL */
    uint32_t writer_pid;            /* 52 */
    char nickname_utf8[161];        /* 56: at most 160 bytes plus NUL */
    uint8_t reserved[39];           /* 217: zero */
} lm_obs_metadata;

#ifdef __cplusplus
#define LM_METADATA_ASSERT static_assert
#else
#define LM_METADATA_ASSERT _Static_assert
#endif
LM_METADATA_ASSERT(sizeof(lm_obs_metadata) == LM_OBS_METADATA_BYTES, "metadata mapping ABI");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, sequence) == 16, "metadata sequence alignment");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, session) == 24, "metadata session offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, participant_id) == 32, "metadata participant offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, stream_epoch) == 36, "metadata epoch offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, state) == 40, "metadata state offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, kind) == 44, "metadata kind offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, nickname_bytes) == 48, "metadata name length offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, writer_pid) == 52, "metadata writer offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, nickname_utf8) == 56, "metadata name offset");
LM_METADATA_ASSERT(offsetof(lm_obs_metadata, reserved) == 217, "metadata reserved offset");
#undef LM_METADATA_ASSERT
#endif
