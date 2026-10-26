#include "pwar_packet.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

struct spa_io_position { struct { uint64_t duration; } clock; };
struct data { void* in_port; void* left_out_port; void* right_out_port; bool passthrough_test; bool oneshot_mode; };
static unsigned dispatched = 0, xruns = 0, requests = 0;
static bool lastOneshot = false;
static float* pw_filter_get_dsp_buffer(void* port, uint32_t) { ++requests; return static_cast<float*>(port); }
static void latency_manager_report_xrun() { ++xruns; }
static void process_one_shot(void*, float*, uint32_t, float*, float*) { ++dispatched; lastOneshot = true; }
static void process_ping_pong(void*, float*, uint32_t, float*, float*) { ++dispatched; lastOneshot = false; }
#include "linux_callback.inc"

#define CHECK(x) do { if (!(x)) { std::cerr << "Line " << __LINE__ << ": " #x "\n"; return 1; } } while(false)
int main() {
    std::array<float, 514> input{}, left{}, right{};
    input.fill(0.25f);
    auto reset = [&] { left.fill(9); right.fill(9); dispatched = xruns = requests = 0; };
    data state{input.data()+1, left.data()+1, right.data()+1, false, false};
    spa_io_position position{{512}};
    // Real callback must never send a 512-frame quantum to a 128-frame packet.
    reset(); on_process(&state, &position);
    CHECK(dispatched == 0);
    CHECK(xruns == 1);
    for(unsigned i=1;i<=512;++i) CHECK(left[i] == 0 && right[i] == 0);
    CHECK(left.front()==9 && left.back()==9 && right.front()==9 && right.back()==9);
    for(bool oneshot : {false,true}) {
        state.oneshot_mode=oneshot;
        for(unsigned frames : {129u,512u}) {
            position.clock.duration=frames; reset(); on_process(&state,&position);
            CHECK(dispatched==0 && xruns==1);
            CHECK(requests==2); // Oversized network blocks only acquire outputs for silence.
            for(unsigned i=1;i<=frames;++i) CHECK(left[i]==0 && right[i]==0);
            CHECK(left[frames+1]==9 && right[frames+1]==9);
        }
        for(unsigned frames : {64u,128u}) {
            position.clock.duration=frames; reset(); on_process(&state,&position);
            CHECK(dispatched==1 && lastOneshot==oneshot && xruns==0);
        }
    }
    for(bool passthrough : {false,true}) {
        state.passthrough_test=passthrough; state.in_port=nullptr;
        position.clock.duration=128; reset(); on_process(&state,&position);
        CHECK(dispatched==0 && xruns==1);
        for(unsigned i=1;i<=128;++i) CHECK(left[i]==0 && right[i]==0);
        CHECK(left[129]==9 && right[129]==9);
    }
    state.in_port=input.data()+1; state.passthrough_test=true;
    position.clock.duration=512; reset(); on_process(&state,&position);
    CHECK(dispatched==0 && xruns==0);
    for(unsigned i=1;i<=512;++i) CHECK(left[i]==0.25f && right[i]==0.25f);
    state.passthrough_test=false; state.left_out_port=nullptr; state.right_out_port=nullptr;
    position.clock.duration=512; reset(); on_process(&state,&position); CHECK(dispatched==0);
    position.clock.duration=0; reset(); on_process(&state,&position); CHECK(dispatched==0);
    position.clock.duration=uint64_t(UINT32_MAX)+1; reset(); on_process(&state,&position);
    CHECK(dispatched==0 && requests==0);
    reset(); on_process(&state,nullptr); CHECK(dispatched==0 && requests==0);
    std::cout << "Linux callback bounds, null input, passthrough and valid dispatch passed\n";
}
