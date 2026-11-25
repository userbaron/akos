#include "../include/net.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: %s IP PORT [--render=full|changes]\n", argv[0]); return 0;
    }
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s IP PORT [--render=full|changes]\n", argv[0]); return 1;
    }
    int render = -1;
    if (argc == 4) {
        if (!strcmp(argv[3], "--render=full")) render = RENDER_FULL;
        else if (!strcmp(argv[3], "--render=changes")) render = RENDER_CHANGES;
        else { fprintf(stderr, "Render mode must be full or changes\n"); return 1; }
    }
    char* end;
    errno = 0;
    long port = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || port < 1 || port > 65535) {
        fprintf(stderr, "Invalid TCP port\n"); return 1;
    }
    net_setup_signals();
    int server = net_connect(argv[1], (int)port);
    if (server < 0) { perror("TCP connect"); return 1; }
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = NET_HELLO_OBSERVER;
    if (net_send_message(server, &message) != 0) { close(server); return 1; }
    int last_tick = -1;
    int result = 0;
    while (!g_stop) {
        int status = net_recv_message(server, &message);
        if (status == 0) { printf("Server closed connection\n"); break; }
        if (status < 0) {
            if (!g_stop) { fprintf(stderr, "Observer connection or protocol error\n"); result = 1; }
            break;
        }
        if (message.type == NET_EVENT) printf("%s", message.text);
        else if (message.type == NET_STATE) {
            int mode = render < 0 ? message.port.render_mode : render;
            if (mode == RENDER_FULL && (last_tick < 0 ||
                message.port.tick % 10 == 0 || !message.port.running) && last_tick != message.port.tick) {
                port_render(&message.port);
                last_tick = message.port.tick;
            }
        } else if (message.type == NET_STOP) { printf("%s", message.text); break; }
        else if (message.type == NET_ERROR) { fprintf(stderr, "%s\n", message.text); result = 1; break; }
        else { fprintf(stderr, "Unexpected observer message\n"); result = 1; break; }
        fflush(stdout);
    }
    close(server);
    return result;
}
