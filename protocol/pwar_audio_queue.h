#ifndef PWAR_AUDIO_QUEUE_H
#define PWAR_AUDIO_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PWAR_AUDIO_QUEUE_CHANNELS 2u
#define PWAR_AUDIO_QUEUE_FRAMES 4096u

/*
 * Bounded SPSC interleaved-stereo queue. read_frame/write_frame count stored
 * frames; the timeline cursor also includes explicitly skipped frames so a
 * consumer can preserve duration by emitting silence at the right position.
 * Initialize before starting either thread. No reset is allowed while active.
 */
typedef struct pwar_audio_queue {
    float samples[PWAR_AUDIO_QUEUE_FRAMES * PWAR_AUDIO_QUEUE_CHANNELS];
    uint32_t frame_timeline[PWAR_AUDIO_QUEUE_FRAMES];
    atomic_uint read_frame;
    atomic_uint write_frame;
    atomic_uint published_timeline;
    atomic_uint consumed_timeline;
    atomic_uint skipped_frames;
    uint32_t producer_timeline;
    uint32_t consumer_timeline;
    /* Consumer-owned count; read only on the same thread as queue_read. */
    uint64_t synthesized_frames;
} pwar_audio_queue;

void pwar_audio_queue_init(pwar_audio_queue *queue);

/* Returns the number of stored frames, or zero if the entire write did not fit. */
uint32_t pwar_audio_queue_write(pwar_audio_queue *queue,
                                const float *interleaved_stereo,
                                uint32_t frames);

/*
 * Returns frames copied plus any already-known timeline gap filled with zero.
 * It never waits. Requests larger than the ring capacity return zero.
 */
uint32_t pwar_audio_queue_read(pwar_audio_queue *queue,
                               float *interleaved_stereo,
                               uint32_t frames);

/* Mark frames lost by the producer without blocking or overwriting old data. */
void pwar_audio_queue_skip_write(pwar_audio_queue *queue, uint32_t frames);

/* Advance a clocked consumer over frames it had to zero-fill on underrun. */
void pwar_audio_queue_skip_read(pwar_audio_queue *queue, uint32_t frames);

/* Number of stored (not synthesized) frames currently queued. */
uint32_t pwar_audio_queue_available(const pwar_audio_queue *queue);

#ifdef __cplusplus
}
#endif

#endif
