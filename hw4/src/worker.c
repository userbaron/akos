#include "../include/net.h"
#include "../include/dispatcher.h"
#include "../include/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Дочерний процесс считает модель, родитель связывает его с TCP-сервером. */
static int simulation(int input, int output, int index, int count) {
    log_set_channel(output);
    Message message;
    int status = 0;
    while (!g_stop && (status = net_recv_message(input, &message)) == 1) {
        if (message.type == NET_STOP) return 0;
        if (message.type != NET_STEP || message.index != index || message.count != count ||
            (message.phase != PHASE_WAREHOUSES && message.phase != PHASE_SHIPS)) return 1;
        Port* p = &message.port;
        for (int i = index; i < p->warehouse_count + p->ship_count; i += count) {
            int warehouse = i < p->warehouse_count;
            if ((message.phase == PHASE_WAREHOUSES) != warehouse) continue;
            if (warehouse) port_warehouse_step(p, i);
            else dispatcher_ship_step(p, i - p->warehouse_count);
        }
        message.type = NET_STATE;
        if (net_send_message(output, &message) != 0) return g_stop ? 0 : 1;
    }
    return g_stop || status == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: %s IP PORT INDEX\n", argv[0]); return 0;
    }
    if (argc != 4) { fprintf(stderr, "Usage: %s IP PORT INDEX\n", argv[0]); return 1; }
    char* end;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid TCP port\n"); return 1;
    }
    errno = 0;
    long index = strtol(argv[3], &end, 10);
    if (errno || end == argv[3] || *end || index < 0 || index >= MAX_WORKERS) {
        fprintf(stderr, "Invalid worker index\n"); return 1;
    }
    net_setup_signals();
    int server = net_connect(argv[1], (int)port);
    if (server < 0) { perror("TCP connect"); return 1; }
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = NET_HELLO_WORKER;
    message.index = (int)index;
    if (net_send_message(server, &message) != 0 || net_recv_message(server, &message) != 1) {
        fprintf(stderr, "Worker handshake failed\n"); close(server); return 1;
    }
    if (message.type == NET_ERROR) {
        fprintf(stderr, "%s\n", message.text); close(server); return 1;
    }
    if (message.type != NET_STATE || message.index != index || message.count < 1 ||
        message.count > MAX_WORKERS || index >= message.count) {
        fprintf(stderr, "Invalid worker handshake\n"); close(server); return 1;
    }
    int count = message.count;
    int commands[2], replies[2];
    if (pipe(commands) != 0) { perror("pipe"); close(server); return 1; }
    if (pipe(replies) != 0) {
        perror("pipe"); close(commands[0]); close(commands[1]); close(server); return 1;
    }
    pid_t child = fork();
    if (child < 0) {
        perror("fork"); close(commands[0]); close(commands[1]);
        close(replies[0]); close(replies[1]); close(server); return 1;
    }
    if (child == 0) {
        close(server);
        close(commands[1]);
        close(replies[0]);
        int result = simulation(commands[0], replies[1], (int)index, count);
        close(commands[0]); close(replies[1]);
        _exit(result);
    }
    close(commands[0]);
    close(replies[1]);
    printf("Worker %ld connected to %s:%ld\n", index, argv[1], port);
    fflush(stdout);
    int result = 0;
    while (!g_stop) {
        int status = net_recv_message(server, &message);
        if (status == 0) { printf("Server closed connection\n"); break; }
        if (status < 0) { if (!g_stop) result = 1; break; }
        if (message.type == NET_STOP) { printf("%s", message.text); break; }
        if (message.type != NET_STEP || net_send_message(commands[1], &message) != 0) {
            result = 1; break;
        }
        do {
            if (net_recv_message(replies[0], &message) != 1 ||
                (message.type != NET_EVENT && message.type != NET_STATE) ||
                net_send_message(server, &message) != 0) { result = 1; break; }
        } while (!g_stop && message.type != NET_STATE);
        if (result) break;
    }
    close(server);
    close(commands[1]);
    close(replies[0]);
    kill(child, SIGTERM);
    while (waitpid(child, NULL, 0) < 0) if (errno != EINTR) break;
    if (g_stop) return 0;
    if (result) fprintf(stderr, "Worker connection or pipe error\n");
    return result;
}
