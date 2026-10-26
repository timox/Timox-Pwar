/* PWAR stream backend: fixed wire format, PipeWire adapters, bounded RT queues.
 * GPL-3.0, like the surrounding PWAR project.
 */
#define _GNU_SOURCE
#include "libpwar.h"
#include "../protocol/pwar_audio_queue.h" /* supplied by prerequisite patch 0011 */
#include "../protocol/pwar_router.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <unistd.h>

enum { WIRE_RATE=48000, WIRE_FRAMES=128, STRIDE=2*sizeof(float) };
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"Counters must be lock free");
struct relay;
struct endpoint { struct relay *relay; struct pw_stream *stream; atomic_bool valid; int capture; };
struct relay {
    struct pw_main_loop *loop;
    struct endpoint capture, playback;
    struct spa_source *reporter;
    int socket_fd, passthrough, worker_started;
    struct sockaddr_in peer;
    pthread_t worker;
    atomic_bool stop, failed;
    atomic_ullong captured, played, transmitted, received, invalid, tx_dropped, rx_dropped, silenced;
    atomic_ullong capture_callbacks, capture_quantum_frames, capture_quantum_min, capture_quantum_max;
    atomic_ullong playback_callbacks, playback_quantum_frames, playback_quantum_min, playback_quantum_max;
    atomic_ullong tx_queue_high_water, rx_queue_high_water, tx_queue_overflow_events, rx_queue_overflow_events;
    pwar_audio_queue tx, rx;
    pwar_router_t router;
};

static void observe_quantum(atomic_ullong *total,atomic_ullong *minimum,
                            atomic_ullong *maximum,uint64_t frames) {
    atomic_fetch_add_explicit(total,frames,memory_order_relaxed);
    uint64_t old=atomic_load_explicit(minimum,memory_order_relaxed);
    while((old==0 || frames<old) &&
          !atomic_compare_exchange_weak_explicit(minimum,&old,frames,
              memory_order_relaxed,memory_order_relaxed)) {}
    old=atomic_load_explicit(maximum,memory_order_relaxed);
    while(frames>old &&
          !atomic_compare_exchange_weak_explicit(maximum,&old,frames,
              memory_order_relaxed,memory_order_relaxed)) {}
}

static void observe_high_water(atomic_ullong *high_water,uint32_t frames) {
    uint64_t old=atomic_load_explicit(high_water,memory_order_relaxed);
    while(frames>old &&
          !atomic_compare_exchange_weak_explicit(high_water,&old,frames,
              memory_order_relaxed,memory_order_relaxed)) {}
}

static void mark_capture_loss(struct relay *r,const struct pw_buffer *b) {
    uint64_t requested=b->requested;
    observe_quantum(&r->capture_quantum_frames,&r->capture_quantum_min,
                    &r->capture_quantum_max,requested);
    atomic_fetch_add_explicit(&r->invalid,1,memory_order_relaxed);
    if(requested>UINT32_MAX) return;
    pwar_audio_queue_skip_write(&r->tx,(uint32_t)requested);
    atomic_fetch_add_explicit(&r->captured,requested,memory_order_relaxed);
    atomic_fetch_add_explicit(&r->tx_dropped,requested,memory_order_relaxed);
}

static void capture_process(void *userdata) {
    struct endpoint *e=userdata;
    struct relay *r=e->relay;
    struct pw_buffer *b=pw_stream_dequeue_buffer(e->stream);
    if(!b) return;
    atomic_fetch_add_explicit(&r->capture_callbacks,1,memory_order_relaxed);
    if(b->buffer->n_datas<1) { mark_capture_loss(r,b); goto done; }
    struct spa_data *d=&b->buffer->datas[0];
    if(!d->chunk || ((uintptr_t)d->data%STRIDE)!=0 || d->chunk->offset%STRIDE || d->chunk->offset>d->maxsize ||
       d->chunk->size>d->maxsize-d->chunk->offset || d->chunk->size%STRIDE) {
        mark_capture_loss(r,b); goto done;
    }
    unsigned frames=d->chunk->size/STRIDE;
    observe_quantum(&r->capture_quantum_frames,&r->capture_quantum_min,
                    &r->capture_quantum_max,frames);
    if(!atomic_load_explicit(&e->valid,memory_order_acquire) || !d->data ||
       frames>PWAR_AUDIO_QUEUE_FRAMES) {
        pwar_audio_queue_skip_write(&r->tx,frames);
        atomic_fetch_add_explicit(&r->captured,frames,memory_order_relaxed);
        atomic_fetch_add_explicit(&r->tx_dropped,frames,memory_order_relaxed);
        goto done;
    }
    unsigned written=pwar_audio_queue_write(&r->tx,
        (float*)((unsigned char*)d->data+d->chunk->offset),frames);
    observe_high_water(&r->tx_queue_high_water,pwar_audio_queue_available(&r->tx));
    atomic_fetch_add_explicit(&r->captured,frames,memory_order_relaxed);
    if(written!=frames) {
        /* Whole callback span is lost; preserve its position for the worker. */
        pwar_audio_queue_skip_write(&r->tx,frames);
        atomic_fetch_add_explicit(&r->tx_dropped,frames,memory_order_relaxed);
        atomic_fetch_add_explicit(&r->tx_queue_overflow_events,1,memory_order_relaxed);
    }
done:
    pw_stream_queue_buffer(e->stream,b);
}

static void playback_process(void *userdata) {
    struct endpoint *e=userdata;
    struct relay *r=e->relay;
    struct pw_buffer *b=pw_stream_dequeue_buffer(e->stream);
    if(!b) return;
    atomic_fetch_add_explicit(&r->playback_callbacks,1,memory_order_relaxed);
    if(b->buffer->n_datas<1) {
        if(b->requested<=UINT32_MAX) pwar_audio_queue_skip_read(&r->rx,(uint32_t)b->requested);
        atomic_fetch_add_explicit(&r->invalid,1,memory_order_relaxed);
        goto done;
    }
    struct spa_data *d=&b->buffer->datas[0];
    uint64_t requested=b->requested;
    if(!requested) requested=d->maxsize/STRIDE;
    observe_quantum(&r->playback_quantum_frames,&r->playback_quantum_min,
                    &r->playback_quantum_max,requested);
    if(!d->data || !d->chunk || ((uintptr_t)d->data%STRIDE)!=0 || d->chunk->offset%STRIDE ||
       d->chunk->offset>d->maxsize) {
        atomic_fetch_add_explicit(&r->invalid,1,memory_order_relaxed);
        if(requested<=UINT32_MAX) pwar_audio_queue_skip_read(&r->rx,(uint32_t)requested);
        goto done;
    }
    uint32_t offset=d->chunk->offset;
    uint32_t accessible=(d->maxsize-offset)/STRIDE;
    uint32_t frames=requested>accessible?accessible:(uint32_t)requested;
    if(requested>accessible) atomic_fetch_add_explicit(&r->invalid,1,memory_order_relaxed);
    float *output=(float*)((unsigned char*)d->data+offset);
    if(requested>PWAR_AUDIO_QUEUE_FRAMES || frames>PWAR_AUDIO_QUEUE_FRAMES) {
        /* Oversized host quantum: silence only verified accessible bytes. */
        memset(output,0,(size_t)frames*STRIDE);
        if(requested<=UINT32_MAX) pwar_audio_queue_skip_read(&r->rx,(uint32_t)requested);
        atomic_fetch_add_explicit(&r->silenced,frames,memory_order_relaxed);
        atomic_fetch_add_explicit(&r->invalid,1,memory_order_relaxed);
    } else {
        uint32_t read=0;
        const uint64_t synthesized_before=r->rx.synthesized_frames;
        if(atomic_load_explicit(&e->valid,memory_order_acquire) && frames)
            read=pwar_audio_queue_read(&r->rx,output,frames);
        observe_high_water(&r->rx_queue_high_water,pwar_audio_queue_available(&r->rx));
        if(read<frames) {
            memset(output+(size_t)read*2,0,(size_t)(frames-read)*STRIDE);
            /* Advance by the host clock quantum; late queued frames are stale. */
            pwar_audio_queue_skip_read(&r->rx,(uint32_t)requested-read);
        } else if(requested>frames && requested<=UINT32_MAX) {
            pwar_audio_queue_skip_read(&r->rx,(uint32_t)(requested-frames));
        }
        d->chunk->offset=offset; d->chunk->stride=STRIDE; d->chunk->size=frames*STRIDE;
        const uint32_t synthesized=(uint32_t)(r->rx.synthesized_frames-synthesized_before);
        atomic_fetch_add_explicit(&r->played,read-synthesized,memory_order_relaxed);
        atomic_fetch_add_explicit(&r->silenced,frames-read+synthesized,memory_order_relaxed);
    }
done:
    pw_stream_queue_buffer(e->stream,b);
}

static void format_changed(void *userdata,uint32_t id,const struct spa_pod *param) {
    struct endpoint *e=userdata;
    if(id!=SPA_PARAM_Format) return;
    struct spa_audio_info_raw info={0};
    int valid=param && spa_format_audio_raw_parse(param,&info)>=0 &&
        info.format==SPA_AUDIO_FORMAT_F32 && info.channels==2 && info.rate==WIRE_RATE;
    atomic_store_explicit(&e->valid,valid,memory_order_release);
    if(param && !valid) {
        fprintf(stderr,"Unsupported negotiated format: %u Hz, %u channels\n",info.rate,info.channels);
        atomic_store(&e->relay->failed,1);
    } else if(valid) {
        fprintf(stderr,"%s negotiated: %u Hz stereo float32\n",e->capture?"capture":"playback",info.rate);
    }
}
static void state_changed(void *userdata,enum pw_stream_state old,enum pw_stream_state state,const char *error) {
    (void)old;
    struct endpoint *e=userdata;
    if(state==PW_STREAM_STATE_ERROR) {
        fprintf(stderr,"PipeWire %s error: %s\n",e->capture?"capture":"playback",error?error:"unknown");
        atomic_store(&e->relay->failed,1);
    }
}
static const struct pw_stream_events capture_events={
    PW_VERSION_STREAM_EVENTS,.state_changed=state_changed,.param_changed=format_changed,.process=capture_process};
static const struct pw_stream_events playback_events={
    PW_VERSION_STREAM_EVENTS,.state_changed=state_changed,.param_changed=format_changed,.process=playback_process};

static void *network_worker(void *userdata) {
    struct relay *r=userdata;
    uint64_t seq=0;
    float interleaved[PWAR_ROUTER_MAX_BUFFER_SIZE*2],planar[PWAR_ROUTER_MAX_BUFFER_SIZE*2];
    float tx_staging[WIRE_FRAMES*2];
    unsigned tx_staged=0;
    while(!atomic_load_explicit(&r->stop,memory_order_acquire)) {
        /* Persistent staging reblocks arbitrary callback spans into 128 frames.
         * Queue reads also materialize known skip_write gaps as silence. */
        for(unsigned iteration=0;iteration<64;++iteration) {
            if(tx_staged<WIRE_FRAMES) {
                unsigned got=pwar_audio_queue_read(&r->tx,tx_staging+tx_staged*2,
                                                    WIRE_FRAMES-tx_staged);
                tx_staged+=got;
            }
            if(tx_staged<WIRE_FRAMES) break;
            if(r->passthrough) {
                unsigned written=pwar_audio_queue_write(&r->rx,tx_staging,WIRE_FRAMES);
                if(written!=WIRE_FRAMES) {
                    pwar_audio_queue_skip_write(&r->rx,WIRE_FRAMES);
                    atomic_fetch_add(&r->rx_dropped,WIRE_FRAMES);
                }
                tx_staged=0;
                continue;
            }
            pwar_packet_t packet;
            memset(&packet,0,sizeof(packet));
            packet.n_samples=WIRE_FRAMES; packet.seq=seq++; packet.num_packets=1;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC,&now);
            packet.timestamp=packet.seq_timestamp=(uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
            for(unsigned f=0;f<WIRE_FRAMES;++f)
                for(unsigned c=0;c<2;++c) packet.samples[c][f]=isfinite(tx_staging[f*2+c])?tx_staging[f*2+c]:0;
            ssize_t sent=sendto(r->socket_fd,&packet,sizeof(packet),0,(struct sockaddr*)&r->peer,sizeof(r->peer));
            if(sent==(ssize_t)sizeof(packet)) atomic_fetch_add(&r->transmitted,1);
            else atomic_fetch_add(&r->tx_dropped,WIRE_FRAMES);
            tx_staged=0;
        }
        if(r->passthrough) { struct pollfd unused={.fd=-1}; poll(&unused,0,1); continue; }
        struct pollfd fd={.fd=r->socket_fd,.events=POLLIN};
        if(poll(&fd,1,1)<0 && errno!=EINTR) { atomic_store(&r->failed,1); break; }
        for(unsigned iteration=0;iteration<64;++iteration) {
            pwar_packet_t packet;
            struct sockaddr_in sender;
            struct iovec iov={.iov_base=&packet,.iov_len=sizeof(packet)};
            struct msghdr msg={.msg_name=&sender,.msg_namelen=sizeof(sender),.msg_iov=&iov,.msg_iovlen=1};
            ssize_t bytes=recvmsg(r->socket_fd,&msg,MSG_DONTWAIT);
            if(bytes<0) { if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) atomic_store(&r->failed,1); break; }
            if(sender.sin_addr.s_addr!=r->peer.sin_addr.s_addr || (msg.msg_flags&MSG_TRUNC)) {
                atomic_fetch_add(&r->invalid,1); continue;
            }
            if(bytes==(ssize_t)sizeof(pwar_latency_info_t)) continue;
            if(bytes!=(ssize_t)sizeof(packet)) { atomic_fetch_add(&r->invalid,1); continue; }
            int frames=pwar_router_process_packet(&r->router,&packet,planar,PWAR_ROUTER_MAX_BUFFER_SIZE,2);
            if(frames<0) { atomic_fetch_add(&r->invalid,1); continue; }
            atomic_fetch_add(&r->received,1);
            if(frames==0) continue;
            for(int f=0;f<frames;++f)
                for(unsigned c=0;c<2;++c) interleaved[f*2+c]=isfinite(planar[c*frames+f])?planar[c*frames+f]:0;
            unsigned written=pwar_audio_queue_write(&r->rx,interleaved,(unsigned)frames);
            observe_high_water(&r->rx_queue_high_water,pwar_audio_queue_available(&r->rx));
            if(written!=(unsigned)frames) {
                pwar_audio_queue_skip_write(&r->rx,(unsigned)frames);
                atomic_fetch_add(&r->rx_dropped,(unsigned)frames);
                atomic_fetch_add(&r->rx_queue_overflow_events,1);
            }
        }
    }
    return NULL;
}
static void quit_signal(void *userdata,int signal_number) {
    (void)signal_number;
    pw_main_loop_quit(((struct relay*)userdata)->loop);
}
static void report(void *userdata,uint64_t expirations) {
    (void)expirations;
    struct relay *r=userdata;
    const uint64_t capture_callbacks=atomic_exchange(&r->capture_callbacks,0);
    const uint64_t capture_quantum_frames=atomic_exchange(&r->capture_quantum_frames,0);
    const uint64_t capture_quantum_min=atomic_exchange(&r->capture_quantum_min,0);
    const uint64_t capture_quantum_max=atomic_exchange(&r->capture_quantum_max,0);
    const uint64_t playback_callbacks=atomic_exchange(&r->playback_callbacks,0);
    const uint64_t playback_quantum_frames=atomic_exchange(&r->playback_quantum_frames,0);
    const uint64_t playback_quantum_min=atomic_exchange(&r->playback_quantum_min,0);
    const uint64_t playback_quantum_max=atomic_exchange(&r->playback_quantum_max,0);
    const uint64_t tx_queue_high_water=atomic_exchange(&r->tx_queue_high_water,0);
    const uint64_t rx_queue_high_water=atomic_exchange(&r->rx_queue_high_water,0);
    const uint64_t tx_queue_overflow_events=atomic_exchange(&r->tx_queue_overflow_events,0);
    const uint64_t rx_queue_overflow_events=atomic_exchange(&r->rx_queue_overflow_events,0);
    fprintf(stderr,"{\"captured_frames\":%llu,\"played_frames\":%llu,\"tx_packets\":%llu,\"rx_packets\":%llu,\"invalid_packets\":%llu,\"tx_dropped_frames\":%llu,\"rx_dropped_frames\":%llu,\"silenced_frames\":%llu,\"tx_queue_frames\":%u,\"tx_queue_high_water_frames_interval\":%llu,\"tx_queue_overflow_events_interval\":%llu,\"rx_queue_frames\":%u,\"rx_queue_high_water_frames_interval\":%llu,\"rx_queue_overflow_events_interval\":%llu,\"capture_callbacks_interval\":%llu,\"capture_quantum_frames_interval\":%llu,\"capture_quantum_min_frames_interval\":%llu,\"capture_quantum_max_frames_interval\":%llu,\"playback_callbacks_interval\":%llu,\"playback_quantum_frames_interval\":%llu,\"playback_quantum_min_frames_interval\":%llu,\"playback_quantum_max_frames_interval\":%llu}\n",
        atomic_load(&r->captured),atomic_load(&r->played),atomic_load(&r->transmitted),atomic_load(&r->received),
        atomic_load(&r->invalid),atomic_load(&r->tx_dropped),atomic_load(&r->rx_dropped),atomic_load(&r->silenced),
        pwar_audio_queue_available(&r->tx),(unsigned long long)tx_queue_high_water,
        (unsigned long long)tx_queue_overflow_events,pwar_audio_queue_available(&r->rx),
        (unsigned long long)rx_queue_high_water,(unsigned long long)rx_queue_overflow_events,
        (unsigned long long)capture_callbacks,(unsigned long long)capture_quantum_frames,
        (unsigned long long)capture_quantum_min,(unsigned long long)capture_quantum_max,
        (unsigned long long)playback_callbacks,(unsigned long long)playback_quantum_frames,
        (unsigned long long)playback_quantum_min,(unsigned long long)playback_quantum_max);
    if(atomic_load(&r->failed)) pw_main_loop_quit(r->loop);
}
static int make_stream(struct relay *r,struct endpoint *e,int capture,unsigned requested_frames) {
    e->relay=r; e->capture=capture; atomic_init(&e->valid,0);
    struct pw_properties *props=pw_properties_new(
        PW_KEY_NODE_NAME,capture?"pwar-send":"pwar-receive",
        PW_KEY_NODE_DESCRIPTION,capture?"PWAR to Windows":"PWAR from Windows",
        PW_KEY_MEDIA_TYPE,"Audio",PW_KEY_MEDIA_CATEGORY,capture?"Capture":"Playback",
        PW_KEY_MEDIA_ROLE,"DSP","node.autoconnect","false","node.dont-reconnect","true",NULL);
    if(!props) return -1;
    pw_properties_setf(props,PW_KEY_NODE_LATENCY,"%u/%u",requested_frames,WIRE_RATE);
    e->stream=pw_stream_new_simple(pw_main_loop_get_loop(r->loop),capture?"PWAR send":"PWAR receive",
        props,capture?&capture_events:&playback_events,e);
    if(!e->stream) return -1;
    uint8_t buffer[1024];
    struct spa_pod_builder builder=SPA_POD_BUILDER_INIT(buffer,sizeof(buffer));
    struct spa_audio_info_raw format={.format=SPA_AUDIO_FORMAT_F32,.rate=WIRE_RATE,.channels=2,
        .position={SPA_AUDIO_CHANNEL_FL,SPA_AUDIO_CHANNEL_FR}};
    const struct spa_pod *params[]={spa_format_audio_raw_build(&builder,SPA_PARAM_EnumFormat,&format)};
    return pw_stream_connect(e->stream,capture?PW_DIRECTION_INPUT:PW_DIRECTION_OUTPUT,PW_ID_ANY,
        PW_STREAM_FLAG_MAP_BUFFERS|PW_STREAM_FLAG_RT_PROCESS,params,1);
}

int pwar_cli_run(const pwar_config_t *config) {
    if(!config || config->buffer_size<1 || config->buffer_size>4096 || config->oneshot_mode) {
        fprintf(stderr,"Stream backend requires asynchronous mode and a buffer request in 1..4096\n"); return 2;
    }
    struct relay *r=aligned_alloc(_Alignof(struct relay),sizeof(struct relay));
    if(!r) return 1;
    memset(r,0,sizeof(*r));
    r->socket_fd=-1;
    atomic_init(&r->stop,0); atomic_init(&r->failed,0);
    atomic_init(&r->captured,0); atomic_init(&r->played,0); atomic_init(&r->transmitted,0); atomic_init(&r->received,0);
    atomic_init(&r->invalid,0); atomic_init(&r->tx_dropped,0); atomic_init(&r->rx_dropped,0); atomic_init(&r->silenced,0);
    atomic_init(&r->capture_callbacks,0); atomic_init(&r->capture_quantum_frames,0);
    atomic_init(&r->capture_quantum_min,0); atomic_init(&r->capture_quantum_max,0);
    atomic_init(&r->playback_callbacks,0); atomic_init(&r->playback_quantum_frames,0);
    atomic_init(&r->playback_quantum_min,0); atomic_init(&r->playback_quantum_max,0);
    atomic_init(&r->tx_queue_high_water,0); atomic_init(&r->rx_queue_high_water,0);
    atomic_init(&r->tx_queue_overflow_events,0); atomic_init(&r->rx_queue_overflow_events,0);
    pwar_audio_queue_init(&r->tx); pwar_audio_queue_init(&r->rx); pwar_router_init(&r->router,2);
    r->peer.sin_family=AF_INET; r->peer.sin_port=htons(config->stream_port);
    int result=1;
    if(config->stream_port<1 || config->stream_port>65535 || inet_pton(AF_INET,config->stream_ip,&r->peer.sin_addr)!=1) goto done;
    r->passthrough=config->passthrough_test;
    r->socket_fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(r->socket_fd<0) { perror("PWAR socket"); goto done; }
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(8321),.sin_addr={.s_addr=INADDR_ANY}};
    if(bind(r->socket_fd,(struct sockaddr*)&local,sizeof(local))<0) { perror("PWAR receive port 8321"); goto done; }
    pw_init(NULL,NULL);
    r->loop=pw_main_loop_new(NULL);
    if(!r->loop) goto deinit;
    if(make_stream(r,&r->capture,1,(unsigned)config->buffer_size)<0 ||
       make_stream(r,&r->playback,0,(unsigned)config->buffer_size)<0) goto streams;
    if(pthread_create(&r->worker,NULL,network_worker,r)!=0) goto streams;
    r->worker_started=1;
    struct pw_loop *loop=pw_main_loop_get_loop(r->loop);
    pw_loop_add_signal(loop,SIGINT,quit_signal,r); pw_loop_add_signal(loop,SIGTERM,quit_signal,r);
    r->reporter=pw_loop_add_timer(loop,report,r);
    if(!r->reporter) goto streams;
    struct timespec interval={.tv_sec=1};
    pw_loop_update_timer(loop,r->reporter,&interval,&interval,false);
    pw_main_loop_run(r->loop);
    result=atomic_load(&r->failed)?1:0;
streams:
    /* Stream teardown drains callbacks before queue ownership can end. */
    if(r->capture.stream) pw_stream_destroy(r->capture.stream);
    if(r->playback.stream) pw_stream_destroy(r->playback.stream);
    atomic_store_explicit(&r->stop,1,memory_order_release);
    if(r->worker_started) pthread_join(r->worker,NULL);
    if(r->reporter) pw_loop_destroy_source(pw_main_loop_get_loop(r->loop),r->reporter);
    pw_main_loop_destroy(r->loop);
deinit:
    pw_deinit();
done:
    if(r->socket_fd>=0) close(r->socket_fd);
    free(r);
    return result;
}
