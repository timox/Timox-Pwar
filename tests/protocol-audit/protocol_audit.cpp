#include "pwar_router.h"
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <limits>

// Audit the actual C implementation, without sockets, drivers or PipeWire mocks.
// Exit nonzero when the required protocol invariant is violated.
static pwar_router_t router;
static pwar_packet_t packet(unsigned index, unsigned count, unsigned frames, uint64_t seq) {
    pwar_packet_t result{};
    result.packet_index = index; result.num_packets = count;
    result.n_samples = static_cast<uint16_t>(frames); result.seq = seq;
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned f = 0; f < frames; ++f)
            result.samples[c][f] = float(1000 * c + 100 * index + f + 1);
    return result;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    pwar_router_init(&router, 2);
    std::array<float, 512> output{};
    const std::string mode = argv[1];
    if (mode == "deterministic") {
        std::array<float, 96> mono{};
        for (unsigned i=0; i<96; ++i) mono[i] = float(i+1);
        pwar_packet_t a, b;
        std::memset(&a, 0xA5, sizeof(a)); std::memset(&b, 0x5A, sizeof(b));
        uint32_t count = 0;
        if (pwar_router_send_buffer(&router, 128, mono.data(), 96, 1, &a, 1, &count) <= 0 || count != 1) return 1;
        if (pwar_router_send_buffer(&router, 128, mono.data(), 96, 1, &b, 1, &count) <= 0) return 1;
        // Including padding: no previous stack/heap contents on the wire.
        if (std::memcmp(&a, &b, sizeof(a))) return 1;
        for (unsigned i=0; i<128; ++i)
            if (a.samples[0][i] != (i<96 ? mono[i] : 0.0f) || a.samples[1][i] != 0.0f) return 1;
        return 0;
    }
    if (mode == "sender_capacity") {
        std::array<float, 130> input{};
        std::array<pwar_packet_t, 65> packets{};
        uint32_t count = 123;
        // The sender must not generate more fragments than its peer can assemble.
        const auto before = packets;
        int result = pwar_router_send_buffer(&router, 1, input.data(), 65, 2, packets.data(), 65, &count);
        return result < 0 && count == 0 && std::memcmp(before.data(), packets.data(), sizeof(packets)) == 0 ? 0 : 1;
    }
    if (mode == "permutations") {
        // 320 frames = 128 + 128 + 64, all six orders, exact stereo payload.
        std::array<unsigned, 3> order{0, 1, 2};
        do {
            pwar_router_init(&router, 2);
            std::array<float, 640> actual{};
            for (unsigned arrival = 0; arrival < 3; ++arrival) {
                unsigned index = order[arrival];
                auto p = packet(index, 3, index == 2 ? 64 : 128, 10);
                int result = pwar_router_process_packet(&router, &p, actual.data(), 320, 2);
                if (result != (arrival == 2 ? 320 : 0)) return 1;
            }
            for (unsigned c = 0; c < 2; ++c)
                for (unsigned f = 0; f < 320; ++f) {
                    const unsigned index = f / 128, offset = f % 128;
                    if (actual[c * 320 + f] != float(1000*c + 100*index + offset + 1)) return 1;
                }
        } while (std::next_permutation(order.begin(), order.end()));
        return 0;
    }
    if (mode == "streaming") {
        // Linux wire sequence increments per packet; Windows groups two.
        // Exercise real startup zero, duplicate, and next complete block.
        for (uint64_t seq = 0; seq < 4; ++seq) {
            auto p = packet(0, 2, 64, seq);
            for (unsigned c = 0; c < 2; ++c)
                for (unsigned f = 0; f < 64; ++f) p.samples[c][f] = float(1000*c + seq*64 + f);
            int result = pwar_router_process_streaming_packet(&router, &p, output.data(), 128, 2);
            if (result != (seq % 2 ? 128 : 0)) return 1;
            if (seq % 2)
                for (unsigned c = 0; c < 2; ++c)
                    for (unsigned f = 0; f < 128; ++f)
                        if (output[c*128 + f] != float(1000*c + (seq-1)*64 + f)) return 1;
            auto duplicate = packet(0, 2, 64, seq);
            if (pwar_router_process_streaming_packet(&router, &duplicate, output.data(), 128, 2) != 0) return 1;
        }
        return 0;
    }
    if (mode == "wrap") {
        for (const uint64_t seq : {UINT64_MAX-1, UINT64_MAX, uint64_t{0}, uint64_t{1}}) {
            auto p = packet(0, 1, 64, seq);
            if (pwar_router_process_packet(&router, &p, output.data(), 128, 2) != 64) return 1;
        }
        auto old = packet(0, 1, 64, UINT64_MAX);
        return pwar_router_process_packet(&router, &old, output.data(), 128, 2) == 0 ? 0 : 1;
    }
    if (mode == "loss" || mode == "reset") {
        auto partial = packet(0, 2, 64, 100);
        if (pwar_router_process_packet(&router, &partial, output.data(), 128, 2) != 0) return 1;
        if (mode == "reset") pwar_router_init(&router, 2);
        auto complete = packet(0, 1, 64, mode == "reset" ? 0 : 102);
        if (pwar_router_process_packet(&router, &complete, output.data(), 128, 2) != 64) return 1;
        for (unsigned c=0; c<2; ++c)
            for (unsigned f=0; f<64; ++f)
                if (output[c*64+f] != complete.samples[c][f]) return 1;
        return 0;
    }
    if (mode == "dimensions") {
        auto p = packet(0, 2, 128, 10);
        if (pwar_router_process_packet(&router, &p, output.data(), 256, 2) != 0) return 1;
        const auto before = router;
        auto invalid = packet(1, 2, 128, 10);
        output.fill(17.0f);
        if (pwar_router_process_packet(&router, &invalid, output.data(), 192, 2) >= 0) return 1;
        if (std::memcmp(&before, &router, sizeof(router))) return 1;
        for (float f : output) if (f != 17.0f) return 1;
        return pwar_router_process_packet(&router, &invalid, output.data(), 256, 2) == 256 ? 0 : 1;
    }
    if (mode == "partial") {
        std::array<float, 384> input{};
        for (unsigned i = 0; i < input.size(); ++i) input[i] = float(i + 1);
        std::array<pwar_packet_t, 2> packets{};
        uint32_t count = 0;
        if (pwar_router_send_buffer(&router, 128, input.data(), 192, 2,
                packets.data(), 2, &count) <= 0 || count != 2) return 2;
        packets[0].seq = packets[1].seq = 10;
        const int first = pwar_router_process_packet(&router, &packets[0], output.data(), 256, 2);
        const int last = pwar_router_process_packet(&router, &packets[1], output.data(), 256, 2);
        const bool exact = first == 0 && last == 192 &&
            std::memcmp(input.data(), output.data(), sizeof(input)) == 0;
        std::cout << "192-frame roundtrip: returned=" << last << " exact=" << exact << '\n';
        return exact ? 0 : 1;
    }
    if (mode == "duplicate") {
        auto p = packet(0, 1, 128, 10);
        const int first = pwar_router_process_packet(&router, &p, output.data(), 256, 2);
        const int duplicate = pwar_router_process_packet(&router, &p, output.data(), 256, 2);
        std::cout << "delivery=" << first << " duplicate delivery=" << duplicate << '\n';
        return first == 128 && duplicate <= 0 ? 0 : 1;
    }
    if (mode == "inconsistent") {
        auto first = packet(0, 2, 128, 10);
        auto conflicting = packet(1, 3, 128, 10);
        pwar_router_process_packet(&router, &first, output.data(), 256, 2);
        const auto before = router;
        const int result = pwar_router_process_packet(&router, &conflicting, output.data(), 256, 2);
        const bool unchanged = std::memcmp(&before, &router, sizeof(router)) == 0;
        std::cout << "conflicting fragment count: result=" << result << " state preserved=" << unchanged << '\n';
        return result < 0 && unchanged ? 0 : 1;
    }
    if (mode == "stale") {
        auto fresh = packet(0, 2, 128, 20);
        auto stale = packet(0, 2, 128, 19);
        pwar_router_process_packet(&router, &fresh, output.data(), 256, 2);
        const auto before = router;
        const int result = pwar_router_process_packet(&router, &stale, output.data(), 256, 2);
        const bool unchanged = std::memcmp(&before, &router, sizeof(router)) == 0;
        std::cout << "late fragment: result=" << result << " current assembly preserved=" << unchanged << '\n';
        return result <= 0 && unchanged ? 0 : 1;
    }
    return 2;
}
