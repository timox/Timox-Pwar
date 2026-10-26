#include "libpwar.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <unistd.h>
#include <spa/pod/builder.h>
#include <spa/param/latency-utils.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include "latency_manager.h"

#include "pwar_packet.h"
#include "pwar_router.h"
#include "pwar_audio_queue.h"

#define DEFAULT_STREAM_IP "192.168.66.3"
#define DEFAULT_STREAM_PORT 8321

#define MAX_BUFFER_SIZE 4096
#define NUM_CHANNELS 2

// Global data for GUI mode
static struct data *g_pwar_data = NULL;
static int g_pwar_initialized = 0;
static int g_pwar_running = 0;
static pwar_config_t g_current_config;

struct data;

struct port {
    struct data *data;
};

struct data {
    struct pw_main_loop *loop;
    struct pw_filter *filter;
    pthread_t pw_thread;
    int pw_thread_started;
    pthread_t network_thread;
    int network_thread_started;
    atomic_int network_running;
    atomic_uint rt_xruns;
    atomic_uint current_windows_buffer_size;
    pwar_audio_queue linux_to_windows;
    pwar_audio_queue windows_to_linux;
    struct port *in_port;
    struct port *left_out_port;
    struct port *right_out_port;
    float sine_phase;
    uint8_t passthrough_test; // Add passthrough_test flag
    uint8_t oneshot_mode; // Add oneshot_mode flag
    uint64_t seq;
    int sockfd;
    struct sockaddr_in servaddr;
    int recv_sockfd;

    pwar_router_t linux_router;
};

static int setup_recv_socket(struct data *data, int port);
static void *receiver_thread(void *userdata);

static int setup_socket(struct data *data, const char *ip, int port);

static void on_process(void *userdata, struct spa_io_position *position);
static void do_quit(void *userdata, int signal_number);

// Extract common initialization logic
static int init_data_structure(struct data *data, const pwar_config_t *config);
static int create_pipewire_filter(struct data *data);

// New GUI functions
int pwar_requires_restart(const pwar_config_t *old_config, const pwar_config_t *new_config);
int pwar_update_config(const pwar_config_t *config);
int pwar_init(const pwar_config_t *config);
int pwar_start(void);
int pwar_stop(void);
void pwar_cleanup(void);
int pwar_is_running(void);

static int setup_recv_socket(struct data *data, int port) {
    data->recv_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data->recv_sockfd < 0) {
        perror("recv socket creation failed");
        return -1;
    }
    // Increase UDP receive buffer to 1MB to reduce risk of overrun
    int rcvbuf = 1024 * 1024;
    if (setsockopt(data->recv_sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF failed");
    }
    struct sockaddr_in recv_addr;
    memset(&recv_addr, 0, sizeof(recv_addr));
    recv_addr.sin_family = AF_INET;
    recv_addr.sin_addr.s_addr = INADDR_ANY;
    recv_addr.sin_port = htons(port);
    if (bind(data->recv_sockfd, (struct sockaddr *)&recv_addr, sizeof(recv_addr)) < 0) {
        perror("recv socket bind failed");
        close(data->recv_sockfd);
        data->recv_sockfd = -1;
        return -1;
    }
    return 0;
}

static uint32_t pending_router_block_frames(const pwar_router_t *router, uint32_t fallback) {
    if (!router || !router->expected_packets ||
        router->expected_packets > sizeof(router->fragment_frames) / sizeof(router->fragment_frames[0]))
        return fallback;
    const uint32_t chunk_frames = router->chunk_frames ? router->chunk_frames : fallback;
    uint64_t frames = 0;
    for (uint32_t i = 0; i < router->expected_packets; ++i)
        frames += router->fragment_frames[i] ? router->fragment_frames[i] : chunk_frames;
    return frames && frames <= UINT32_MAX ? (uint32_t)frames : fallback;
}
static void *receiver_thread(void *userdata) {
    struct data *data = (struct data *)userdata;
    char recv_buffer[sizeof(pwar_packet_t) > sizeof(pwar_latency_info_t) ? sizeof(pwar_packet_t) : sizeof(pwar_latency_info_t)];
    float linux_output_buffers[NUM_CHANNELS * MAX_BUFFER_SIZE] = {0};
    float interleaved[NUM_CHANNELS * MAX_BUFFER_SIZE];
    float tx_packet[NUM_CHANNELS * PWAR_PACKET_MAX_CHUNK_SIZE];
    uint32_t tx_staged = 0;
    const uint32_t tx_packet_frames = PWAR_PACKET_MIN_CHUNK_SIZE;

    while (atomic_load_explicit(&data->network_running, memory_order_acquire)) {
        while (tx_staged < tx_packet_frames) {
            uint32_t got = pwar_audio_queue_read(&data->linux_to_windows,
                &tx_packet[tx_staged * NUM_CHANNELS],
                tx_packet_frames - tx_staged);
            tx_staged += got;
            if (got == 0) break;
        }
        if (tx_staged == tx_packet_frames) {
            pwar_packet_t outgoing = {0};
            outgoing.seq = data->seq++;
            outgoing.n_samples = (uint16_t)tx_packet_frames;
            outgoing.num_packets = 1;
            outgoing.packet_index = 0;
            outgoing.timestamp = latency_manager_timestamp_now();
            outgoing.seq_timestamp = outgoing.timestamp;
            for (uint32_t frame = 0; frame < tx_packet_frames; ++frame) {
                outgoing.samples[0][frame] = tx_packet[frame * NUM_CHANNELS];
                outgoing.samples[1][frame] = tx_packet[frame * NUM_CHANNELS + 1];
            }
            if (sendto(data->sockfd, &outgoing, sizeof(outgoing), 0,
                    (struct sockaddr *)&data->servaddr, sizeof(data->servaddr)) < 0)
                atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            tx_staged = 0;
        }

        struct pollfd socket_event = { .fd = data->recv_sockfd, .events = POLLIN };
        int ready = poll(&socket_event, 1, 1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            break;
        }
        if (ready == 0) continue;
        if (socket_event.revents & (POLLNVAL | POLLHUP)) {
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            break;
        }
        if (socket_event.revents & POLLERR) {
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            getsockopt(data->recv_sockfd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size);
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            continue;
        }
        if (!(socket_event.revents & POLLIN)) continue;

        struct iovec packet_iov = { .iov_base = recv_buffer, .iov_len = sizeof(recv_buffer) };
        struct msghdr packet_message = {0};
        packet_message.msg_iov = &packet_iov;
        packet_message.msg_iovlen = 1;
        ssize_t received = recvmsg(data->recv_sockfd, &packet_message, MSG_TRUNC);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED)
                continue;
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            break;
        }
        if (packet_message.msg_flags & MSG_TRUNC) {
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            continue;
        }
        if (received == (ssize_t)sizeof(pwar_packet_t)) {
            pwar_packet_t packet;
            memcpy(&packet, recv_buffer, sizeof(packet));
            if (packet.n_samples < PWAR_PACKET_MIN_CHUNK_SIZE ||
                packet.n_samples > PWAR_PACKET_MAX_CHUNK_SIZE ||
                !packet.num_packets || packet.num_packets > MAX_BUFFER_SIZE / packet.n_samples ||
                packet.packet_index >= packet.num_packets) {
                atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
                continue;
            }

            /* Sequence numbers count the Linux 64-frame chunks that begin a
               block. Windows response fragments share that block sequence. */
            if (data->linux_router.active && packet.seq != data->linux_router.current_seq) {
                const uint64_t delta = packet.seq - data->linux_router.current_seq;
                if (delta < (UINT64_C(1) << 63)) {
                    const uint32_t old_packets = data->linux_router.expected_packets;
                    const uint32_t old_frames = pending_router_block_frames(
                        &data->linux_router, (uint32_t)packet.n_samples * packet.num_packets);
                    const uint64_t max_gap = PWAR_AUDIO_QUEUE_FRAMES;
                    uint64_t lost_frames = data->linux_router.delivered ? 0u : old_frames;
                    if (lost_frames > max_gap) lost_frames = max_gap;
                    if (old_packets && delta > old_packets) {
                        const uint64_t missing_chunks = delta - old_packets;
                        if (missing_chunks > max_gap / PWAR_PACKET_MIN_CHUNK_SIZE) {
                            lost_frames = max_gap;
                        } else {
                            const uint64_t missing_frames = missing_chunks * PWAR_PACKET_MIN_CHUNK_SIZE;
                            lost_frames = lost_frames > max_gap - missing_frames
                                ? max_gap : lost_frames + missing_frames;
                        }
                    }
                    if (lost_frames) {
                        pwar_audio_queue_skip_write(&data->windows_to_linux, (uint32_t)lost_frames);
                        atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
                    }
                }
            }

            /* Explicit grouped fragments pass unchanged to the protocol router. */
            atomic_store_explicit(&data->current_windows_buffer_size,
                (uint32_t)packet.n_samples * packet.num_packets, memory_order_relaxed);
            latency_manager_process_packet_server(&packet);
            int samples_ready = pwar_router_process_packet(&data->linux_router,
                &packet, linux_output_buffers, MAX_BUFFER_SIZE, NUM_CHANNELS);
            if (samples_ready > 0) {
                for (int frame = 0; frame < samples_ready; ++frame) {
                    interleaved[frame * NUM_CHANNELS] = linux_output_buffers[frame];
                    interleaved[frame * NUM_CHANNELS + 1] =
                        linux_output_buffers[samples_ready + frame];
                }
                if (pwar_audio_queue_write(&data->windows_to_linux, interleaved,
                        (uint32_t)samples_ready) == 0) {
                    pwar_audio_queue_skip_write(&data->windows_to_linux,
                        (uint32_t)samples_ready);
                    atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
                }
            } else if (samples_ready < 0) {
                atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
            }
        } else if (received == (ssize_t)sizeof(pwar_latency_info_t)) {
            pwar_latency_info_t latency_info;
            memcpy(&latency_info, recv_buffer, sizeof(latency_info));
            latency_manager_handle_latency_info(&latency_info);
        } else {
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
        }
    }
    atomic_store_explicit(&data->network_running, 0, memory_order_release);
    return NULL;
}

static int setup_socket(struct data *data, const char *ip, int port) {
    if (!ip || port < 1 || port > 65535) return -1;
    data->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (data->sockfd < 0) {
        perror("socket creation failed");
        return -1;
    }
    memset(&data->servaddr, 0, sizeof(data->servaddr));
    data->servaddr.sin_family = AF_INET;
    data->servaddr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &data->servaddr.sin_addr) != 1) {
        close(data->sockfd);
        data->sockfd = -1;
        return -1;
    }
    return 0;
}

static void on_process(void *userdata, struct spa_io_position *position) {
    struct data *data = (struct data *)userdata;
    if (!data || !position || position->clock.duration == 0)
        return;
    if (position->clock.duration > UINT32_MAX) {
        atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
        return;
    }
    const uint32_t n_samples = (uint32_t)position->clock.duration;
    if (n_samples > MAX_BUFFER_SIZE) {
        float *left_out = pw_filter_get_dsp_buffer(data->left_out_port, n_samples);
        float *right_out = pw_filter_get_dsp_buffer(data->right_out_port, n_samples);
        if (left_out) memset(left_out, 0, (size_t)n_samples * sizeof(float));
        if (right_out) memset(right_out, 0, (size_t)n_samples * sizeof(float));
        pwar_audio_queue_skip_write(&data->linux_to_windows, n_samples);
        pwar_audio_queue_skip_read(&data->windows_to_linux, n_samples);
        atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
        return;
    }
    float *in = pw_filter_get_dsp_buffer(data->in_port, n_samples);
    float *left_out = pw_filter_get_dsp_buffer(data->left_out_port, n_samples);
    float *right_out = pw_filter_get_dsp_buffer(data->right_out_port, n_samples);

    if (data->passthrough_test) {
        if (!in)
            atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
        if (left_out && in)
            memcpy(left_out, in, n_samples * sizeof(float));
        else if (left_out)
            memset(left_out, 0, n_samples * sizeof(float));
        if (right_out && in)
            memcpy(right_out, in, n_samples * sizeof(float));
        else if (right_out)
            memset(right_out, 0, n_samples * sizeof(float));
        return;
    }
    if (!in)
        atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);

    float interleaved[NUM_CHANNELS * n_samples];
    for (uint32_t frame = 0; frame < n_samples; ++frame) {
        const float sample = in ? in[frame] : 0.0f;
        interleaved[frame * NUM_CHANNELS] = sample;
        interleaved[frame * NUM_CHANNELS + 1] = sample;
    }
    if (pwar_audio_queue_write(&data->linux_to_windows, interleaved, n_samples) == 0) {
        pwar_audio_queue_skip_write(&data->linux_to_windows, n_samples);
        atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
    }

    uint32_t received = pwar_audio_queue_read(&data->windows_to_linux, interleaved, n_samples);
    if (received < n_samples) {
        memset(&interleaved[received * NUM_CHANNELS], 0,
            (n_samples - received) * NUM_CHANNELS * sizeof(float));
        pwar_audio_queue_skip_read(&data->windows_to_linux, n_samples - received);
        atomic_fetch_add_explicit(&data->rt_xruns, 1u, memory_order_relaxed);
    }
    for (uint32_t frame = 0; frame < n_samples; ++frame) {
        if (left_out) left_out[frame] = interleaved[frame * NUM_CHANNELS];
        if (right_out) right_out[frame] = interleaved[frame * NUM_CHANNELS + 1];
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static void do_quit(void *userdata, int signal_number) {
    struct data *data = (struct data *)userdata;
    pw_main_loop_quit(data->loop);
}

// Thread function to run PipeWire main loop for GUI mode
static void *pipewire_thread_func(void *userdata) {
    struct data *data = (struct data *)userdata;
    pw_main_loop_run(data->loop);
    return NULL;
}

// Extract common initialization logic
static int init_data_structure(struct data *data, const pwar_config_t *config) {
    memset(data, 0, sizeof(struct data));
    data->sockfd = -1;
    data->recv_sockfd = -1;
    atomic_init(&data->network_running, 0);
    atomic_init(&data->rt_xruns, 0);
    atomic_init(&data->current_windows_buffer_size, 0);
    pwar_audio_queue_init(&data->linux_to_windows);
    pwar_audio_queue_init(&data->windows_to_linux);

    if (setup_socket(data, config->stream_ip, config->stream_port) < 0)
        return -1;
    if (setup_recv_socket(data, DEFAULT_STREAM_PORT) < 0) {
        close(data->sockfd);
        data->sockfd = -1;
        return -1;
    }
    
    data->passthrough_test = config->passthrough_test;
    data->oneshot_mode = config->oneshot_mode;
    data->sine_phase = 0.0f;
    pwar_router_init(&data->linux_router, NUM_CHANNELS);
    
    return 0;
}

static int create_pipewire_filter(struct data *data) {
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    data->filter = pw_filter_new_simple(
        pw_main_loop_get_loop(data->loop),
        "pwar",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            NULL),
        &filter_events,
        data);

    data->in_port = pw_filter_add_port(data->filter,
        PW_DIRECTION_INPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(struct port),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "input",
            NULL),
        NULL, 0);

    data->left_out_port = pw_filter_add_port(data->filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(struct port),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "output-left",
            NULL),
        NULL, 0);

    data->right_out_port = pw_filter_add_port(data->filter,
        PW_DIRECTION_OUTPUT,
        PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(struct port),
        pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio",
            PW_KEY_PORT_NAME, "output-right",
            NULL),
        NULL, 0);

    params[0] = spa_process_latency_build(&b,
        SPA_PARAM_ProcessLatency,
        &SPA_PROCESS_LATENCY_INFO_INIT(
            .ns = 10 * SPA_NSEC_PER_MSEC
        ));

    if (pw_filter_connect(data->filter,
            PW_FILTER_FLAG_RT_PROCESS,
            params, 1) < 0) {
        return -1;
    }

    return 0;
}

// New GUI functions
int pwar_requires_restart(const pwar_config_t *old_config, const pwar_config_t *new_config) {
    if (old_config->buffer_size != new_config->buffer_size ||
        strcmp(old_config->stream_ip, new_config->stream_ip) != 0 ||
        old_config->stream_port != new_config->stream_port) {
        return 1;
    }
    return 0;
}

int pwar_update_config(const pwar_config_t *config) {
    if (!g_pwar_initialized) {
        return -1;
    }

    if (pwar_requires_restart(&g_current_config, config)) {
        return -2; // Signal that restart is needed
    }

    // Apply runtime-changeable settings
    g_pwar_data->passthrough_test = config->passthrough_test;
    g_pwar_data->oneshot_mode = config->oneshot_mode;
    g_current_config = *config;
    
    return 0;
}

int pwar_init(const pwar_config_t *config) {
    if (g_pwar_initialized) {
        return -1;
    }

    g_current_config = *config;

    char latency[32];
    snprintf(latency, sizeof(latency), "%d/48000", config->buffer_size);
    setenv("PIPEWIRE_LATENCY", latency, 1);

    g_pwar_data = malloc(sizeof(struct data));
    if (!g_pwar_data) {
        return -1;
    }

    if (init_data_structure(g_pwar_data, config) < 0) {
        free(g_pwar_data);
        g_pwar_data = NULL;
        return -1;
    }

    pw_init(NULL, NULL);
    g_pwar_data->loop = pw_main_loop_new(NULL);

    g_pwar_initialized = 1;
    return 0;
}

int pwar_start(void) {
    if (!g_pwar_initialized || g_pwar_running) {
        return -1;
    }

    // Create a new main loop for this start session
    if (g_pwar_data->loop) {
        pw_main_loop_destroy(g_pwar_data->loop);
    }
    g_pwar_data->loop = pw_main_loop_new(NULL);

    pwar_audio_queue_init(&g_pwar_data->linux_to_windows);
    pwar_audio_queue_init(&g_pwar_data->windows_to_linux);
    pwar_router_init(&g_pwar_data->linux_router, NUM_CHANNELS);
    atomic_store_explicit(&g_pwar_data->network_running, 1, memory_order_release);
    if (pthread_create(&g_pwar_data->network_thread, NULL,
            receiver_thread, g_pwar_data) != 0) {
        atomic_store_explicit(&g_pwar_data->network_running, 0, memory_order_release);
        return -1;
    }
    g_pwar_data->network_thread_started = 1;

    if (create_pipewire_filter(g_pwar_data) < 0) {
        if (g_pwar_data->filter) {
            pw_filter_destroy(g_pwar_data->filter);
            g_pwar_data->filter = NULL;
        }
        atomic_store_explicit(&g_pwar_data->network_running, 0, memory_order_release);
        pthread_join(g_pwar_data->network_thread, NULL);
        g_pwar_data->network_thread_started = 0;
        return -1;
    }

    // Start the PipeWire main loop in a separate thread for GUI mode
    if (pthread_create(&g_pwar_data->pw_thread, NULL,
            pipewire_thread_func, g_pwar_data) != 0) {
        pw_filter_destroy(g_pwar_data->filter);
        g_pwar_data->filter = NULL;
        atomic_store_explicit(&g_pwar_data->network_running, 0, memory_order_release);
        pthread_join(g_pwar_data->network_thread, NULL);
        g_pwar_data->network_thread_started = 0;
        return -1;
    }
    g_pwar_data->pw_thread_started = 1;

    g_pwar_running = 1;
    return 0;
}

int pwar_stop(void) {
    if (!g_pwar_running) {
        return -1;
    }

    // Signal the PipeWire main loop to quit
    if (g_pwar_data->loop) {
        pw_main_loop_quit(g_pwar_data->loop);
    }

    if (g_pwar_data->pw_thread_started) {
        pthread_join(g_pwar_data->pw_thread, NULL);
        g_pwar_data->pw_thread_started = 0;
    }

    if (g_pwar_data->filter) {
        pw_filter_destroy(g_pwar_data->filter);
        g_pwar_data->filter = NULL;
    }

    atomic_store_explicit(&g_pwar_data->network_running, 0, memory_order_release);
    if (g_pwar_data->network_thread_started) {
        pthread_join(g_pwar_data->network_thread, NULL);
        g_pwar_data->network_thread_started = 0;
    }

    g_pwar_running = 0;
    return 0;
}

void pwar_cleanup(void) {
    if (g_pwar_running) {
        pwar_stop();
    }

    if (g_pwar_initialized) {
        if (g_pwar_data->loop) {
            pw_main_loop_destroy(g_pwar_data->loop);
        }
        pw_deinit();

        if (g_pwar_data->sockfd >= 0) {
            close(g_pwar_data->sockfd);
        }
        if (g_pwar_data->recv_sockfd >= 0) {
            close(g_pwar_data->recv_sockfd);
        }

        free(g_pwar_data);
        g_pwar_data = NULL;
        g_pwar_initialized = 0;
    }
}

int pwar_is_running(void) {
    return g_pwar_running && g_pwar_data &&
        atomic_load_explicit(&g_pwar_data->network_running, memory_order_acquire);
}


int pwar_cli_run(const pwar_config_t *config) {
    if (!config) return -1;
    char latency[32];
    snprintf(latency, sizeof(latency), "%d/48000", config->buffer_size);
    setenv("PIPEWIRE_LATENCY", latency, 1);

    struct data data;

    // Use the shared initialization function
    if (init_data_structure(&data, config) < 0) {
        return -1;
    }

    atomic_store_explicit(&data.network_running, 1, memory_order_release);
    if (pthread_create(&data.network_thread, NULL, receiver_thread, &data) != 0) {
        atomic_store_explicit(&data.network_running, 0, memory_order_release);
        close(data.sockfd);
        close(data.recv_sockfd);
        return -1;
    }
    data.network_thread_started = 1;
    pw_init(NULL, NULL);
    data.loop = pw_main_loop_new(NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    if (create_pipewire_filter(&data) < 0) {
        fprintf(stderr, "can't connect\n");
        if (data.filter)
            pw_filter_destroy(data.filter);
        atomic_store_explicit(&data.network_running, 0, memory_order_release);
        pthread_join(data.network_thread, NULL);
        pw_main_loop_destroy(data.loop);
        pw_deinit();
        close(data.sockfd);
        close(data.recv_sockfd);
        return -1;
    }

    pw_main_loop_run(data.loop);
    pw_filter_destroy(data.filter);
    atomic_store_explicit(&data.network_running, 0, memory_order_release);
    if (data.network_thread_started)
        pthread_join(data.network_thread, NULL);
    pw_main_loop_destroy(data.loop);
    pw_deinit();
    close(data.sockfd);
    close(data.recv_sockfd);
    return 0;
}

void pwar_get_latency_metrics(pwar_latency_metrics_t *metrics) {
    if (!metrics) return;
    
    if (g_pwar_initialized && g_pwar_running) {
        latency_manager_get_current_metrics(metrics);
        metrics->xruns += atomic_load_explicit(&g_pwar_data->rt_xruns,
                                                memory_order_relaxed);
    } else {
        // Return zeros if not running
        metrics->audio_proc_min_ms = 0.0;
        metrics->audio_proc_max_ms = 0.0;
        metrics->audio_proc_avg_ms = 0.0;
        metrics->jitter_min_ms = 0.0;
        metrics->jitter_max_ms = 0.0;
        metrics->jitter_avg_ms = 0.0;
        metrics->rtt_min_ms = 0.0;
        metrics->rtt_max_ms = 0.0;
        metrics->rtt_avg_ms = 0.0;
        metrics->xruns = 0;
    }
}

uint32_t pwar_get_current_windows_buffer_size(void) {
    if (g_pwar_initialized && g_pwar_running && g_pwar_data) {
        return atomic_load_explicit(&g_pwar_data->current_windows_buffer_size,
                                    memory_order_relaxed);
    }
    return 0;
}
