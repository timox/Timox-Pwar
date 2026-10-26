#include "pwar_audio_queue.h"

#include <stddef.h>
#include <string.h>

#define QUEUE_MASK (PWAR_AUDIO_QUEUE_FRAMES - 1u)

_Static_assert((PWAR_AUDIO_QUEUE_FRAMES & QUEUE_MASK) == 0,
               "PWAR_AUDIO_QUEUE_FRAMES must be a power of two");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "PWAR callback queues require lock-free unsigned atomics");

void pwar_audio_queue_init(pwar_audio_queue *queue) {
    if (!queue) return;
    memset(queue->samples, 0, sizeof(queue->samples));
    memset(queue->frame_timeline, 0, sizeof(queue->frame_timeline));
    atomic_init(&queue->read_frame, 0u);
    atomic_init(&queue->write_frame, 0u);
    atomic_init(&queue->published_timeline, 0u);
    atomic_init(&queue->consumed_timeline, 0u);
    atomic_init(&queue->skipped_frames, 0u);
    queue->producer_timeline = 0u;
    queue->consumer_timeline = 0u;
    queue->synthesized_frames = 0u;
}

uint32_t pwar_audio_queue_available(const pwar_audio_queue *queue) {
    if (!queue) return 0u;
    const uint32_t write = atomic_load_explicit(&queue->write_frame, memory_order_acquire);
    const uint32_t read = atomic_load_explicit(&queue->read_frame, memory_order_acquire);
    const uint32_t available = write - read;
    return available <= PWAR_AUDIO_QUEUE_FRAMES ? available : 0u;
}

uint32_t pwar_audio_queue_write(pwar_audio_queue *queue,
                                const float *interleaved_stereo,
                                uint32_t frames) {
    if (!queue || !interleaved_stereo || !frames || frames > PWAR_AUDIO_QUEUE_FRAMES)
        return 0u;

    const uint32_t consumed = atomic_load_explicit(&queue->consumed_timeline,
                                                    memory_order_acquire);
    if ((int32_t)(consumed - queue->producer_timeline) > 0)
        queue->producer_timeline = consumed;

    const uint32_t write = atomic_load_explicit(&queue->write_frame, memory_order_relaxed);
    const uint32_t read = atomic_load_explicit(&queue->read_frame, memory_order_acquire);
    const uint32_t used = write - read;
    if (used > PWAR_AUDIO_QUEUE_FRAMES || frames > PWAR_AUDIO_QUEUE_FRAMES - used)
        return 0u;

    for (uint32_t i = 0; i < frames; ++i) {
        const uint32_t slot = (write + i) & QUEUE_MASK;
        queue->samples[slot * PWAR_AUDIO_QUEUE_CHANNELS] =
            interleaved_stereo[i * PWAR_AUDIO_QUEUE_CHANNELS];
        queue->samples[slot * PWAR_AUDIO_QUEUE_CHANNELS + 1u] =
            interleaved_stereo[i * PWAR_AUDIO_QUEUE_CHANNELS + 1u];
        queue->frame_timeline[slot] = queue->producer_timeline + i;
    }

    queue->producer_timeline += frames;
    atomic_store_explicit(&queue->write_frame, write + frames, memory_order_release);
    atomic_store_explicit(&queue->published_timeline, queue->producer_timeline,
                          memory_order_release);
    return frames;
}

void pwar_audio_queue_skip_write(pwar_audio_queue *queue, uint32_t frames) {
    if (!queue || !frames) return;
    const uint32_t consumed = atomic_load_explicit(&queue->consumed_timeline,
                                                    memory_order_acquire);
    if ((int32_t)(consumed - queue->producer_timeline) > 0)
        queue->producer_timeline = consumed;
    queue->producer_timeline += frames;
    atomic_fetch_add_explicit(&queue->skipped_frames, frames, memory_order_relaxed);
    atomic_store_explicit(&queue->published_timeline, queue->producer_timeline,
                          memory_order_release);
}

uint32_t pwar_audio_queue_read(pwar_audio_queue *queue,
                               float *interleaved_stereo,
                               uint32_t frames) {
    if (!queue || !interleaved_stereo || !frames ||
        frames > PWAR_AUDIO_QUEUE_FRAMES) return 0u;

    uint32_t read = atomic_load_explicit(&queue->read_frame, memory_order_relaxed);
    const uint32_t published = atomic_load_explicit(&queue->published_timeline,
                                                     memory_order_acquire);
    /*
     * A successful producer write publishes write_frame before the timeline.
     * Load write_frame after the acquire on published_timeline so seeing a
     * newer timeline can never make newly queued samples look like a gap.
     */
    const uint32_t write = atomic_load_explicit(&queue->write_frame, memory_order_acquire);
    uint32_t available = write - read;
    if (available > PWAR_AUDIO_QUEUE_FRAMES) return 0u;

    uint32_t produced = 0u;
    while (produced < frames) {
        if (available) {
            const uint32_t slot = read & QUEUE_MASK;
            const uint32_t frame = queue->frame_timeline[slot];
            const int32_t distance = (int32_t)(frame - queue->consumer_timeline);
            if (distance > 0) {
                uint32_t gap = (uint32_t)distance;
                if (gap > frames - produced) gap = frames - produced;
                memset(interleaved_stereo + produced * PWAR_AUDIO_QUEUE_CHANNELS, 0,
                       gap * PWAR_AUDIO_QUEUE_CHANNELS * sizeof(float));
                queue->consumer_timeline += gap;
                queue->synthesized_frames += gap;
                produced += gap;
                continue;
            }
            if (distance < 0) {
                ++read;
                --available;
                continue;
            }

            uint32_t count = available;
            if (count > frames - produced) count = frames - produced;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t src = (read + i) & QUEUE_MASK;
                const uint32_t expected = queue->consumer_timeline + i;
                if (queue->frame_timeline[src] != expected) {
                    count = i;
                    break;
                }
                interleaved_stereo[(produced + i) * PWAR_AUDIO_QUEUE_CHANNELS] =
                    queue->samples[src * PWAR_AUDIO_QUEUE_CHANNELS];
                interleaved_stereo[(produced + i) * PWAR_AUDIO_QUEUE_CHANNELS + 1u] =
                    queue->samples[src * PWAR_AUDIO_QUEUE_CHANNELS + 1u];
            }
            if (!count) continue;
            read += count;
            available -= count;
            queue->consumer_timeline += count;
            produced += count;
            continue;
        }

        const uint32_t gap = published - queue->consumer_timeline;
        if (!gap || gap >= UINT32_C(0x80000000)) break;
        uint32_t count = gap;
        if (count > frames - produced) count = frames - produced;
        memset(interleaved_stereo + produced * PWAR_AUDIO_QUEUE_CHANNELS, 0,
               count * PWAR_AUDIO_QUEUE_CHANNELS * sizeof(float));
        queue->consumer_timeline += count;
        queue->synthesized_frames += count;
        produced += count;
    }

    atomic_store_explicit(&queue->read_frame, read, memory_order_release);
    atomic_store_explicit(&queue->consumed_timeline, queue->consumer_timeline,
                          memory_order_release);
    return produced;
}

void pwar_audio_queue_skip_read(pwar_audio_queue *queue, uint32_t frames) {
    if (!queue || !frames) return;
    queue->consumer_timeline += frames;
    atomic_store_explicit(&queue->consumed_timeline, queue->consumer_timeline,
                          memory_order_release);
}
