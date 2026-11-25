#include "../include/net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    g_stop = sig;
}

void net_setup_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

static int fill_address(struct sockaddr_in* address, const char* ip, int port) {
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    if (port < 1 || port > 65535 || inet_pton(AF_INET, ip, &address->sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    address->sin_port = htons((unsigned short)port);
    return 0;
}

int net_listen(const char* ip, int port) {
    struct sockaddr_in address;
    if (fill_address(&address, ip, port) != 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &reuse, sizeof(reuse)) != 0 ||
        bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(fd, 128) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int net_connect(const char* ip, int port) {
    struct sockaddr_in address;
    if (fill_address(&address, ip, port) != 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int no_delay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0 ||
        connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

int net_set_timeout(int fd, int seconds) {
    struct timeval timeout = { seconds, 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/* Один и тот же формат используется в TCP и во внутренних pipe рабочего. */
typedef struct {
    unsigned char* data;
    size_t size;
    size_t pos;
    int failed;
} Cursor;

static void put_bytes(Cursor* c, const void* data, size_t size) {
    if (c->failed || size > c->size - c->pos) { c->failed = 1; return; }
    memcpy(c->data + c->pos, data, size);
    c->pos += size;
}

static void get_bytes(Cursor* c, void* data, size_t size) {
    if (c->failed || size > c->size - c->pos) { c->failed = 1; return; }
    memcpy(data, c->data + c->pos, size);
    c->pos += size;
}

static void put_int(Cursor* c, int value) {
    uint32_t number = htonl((uint32_t)(int32_t)value);
    put_bytes(c, &number, sizeof(number));
}

static int get_int(Cursor* c) {
    uint32_t number = 0;
    get_bytes(c, &number, sizeof(number));
    return (int32_t)ntohl(number);
}

static int counts_valid(const Port* p) {
    return p->berth_count >= 0 && p->berth_count <= MAX_BERTHS &&
           p->tug_count >= 0 && p->tug_count <= MAX_TUGS &&
           p->crane_count >= 0 && p->crane_count <= MAX_CRANES &&
           p->warehouse_count >= 0 && p->warehouse_count <= MAX_WAREHOUSES &&
           p->ship_count >= 0 && p->ship_count <= MAX_SHIPS;
}

static int cargo_valid(CargoType cargo) {
    return cargo >= CARGO_DRY && cargo <= CARGO_CONTAINER;
}

static int port_valid(const Port* p) {
    if (!counts_valid(p) || p->render_mode < RENDER_FULL || p->render_mode > RENDER_CHANGES ||
        p->tick < 0 || p->max_ticks < 0 || (p->running != 0 && p->running != 1)) return 0;
    for (int i = 0; i < p->berth_count; i++)
        if (!cargo_valid(p->berths[i].cargo) || p->berths[i].ship_id < -1) return 0;
    for (int i = 0; i < p->tug_count; i++)
        if (p->tugs[i].ship_id < -1) return 0;
    for (int i = 0; i < p->crane_count; i++)
        if (!cargo_valid(p->cranes[i].type) || p->cranes[i].ship_id < -1 || p->cranes[i].rate < 1) return 0;
    for (int i = 0; i < p->warehouse_count; i++) {
        const Warehouse* w = &p->warehouses[i];
        if (!cargo_valid(w->type) || w->capacity < 0 || w->used < 0 || w->used > w->capacity ||
            w->drain_every < 1 || w->drain_amount < 1) return 0;
    }
    for (int i = 0; i < p->ship_count; i++) {
        const Ship* s = &p->ships[i];
        if (!cargo_valid(s->cargo) || s->state < SHIP_ARRIVING || s->state > SHIP_DONE ||
            !memchr(s->name, '\0', sizeof(s->name)) || s->cargo_volume < 0 ||
            s->cargo_initial < s->cargo_volume || s->berth_id < -1 || s->tug_id < -1 ||
            s->crane_id < -1 || s->warehouse_id < -1) return 0;
    }
    return 1;
}

/* Структуры не отправляем как набор байтов: размер int и выравнивание не часть протокола. */
static void put_port(Cursor* c, const Port* p) {
    put_int(c, p->berth_count);
    put_int(c, p->tug_count);
    put_int(c, p->crane_count);
    put_int(c, p->warehouse_count);
    put_int(c, p->ship_count);
    put_int(c, p->enter_ticks);
    put_int(c, p->leave_ticks);
    put_int(c, p->max_ticks);
    put_int(c, p->render_mode);
    put_int(c, p->tick);
    put_int(c, p->running);
    for (int i = 0; i < p->berth_count; i++) {
        put_int(c, p->berths[i].id);
        put_int(c, p->berths[i].cargo);
        put_int(c, p->berths[i].ship_id);
    }
    for (int i = 0; i < p->tug_count; i++) {
        put_int(c, p->tugs[i].id);
        put_int(c, p->tugs[i].ship_id);
    }
    for (int i = 0; i < p->crane_count; i++) {
        put_int(c, p->cranes[i].id);
        put_int(c, p->cranes[i].type);
        put_int(c, p->cranes[i].ship_id);
        put_int(c, p->cranes[i].rate);
    }
    for (int i = 0; i < p->warehouse_count; i++) {
        const Warehouse* w = &p->warehouses[i];
        put_int(c, w->id);
        put_int(c, w->type);
        put_int(c, w->capacity);
        put_int(c, w->used);
        put_int(c, w->drain_every);
        put_int(c, w->drain_amount);
        put_int(c, w->drain_counter);
    }
    for (int i = 0; i < p->ship_count; i++) {
        const Ship* s = &p->ships[i];
        put_int(c, s->id);
        put_bytes(c, s->name, sizeof(s->name));
        put_int(c, s->cargo);
        put_int(c, s->cargo_volume);
        put_int(c, s->cargo_initial);
        put_int(c, s->arrive_tick);
        put_int(c, s->state);
        put_int(c, s->berth_id);
        put_int(c, s->tug_id);
        put_int(c, s->crane_id);
        put_int(c, s->warehouse_id);
        put_int(c, s->stage_ticks);
    }
}

static void get_port(Cursor* c, Port* p) {
    p->berth_count = get_int(c);
    p->tug_count = get_int(c);
    p->crane_count = get_int(c);
    p->warehouse_count = get_int(c);
    p->ship_count = get_int(c);
    if (c->failed || !counts_valid(p)) { c->failed = 1; return; }
    p->enter_ticks = get_int(c);
    p->leave_ticks = get_int(c);
    p->max_ticks = get_int(c);
    p->render_mode = (RenderMode)get_int(c);
    p->tick = get_int(c);
    p->running = get_int(c);
    for (int i = 0; i < p->berth_count; i++) {
        p->berths[i].id = get_int(c);
        p->berths[i].cargo = (CargoType)get_int(c);
        p->berths[i].ship_id = get_int(c);
    }
    for (int i = 0; i < p->tug_count; i++) {
        p->tugs[i].id = get_int(c);
        p->tugs[i].ship_id = get_int(c);
    }
    for (int i = 0; i < p->crane_count; i++) {
        p->cranes[i].id = get_int(c);
        p->cranes[i].type = (CargoType)get_int(c);
        p->cranes[i].ship_id = get_int(c);
        p->cranes[i].rate = get_int(c);
    }
    for (int i = 0; i < p->warehouse_count; i++) {
        Warehouse* w = &p->warehouses[i];
        w->id = get_int(c);
        w->type = (CargoType)get_int(c);
        w->capacity = get_int(c);
        w->used = get_int(c);
        w->drain_every = get_int(c);
        w->drain_amount = get_int(c);
        w->drain_counter = get_int(c);
    }
    for (int i = 0; i < p->ship_count; i++) {
        Ship* s = &p->ships[i];
        s->id = get_int(c);
        get_bytes(c, s->name, sizeof(s->name));
        s->cargo = (CargoType)get_int(c);
        s->cargo_volume = get_int(c);
        s->cargo_initial = get_int(c);
        s->arrive_tick = get_int(c);
        s->state = (ShipState)get_int(c);
        s->berth_id = get_int(c);
        s->tug_id = get_int(c);
        s->crane_id = get_int(c);
        s->warehouse_id = get_int(c);
        s->stage_ticks = get_int(c);
    }
    if (!port_valid(p)) c->failed = 1;
}

int net_pack_message(const Message* message, unsigned char* buffer, size_t capacity) {
    if (capacity > NET_PACKET_SIZE) capacity = NET_PACKET_SIZE;
    if (capacity < 20 || message->type < NET_HELLO_WORKER || message->type > NET_ERROR) {
        errno = EINVAL;
        return -1;
    }
    Cursor c = { buffer, capacity, 4, 0 };
    put_int(&c, message->type);
    put_int(&c, message->index);
    put_int(&c, message->count);
    put_int(&c, message->phase);
    if (message->type == NET_STEP || message->type == NET_STATE) {
        if (!port_valid(&message->port)) { errno = EPROTO; return -1; }
        put_port(&c, &message->port);
    } else if (message->type == NET_EVENT || message->type == NET_STOP || message->type == NET_ERROR) {
        size_t length = strnlen(message->text, sizeof(message->text));
        if (length == sizeof(message->text)) { errno = EPROTO; return -1; }
        put_int(&c, (int)length);
        put_bytes(&c, message->text, length);
    }
    if (c.failed) { errno = EMSGSIZE; return -1; }
    uint32_t length = htonl((uint32_t)(c.pos - 4));
    memcpy(buffer, &length, sizeof(length));
    return (int)c.pos;
}

static int write_all(int fd, const unsigned char* data, size_t size) {
    size_t done = 0;
    while (done < size) {
        if (g_stop) { errno = EINTR; return -1; }
        ssize_t n = write(fd, data + done, size - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR && !g_stop) continue;
        else {
            if (n == 0) errno = EPIPE;
            return -1;
        }
    }
    return 0;
}

static long long monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* 0 означает EOF до первого байта, обрыв посреди сообщения является ошибкой. */
static int read_all(int fd, unsigned char* data, size_t size) {
    struct timeval timeout;
    socklen_t timeout_size = sizeof(timeout);
    long long deadline = 0;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &timeout_size) == 0) {
        if (timeout.tv_sec || timeout.tv_usec) {
            long long now = monotonic_ms();
            if (now < 0) return -1;
            deadline = now + (long long)timeout.tv_sec * 1000 + (timeout.tv_usec + 999) / 1000;
        }
    } else if (errno != ENOTSOCK) return -1;

    size_t done = 0;
    while (done < size) {
        if (g_stop) { errno = EINTR; return -1; }
        int wait_ms = 100;
        if (deadline) {
            long long now = monotonic_ms();
            if (now < 0) return -1;
            long long remaining = deadline - now;
            if (remaining <= 0) { errno = EAGAIN; return -1; }
            if (remaining < wait_ms) wait_ms = (int)remaining;
        }
        /* Короткое ожидание не даст пропустить сигнал перед блокирующим read. */
        struct pollfd waiting = { fd, POLLIN, 0 };
        int ready = poll(&waiting, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR && !g_stop) continue;
            return -1;
        }
        if (!ready) continue;
        if (g_stop) { errno = EINTR; return -1; }
        if (waiting.revents & POLLNVAL) { errno = EBADF; return -1; }
        ssize_t n = read(fd, data + done, size - done);
        if (n > 0) done += (size_t)n;
        else if (n == 0) {
            if (!done) return 0;
            errno = EPROTO;
            return -1;
        } else if (errno == EINTR && !g_stop) continue;
        else return -1;
    }
    return 1;
}

int net_send_message(int fd, const Message* message) {
    unsigned char buffer[NET_PACKET_SIZE];
    int size = net_pack_message(message, buffer, sizeof(buffer));
    if (size < 0) return -1;
    return write_all(fd, buffer, (size_t)size);
}

int net_recv_message(int fd, Message* message) {
    unsigned char buffer[NET_PACKET_SIZE];
    uint32_t length;
    int result = read_all(fd, (unsigned char*)&length, sizeof(length));
    if (result <= 0) return result;
    size_t size = ntohl(length);
    if (size < 16 || size > NET_PACKET_SIZE - 4) { errno = EPROTO; return -1; }
    result = read_all(fd, buffer, size);
    if (result <= 0) { if (!result) errno = EPROTO; return -1; }
    memset(message, 0, sizeof(*message));
    Cursor c = { buffer, size, 0, 0 };
    message->type = get_int(&c);
    message->index = get_int(&c);
    message->count = get_int(&c);
    message->phase = get_int(&c);
    if (message->type == NET_STEP || message->type == NET_STATE) get_port(&c, &message->port);
    else if (message->type == NET_EVENT || message->type == NET_STOP || message->type == NET_ERROR) {
        int text_size = get_int(&c);
        if (text_size < 0 || text_size >= NET_TEXT_SIZE) c.failed = 1;
        else {
            get_bytes(&c, message->text, (size_t)text_size);
            message->text[text_size] = '\0';
        }
    } else if (message->type != NET_HELLO_WORKER && message->type != NET_HELLO_OBSERVER) c.failed = 1;
    if (c.failed || c.pos != c.size) { errno = EPROTO; return -1; }
    return 1;
}
