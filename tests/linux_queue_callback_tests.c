#include "pwar_audio_queue.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_BUFFER_SIZE 4096
#define NUM_CHANNELS 2
struct spa_io_position { struct { uint64_t duration; } clock; };
struct data {
    void *in_port, *left_out_port, *right_out_port;
    bool passthrough_test;
    atomic_uint rt_xruns;
    pwar_audio_queue linux_to_windows, windows_to_linux;
};
static unsigned requests;
static float *pw_filter_get_dsp_buffer(void *port, uint32_t frames) {
    (void)frames; ++requests; return port;
}
#include "linux_callback.inc"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static struct data state;
static float input[MAX_BUFFER_SIZE+3], left[MAX_BUFFER_SIZE+3], right[MAX_BUFFER_SIZE+3];
static float stereo[MAX_BUFFER_SIZE*2], sent[MAX_BUFFER_SIZE*2];
static void reset(void) {
    pwar_audio_queue_init(&state.linux_to_windows);
    pwar_audio_queue_init(&state.windows_to_linux);
    atomic_init(&state.rt_xruns,0);
    state.in_port=input+1; state.left_out_port=left+1; state.right_out_port=right+1;
    state.passthrough_test=false; requests=0;
    for(unsigned i=0;i<MAX_BUFFER_SIZE+3;++i) { input[i]=.25f; left[i]=right[i]=9; }
    for(unsigned i=0;i<MAX_BUFFER_SIZE;++i) { stereo[2*i]=.125f; stereo[2*i+1]=-.5f; }
}
int main(void) {
    const unsigned sizes[]={1,64,128,129,512,1024,4096};
    struct spa_io_position position;
    for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);++s) {
        unsigned frames=sizes[s]; reset(); position.clock.duration=frames;
        CHECK(pwar_audio_queue_write(&state.windows_to_linux,stereo,frames)==frames);
        on_process(&state,&position);
        CHECK(atomic_load(&state.rt_xruns)==0);
        CHECK(pwar_audio_queue_read(&state.linux_to_windows,sent,frames)==frames);
        for(unsigned i=0;i<frames;++i) {
            CHECK(sent[2*i]==.25f && sent[2*i+1]==.25f);
            CHECK(left[i+1]==.125f && right[i+1]==-.5f);
        }
        CHECK(left[0]==9 && right[0]==9 && left[frames+1]==9 && right[frames+1]==9);
    }
    reset(); position.clock.duration=128; state.in_port=NULL;
    on_process(&state,&position);
    CHECK(atomic_load(&state.rt_xruns)==2); /* missing capture and empty return */
    CHECK(pwar_audio_queue_read(&state.linux_to_windows,sent,128)==128);
    for(unsigned i=0;i<128;++i) CHECK(sent[2*i]==0 && sent[2*i+1]==0 && left[i+1]==0 && right[i+1]==0);
    reset(); state.passthrough_test=true; position.clock.duration=512;
    on_process(&state,&position);
    for(unsigned i=1;i<=512;++i) CHECK(left[i]==.25f && right[i]==.25f);
    CHECK(pwar_audio_queue_available(&state.linux_to_windows)==0);
    reset(); position.clock.duration=MAX_BUFFER_SIZE+1;
    on_process(&state,&position);
    CHECK(atomic_load(&state.rt_xruns)==1 && left[MAX_BUFFER_SIZE+2]==9);
    for(unsigned i=1;i<=MAX_BUFFER_SIZE+1;++i) CHECK(left[i]==0 && right[i]==0);
    reset(); position.clock.duration=128;
    CHECK(pwar_audio_queue_write(&state.linux_to_windows,stereo,MAX_BUFFER_SIZE)==MAX_BUFFER_SIZE);
    on_process(&state,&position);
    CHECK(atomic_load(&state.rt_xruns)==2 && pwar_audio_queue_available(&state.linux_to_windows)==MAX_BUFFER_SIZE);
    reset(); state.left_out_port=state.right_out_port=NULL;
    on_process(&state,&position); CHECK(requests==3);
    reset(); position.clock.duration=0; on_process(&state,&position); CHECK(requests==0);
    position.clock.duration=(uint64_t)UINT32_MAX+1; on_process(&state,&position);
    CHECK(requests==0 && atomic_load(&state.rt_xruns)==1);
    on_process(&state,NULL); on_process(NULL,&position); CHECK(requests==0);
    puts("Actual queued Linux callback: variable quanta, stereo, silence, overflow and bounds passed");
    return 0;
}
