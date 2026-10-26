#include <pwar_audio_queue.h> /* Header from PWAR_SOURCE_DIR, not the local reference copy. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static pwar_audio_queue queue;
static void *producer(void *unused) {
    (void)unused;
    unsigned frame=0;
    float samples[2*97];
    while(frame<500000) {
        unsigned count=500000-frame;
        if(count>97) count=97;
        for(unsigned i=0;i<count;++i) { samples[2*i]=(float)(frame+i); samples[2*i+1]=-(float)(frame+i); }
        unsigned written=pwar_audio_queue_write(&queue,samples,count);
        frame+=written;
        if(!written) sched_yield();
    }
    return NULL;
}
int main(void) {
    pwar_audio_queue_init(&queue);
    float input[2*4096], output[2*4096];
    for(unsigned i=0;i<4096;++i) { input[2*i]=(float)i; input[2*i+1]=-(float)i; }
    CHECK(pwar_audio_queue_write(&queue,input,1024)==1024);
    CHECK(pwar_audio_queue_read(&queue,output,96)==96);
    CHECK(memcmp(output,input,96*2*sizeof(float))==0);
    CHECK(pwar_audio_queue_read(&queue,output,1024)==928);
    CHECK(memcmp(output,input+192,928*2*sizeof(float))==0);
    CHECK(pwar_audio_queue_available(&queue)==0);
    CHECK(pwar_audio_queue_write(&queue,NULL,1)==0);
    CHECK(pwar_audio_queue_read(&queue,NULL,1)==0);
    CHECK(pwar_audio_queue_write(&queue,input,PWAR_AUDIO_QUEUE_FRAMES+1)==0);
    // Full queue and backpressure: old unread samples must survive overflow.
    for(unsigned i=0;i<PWAR_AUDIO_QUEUE_FRAMES/4096;++i)
        CHECK(pwar_audio_queue_write(&queue,input,4096)==4096);
    CHECK(pwar_audio_queue_write(&queue,input,1)==0);
    for(unsigned i=0;i<PWAR_AUDIO_QUEUE_FRAMES/4096;++i) {
        CHECK(pwar_audio_queue_read(&queue,output,4096)==4096);
        CHECK(memcmp(output,input,sizeof(input))==0);
    }
    // Set cursors only while quiescent; exercise unsigned wraparound.
    atomic_store(&queue.read_frame,UINT32_MAX-64u);
    atomic_store(&queue.write_frame,UINT32_MAX-64u);
    CHECK(pwar_audio_queue_write(&queue,input,1024)==1024);
    CHECK(pwar_audio_queue_read(&queue,output,1024)==1024);
    CHECK(memcmp(input,output,2048*sizeof(float))==0);
    pwar_audio_queue_init(&queue);
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,producer,NULL)==0);
    unsigned frame=0;
    while(frame<500000) {
        unsigned count=pwar_audio_queue_read(&queue,output,113);
        for(unsigned i=0;i<count;++i) {
            CHECK(output[2*i]==(float)(frame+i));
            CHECK(output[2*i+1]==-(float)(frame+i));
        }
        frame+=count;
        if(!count) sched_yield();
    }
    CHECK(pthread_join(thread,NULL)==0);
    CHECK(pwar_audio_queue_available(&queue)==0);
    puts("SPSC: variable quanta, bounds, backpressure, wrap and 500000 concurrent stereo frames passed");
    // Returned frames include synthesized gaps, but those are not copied audio.
    pwar_audio_queue_init(&queue);
    CHECK(pwar_audio_queue_write(&queue,input,2)==2);
    pwar_audio_queue_skip_write(&queue,3);
    CHECK(pwar_audio_queue_write(&queue,input+4,2)==2);
    CHECK(pwar_audio_queue_read(&queue,output,7)==7);
    CHECK(queue.synthesized_frames==3);
    CHECK(memcmp(output,input,4*sizeof(float))==0);
    for(unsigned f=2;f<5;++f) CHECK(output[2*f]==0 && output[2*f+1]==0);
    CHECK(memcmp(output+10,input+4,4*sizeof(float))==0);
    pwar_audio_queue_skip_write(&queue,4);
    CHECK(pwar_audio_queue_read(&queue,output,4)==4 && queue.synthesized_frames==7);
    CHECK(pwar_audio_queue_read(&queue,output,4)==0 && queue.synthesized_frames==7);
    return 0;
}
