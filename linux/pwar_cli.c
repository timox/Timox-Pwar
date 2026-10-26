/*
 * pwar_cli.c - CLI frontend for PipeWire <-> UDP streaming bridge (PWAR)
 *
 * (c) 2025 Philip K. Gisslow
 * This file is part of the PipeWire ASIO Relay (PWAR) project.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include "libpwar.h"

#define DEFAULT_STREAM_IP "192.168.66.3"
#define DEFAULT_STREAM_PORT 8321
#define DEFAULT_BUFFER_SIZE 64

static void usage(const char *program) {
    const char *name = strrchr(program, '/');
    printf("Usage: %s [options]\n", name ? name + 1 : program);
    puts(
         "  --help, -h              Show help without starting audio\n"
         "  --ip, -i IPv4           Windows destination IPv4\n"
         "  --port, -p PORT         Destination UDP port (1..65535)\n"
         "  --buffer_size, -b N     Requested PipeWire buffer (1..4096 frames)\n"
         "  --oneshot               Enable synchronous request/response mode\n"
         "  --passthrough_test, -pt Local audio passthrough\n"
         "  --check-config          Validate and display settings without audio\n"
         "Linux receive port: 8321. Sample rate request: 48000 Hz.\n"
         "The requested buffer is a latency hint; PipeWire may choose another quantum.\n"
         "The stream backend supports asynchronous mode only (no --oneshot).");
}

static int parse_positive(const char *text, int maximum, int *value) {
    if (!text || !*text) return 0;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno || !end || *end || parsed < 1 || parsed > maximum) return 0;
    *value = (int)parsed;
    return 1;
}

int main(int argc, char *argv[]) {
    pwar_config_t config;
    memset(&config, 0, sizeof(config));
    strncpy(config.stream_ip, DEFAULT_STREAM_IP, sizeof(config.stream_ip) - 1);
    config.stream_port = DEFAULT_STREAM_PORT;
    config.passthrough_test = 0;
    config.oneshot_mode = 0;
    config.buffer_size = DEFAULT_BUFFER_SIZE;

    int check_config = 0;
    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(option, "--check-config")) {
            check_config = 1;
        } else if (!strcmp(option, "--passthrough_test") || !strcmp(option, "-pt")) {
            config.passthrough_test = 1;
        } else if (!strcmp(option, "--oneshot")) {
            config.oneshot_mode = 1;
        } else {
            const int ip = !strcmp(option, "--ip") || !strcmp(option, "-i");
            const int port = !strcmp(option, "--port") || !strcmp(option, "-p");
            const int buffer = !strcmp(option, "--buffer_size") || !strcmp(option, "-b");
            if (!ip && !port && !buffer) {
                fprintf(stderr, "Unknown option: %s (use --help)\n", option);
                return 2;
            }
            if (i + 1 >= argc || !strncmp(argv[i + 1], "--", 2)) {
                fprintf(stderr, "Missing value for %s\n", option);
                return 2;
            }
            const char *value = argv[++i];
            if (ip) {
                struct in_addr address;
                if (strlen(value) >= sizeof(config.stream_ip) || inet_pton(AF_INET, value, &address) != 1) {
                    fprintf(stderr, "Invalid IPv4 address: %s\n", value);
                    return 2;
                }
                strcpy(config.stream_ip, value);
            } else if (!parse_positive(value, port ? 65535 : 4096,
                       port ? &config.stream_port : &config.buffer_size)) {
                fprintf(stderr, "Invalid value for %s: %s\n", option, value);
                return 2;
            }
        }
    }

    printf("%s\n", check_config ? "Configuration valid:" : "Starting PWAR with config:");
    printf("  Stream IP: %s\n", config.stream_ip);
    printf("  Stream Port: %d\n", config.stream_port);
    printf("  Passthrough Test: %s\n", config.passthrough_test ? "Enabled" : "Disabled");
    printf("  Oneshot Mode: %s\n", config.oneshot_mode ? "Enabled" : "Disabled");
    printf("  Buffer Size: %d\n", config.buffer_size);
    if (check_config) return 0;

    char latency[32];
    snprintf(latency, sizeof(latency), "%d/48000", config.buffer_size);
    setenv("PIPEWIRE_LATENCY", latency, 1);

    int ret = pwar_cli_run(&config);
    return ret;
}
