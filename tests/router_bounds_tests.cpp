#include "pwar_router.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>

#define CHECK(expression) do { if (!(expression)) { std::cerr << "line " << __LINE__ << ": " #expression "\n"; return 1; } } while (false)

int main() {
    static pwar_router_t router;
    std::array<float, 256> output{};
    pwar_packet_t packet{};
    pwar_router_init(&router, 2);
    packet.seq = 10;
    packet.num_packets = 1;
    packet.n_samples = 0;
    // First regression is safe to run against upstream: it accepts zero frames.
    CHECK(pwar_router_process_packet(&router, &packet, output.data(), 128, 2) < 0);

    for (unsigned kind = 0; kind < 6; ++kind) {
        pwar_router_init(&router, 2);
        packet = {};
        packet.seq = 20;
        packet.n_samples = 64;
        packet.num_packets = 1;
        if (kind == 0) packet.n_samples = 129;
        if (kind == 1) packet.num_packets = 65;
        if (kind == 2) { packet.num_packets = 64; packet.packet_index = 63; packet.n_samples = 128; }
        if (kind == 3) packet.packet_index = 1;
        if (kind == 4) { packet.num_packets = 0xffffffffu; packet.packet_index = 0xfffffffeu; }
        if (kind == 5) packet.num_packets = 0;
        const auto before = router;
        output.fill(17.0f);
        CHECK(pwar_router_process_packet(&router, &packet, output.data(), 128, 2) < 0);
        CHECK(std::memcmp(&before, &router, sizeof(router)) == 0);
        for (auto sample : output) CHECK(sample == 17.0f);
    }
    packet = {}; packet.n_samples = 64; packet.num_packets = 1;
    CHECK(pwar_router_process_packet(nullptr, &packet, output.data(), 128, 2) < 0);
    CHECK(pwar_router_process_streaming_packet(&router, nullptr, output.data(), 128, 2) < 0);
    CHECK(pwar_router_process_packet(&router, &packet, output.data(), 128, 17) < 0);

    std::array<pwar_packet_t, 2> packets{};
    std::array<float, 256> samples{};
    for (unsigned i = 0; i < 128; ++i) { samples[i] = float(i + 1); samples[128+i] = -float(i + 1); }
    uint32_t count = 99;
    CHECK(pwar_router_send_buffer(&router, 0, samples.data(), 128, 2, packets.data(), 2, &count) < 0);
    CHECK(count == 0);
    CHECK(pwar_router_send_buffer(&router, 129, samples.data(), 128, 2, packets.data(), 2, &count) < 0);
    CHECK(pwar_router_send_buffer(&router, 64, samples.data(), 128, 3, packets.data(), 2, &count) < 0);
    CHECK(pwar_router_send_buffer(&router, 64, samples.data(), std::numeric_limits<uint32_t>::max(), 2, packets.data(), 2, &count) < 0);
    CHECK(pwar_router_send_buffer(&router, 64, samples.data(), 128, 2, packets.data(), 1, &count) < 0);
    CHECK(count == 0);

    pwar_router_init(&router, 2);
    router.seq_timestamp = 42;
    CHECK(pwar_router_send_buffer(&router, 64, samples.data(), 128, 2, packets.data(), 2, &count) > 0);
    CHECK(count == 2);
    for (unsigned p = 0; p < count; ++p) {
        CHECK(packets[p].n_samples == 64 && packets[p].num_packets == 2 && packets[p].packet_index == p);
        CHECK(packets[p].seq_timestamp == 42);
        packets[p].seq = 50;
    }
    // Reverse arrival order must preserve both channels, amplitude and order.
    CHECK(pwar_router_process_packet(&router, &packets[1], output.data(), 128, 2) == 0);
    CHECK(pwar_router_process_packet(&router, &packets[0], output.data(), 128, 2) == 128);
    CHECK(output == samples);

    // Exercise the streaming wrapper as used by PWAR Windows, including sequence
    // progression and preserving the input packet when validation fails.
    pwar_router_init(&router, 2);
    packets[0].seq = 100; packets[1].seq = 101;
    CHECK(pwar_router_process_streaming_packet(&router, &packets[0], output.data(), 128, 2) == 0);
    CHECK(pwar_router_process_streaming_packet(&router, &packets[1], output.data(), 128, 2) == 128);
    CHECK(output == samples);
    packet = {}; packet.seq = 102; packet.num_packets = 2;
    const auto invalidBefore = packet;
    CHECK(pwar_router_process_streaming_packet(&router, &packet, output.data(), 128, 2) < 0);
    CHECK(std::memcmp(&packet, &invalidBefore, sizeof(packet)) == 0);

    // Largest legal assembly at both normal PWAR chunk sizes, with independent
    // channel values all the way to the last sample of the internal arrays.
    std::array<float, 8192> maximumSamples{}, maximumOutput{};
    std::array<pwar_packet_t, 64> maximumPackets{};
    for (unsigned i = 0; i < maximumSamples.size(); ++i) maximumSamples[i] = float(i + 1);
    for (const uint32_t chunk : {64u, 128u}) {
        pwar_router_init(&router, 2);
        CHECK(pwar_router_send_buffer(&router, chunk, maximumSamples.data(), 4096, 2,
              maximumPackets.data(), 64, &count) > 0);
        CHECK(count == 4096 / chunk);
        for (uint32_t p = 0; p < count; ++p) {
            maximumPackets[p].seq = 200;
            const auto result = pwar_router_process_packet(&router, &maximumPackets[p], maximumOutput.data(), 4096, 2);
            CHECK(result == (p + 1 == count ? 4096 : 0));
        }
        CHECK(maximumOutput == maximumSamples);
    }
    std::cout << "PWAR router bounds and stereo round-trip passed\n";
}
