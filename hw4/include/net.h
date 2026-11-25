#ifndef HW4_NET_H
#define HW4_NET_H

#include <signal.h>
#include <stddef.h>
#include "port.h"

#define MAX_WORKERS (MAX_SHIPS + MAX_WAREHOUSES)
#define MAX_OBSERVERS 32
#define NET_PACKET_SIZE 16384
#define NET_TEXT_SIZE 1024

enum { PHASE_WAREHOUSES, PHASE_SHIPS };
enum {
    NET_HELLO_WORKER = 1,
    NET_HELLO_OBSERVER,
    NET_STEP,
    NET_STATE,
    NET_EVENT,
    NET_STOP,
    NET_ERROR
};

typedef struct {
    int type;
    int index;
    int count;
    int phase;
    Port port;
    char text[NET_TEXT_SIZE];
} Message;

extern volatile sig_atomic_t g_stop;

void net_setup_signals(void);
int net_listen(const char* ip, int port);
int net_connect(const char* ip, int port);
int net_set_timeout(int fd, int seconds);
int net_pack_message(const Message* message, unsigned char* buffer, size_t capacity);
int net_send_message(int fd, const Message* message);
int net_recv_message(int fd, Message* message);

#endif
