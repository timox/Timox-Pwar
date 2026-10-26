/*
 * pwar_router.c - PipeWire <-> UDP streaming bridge for PWAR
 *
 * (c) 2025 Philip K. Gisslow
 * This file is part of the PipeWire ASIO Relay (PWAR) project.
 */

#include "pwar_router.h"
#include "pwar_packet.h"
#include <string.h>


void pwar_router_init(pwar_router_t *router, uint32_t channel_count) {
    if (!router) return;
    memset(router, 0, sizeof(*router));
    router->channel_count = channel_count;
}

static int valid_packet(const pwar_router_t *router, const pwar_packet_t *packet,
                        const float *output, uint32_t max_samples, uint32_t channels) {
    if (!router || !packet || !output || !max_samples || !channels ||
        channels > PWAR_CHANNELS || !router->channel_count ||
        router->channel_count > PWAR_CHANNELS || channels != router->channel_count) return 0;
    if (!packet->n_samples || packet->n_samples > PWAR_PACKET_MAX_CHUNK_SIZE ||
        !packet->num_packets ||
        packet->num_packets > sizeof(router->packet_received) / sizeof(router->packet_received[0]) ||
        packet->packet_index >= packet->num_packets) return 0;
    return 1;
}

int pwar_router_process_streaming_packet(pwar_router_t *router, pwar_packet_t *input_packet,
        float *output_buffers, uint32_t max_samples, uint32_t channel_count) {
    if (!router || !input_packet) return -1;
    pwar_packet_t packet = *input_packet;
    packet.packet_index = 0;
    if ((uint64_t)packet.num_packets * packet.n_samples > max_samples ||
        (uint64_t)packet.num_packets * packet.n_samples > PWAR_ROUTER_MAX_BUFFER_SIZE)
        return -2;
    if (router->active) {
        const uint64_t index = packet.seq - router->current_seq;
        if (index < router->expected_packets) {
            packet.packet_index = (uint32_t)index;
            packet.seq = router->current_seq;
        }
    }
    if (!valid_packet(router, &packet, output_buffers, max_samples, channel_count)) return -2;
    if (router->active && packet.seq == router->current_seq) {
        if (packet.num_packets != router->expected_packets) return -2;
        for (uint32_t i = 0; i < packet.packet_index; ++i) {
            if (router->packet_received[i]) continue;
            const uint32_t slot = i * PWAR_PACKET_MAX_CHUNK_SIZE;
            for (uint32_t channel = 0; channel < channel_count; ++channel)
                memset(router->buffers[channel] + slot, 0,
                       packet.n_samples * sizeof(float));
            router->fragment_frames[i] = packet.n_samples;
            router->packet_received[i] = 1;
            ++router->received_packets;
            ++router->synthesized_packets;
        }
    }
    const int result = pwar_router_process_packet(router, &packet, output_buffers,
                                                 max_samples, channel_count);
    if (result >= 0) *input_packet = packet;
    return result;
}

int pwar_router_process_packet(pwar_router_t *router, pwar_packet_t *packet,
        float *output_buffers, uint32_t max_samples, uint32_t channel_count) {
    if (!valid_packet(router, packet, output_buffers, max_samples, channel_count)) return -2;
    const int same = router->active && packet->seq == router->current_seq;
    if (router->active && !same) {
        // Serial-number arithmetic: stale/ambiguous half-range packets cannot
        // replace the current assembly. A peer restart requires explicit init.
        const uint64_t delta = packet->seq - router->current_seq;
        if (delta >= (UINT64_C(1) << 63)) return 0;
    }
    if (same && packet->num_packets != router->expected_packets) return -2;
    if (same && (router->delivered || router->packet_received[packet->packet_index])) return 0;

    const uint32_t last = packet->num_packets - 1;
    uint32_t chunk = same ? router->chunk_frames : 0;
    uint32_t tail = same ? router->fragment_frames[last] : 0;
    if (packet->packet_index != last) {
        if (chunk && chunk != packet->n_samples) return -2;
        chunk = packet->n_samples;
    } else {
        tail = packet->n_samples;
    }
    if (chunk && tail > chunk) return -2;
    // Validate dimensions before changing state. With an unknown tail, at
    // least one frame must still fit. With only the tail, full chunks must
    // be at least as large as that tail.
    const uint32_t minimum = last * (chunk ? chunk : tail) + (tail ? tail : 1);
    if (minimum > PWAR_ROUTER_MAX_BUFFER_SIZE || minimum > max_samples) return -2;

    if (!same) {
        router->current_seq = packet->seq;
        router->seq_timestamp = packet->seq_timestamp;
        router->received_packets = 0;
        router->expected_packets = packet->num_packets;
        router->active = 1;
        router->delivered = 0;
        memset(router->packet_received, 0, sizeof(router->packet_received));
        memset(router->fragment_frames, 0, sizeof(router->fragment_frames));
    }
    router->chunk_frames = chunk;
    const uint32_t slot = packet->packet_index * PWAR_PACKET_MAX_CHUNK_SIZE;
    for (uint32_t ch = 0; ch < channel_count; ++ch)
        memcpy(router->buffers[ch] + slot, packet->samples[ch], packet->n_samples * sizeof(float));
    router->fragment_frames[packet->packet_index] = packet->n_samples;
    router->packet_received[packet->packet_index] = 1;
    ++router->received_packets;
    if (router->received_packets != router->expected_packets) return 0;

    const uint32_t frames = last * chunk + tail;
    for (uint32_t ch = 0; ch < channel_count; ++ch) {
        uint32_t offset = 0;
        for (uint32_t i = 0; i < router->expected_packets; ++i) {
            memcpy(output_buffers + ch * frames + offset,
                   router->buffers[ch] + i * PWAR_PACKET_MAX_CHUNK_SIZE,
                   router->fragment_frames[i] * sizeof(float));
            offset += router->fragment_frames[i];
        }
    }
    router->delivered = 1;
    return (int)frames;
}


// Returns 0 on success, -1 if not enough space in packets array, -2 if invalid arguments
int pwar_router_send_buffer(pwar_router_t *router, uint32_t chunk_size, float *samples, uint32_t n_samples, uint32_t channel_count, pwar_packet_t *packets, const uint32_t packet_count, uint32_t *packets_to_send) {
    if (!packets_to_send) return -2;
    *packets_to_send = 0;
    if (!router || !samples || !packets || !channel_count || channel_count > PWAR_CHANNELS ||
        !n_samples || n_samples > PWAR_ROUTER_MAX_BUFFER_SIZE ||
        !chunk_size || chunk_size > PWAR_PACKET_MAX_CHUNK_SIZE) return -2;
    uint32_t total_packets = n_samples / chunk_size + (n_samples % chunk_size != 0);
    if (total_packets > sizeof(router->packet_received) / sizeof(router->packet_received[0])) return -2;
    if (total_packets > packet_count) {
        *packets_to_send = 0;
        return -1; // Not enough space in packets array
    }
    for (uint32_t p = 0; p < total_packets; ++p) {
        uint32_t start = p * chunk_size;
        uint32_t ns = (n_samples - start > chunk_size) ? chunk_size : (n_samples - start);
        // The complete struct is transmitted, including padding and unused
        // channels/samples. Never retain bytes from a previous packet.
        memset(&packets[p], 0, sizeof(packets[p]));
        packets[p].packet_index = p;
        packets[p].num_packets = total_packets;
        packets[p].n_samples = (uint16_t)ns;
        packets[p].seq_timestamp = router->seq_timestamp;
        for (uint32_t ch = 0; ch < channel_count; ++ch) {
            memcpy(packets[p].samples[ch], &samples[ch * n_samples + start], ns * sizeof(float));
        }
    }
    *packets_to_send = total_packets;
    return 1;
}

