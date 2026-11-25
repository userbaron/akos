#include "../include/net.h"
#include "../include/log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PENDING 16
#define OBSERVER_BUFFER (64 * 1024)

typedef struct {
    int fd;
    int sent;
    int size;
    unsigned char data[OBSERVER_BUFFER];
} Observer;

typedef struct {
    int fd;
    time_t connected;
} Pending;

static Port g_port;
static int g_count;
static int g_workers[MAX_WORKERS];
static Observer g_observers[MAX_OBSERVERS];
static Pending g_pending[MAX_PENDING];
static int g_listener = -1;
static int g_failed = 0;

/* У каждого наблюдателя своя очередь. Медленный клиент не задерживает порт. */
static void flush_observer(Observer* observer) {
    while (observer->fd >= 0 && observer->sent < observer->size) {
        ssize_t n = send(observer->fd, observer->data + observer->sent,
                         observer->size - observer->sent, MSG_DONTWAIT);
        if (n > 0) observer->sent += (int)n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        else {
            close(observer->fd);
            observer->fd = -1;
        }
    }
    if (observer->sent == observer->size) observer->sent = observer->size = 0;
}

static void queue_message(Observer* observer, const Message* message) {
    unsigned char data[NET_PACKET_SIZE];
    int n = net_pack_message(message, data, sizeof(data));
    if (n < 0 || observer->fd < 0) return;
    flush_observer(observer);
    if (observer->fd < 0) return;
    if (observer->sent) {
        memmove(observer->data, observer->data + observer->sent,
                observer->size - observer->sent);
        observer->size -= observer->sent;
        observer->sent = 0;
    }
    if (n > OBSERVER_BUFFER - observer->size) {
        printf("Slow observer disconnected\n");
        close(observer->fd);
        observer->fd = -1;
        return;
    }
    memcpy(observer->data + observer->size, data, n);
    observer->size += n;
    flush_observer(observer);
}

static void broadcast(const Message* message) {
    for (int i = 0; i < MAX_OBSERVERS; i++)
        if (g_observers[i].fd >= 0) queue_message(&g_observers[i], message);
}

static void event(const char* fmt, ...) {
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = NET_EVENT;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message.text, sizeof(message.text), fmt, ap);
    va_end(ap);
    log_write("%s", message.text);
    broadcast(&message);
}

static void snapshot(void) {
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = NET_STATE;
    message.count = g_count;
    message.port = g_port;
    broadcast(&message);
}

static void reject_client(int fd, const char* reason) {
    Message message;
    memset(&message, 0, sizeof(message));
    message.type = NET_ERROR;
    snprintf(message.text, sizeof(message.text), "%s", reason);
    net_send_message(fd, &message);
    close(fd);
}

/* Первое сообщение определяет роль клиента. */
static void register_client(int fd, const Message* hello) {
    Message answer;
    memset(&answer, 0, sizeof(answer));
    answer.type = NET_STATE;
    answer.count = g_count;
    answer.port = g_port;
    if (hello->type == NET_HELLO_WORKER) {
        int i = hello->index;
        if (i < 0 || i >= g_count) {
            reject_client(fd, "Worker index is outside the configured range");
            return;
        }
        if (g_workers[i] >= 0) {
            reject_client(fd, "Worker index is already connected");
            return;
        }
        answer.index = i;
        if (net_send_message(fd, &answer) != 0) { close(fd); return; }
        g_workers[i] = fd;
        printf("Worker %d connected\n", i);
    } else if (hello->type == NET_HELLO_OBSERVER) {
        for (int i = 0; i < MAX_OBSERVERS; i++) {
            Observer* observer = &g_observers[i];
            if (observer->fd >= 0) continue;
            observer->fd = fd;
            observer->sent = observer->size = 0;
            int size = 8192;
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
            queue_message(observer, &answer);  /* Состояние видно сразу после подключения. */
            printf("Observer connected\n");
            return;
        }
        reject_client(fd, "Too many observers (maximum 32)");
    } else reject_client(fd, "Unknown client role");
}

/* Не ждём hello внутри accept: подключение без данных не блокирует симуляцию. */
static void service_clients(int active_worker) {
    for (int attempts = 0; attempts < MAX_PENDING; attempts++) {
        int fd = accept(g_listener, NULL, NULL);
        if (fd < 0) break;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0 ||
            net_set_timeout(fd, 5) != 0) { close(fd); continue; }
        int yes = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        int slot = -1;
        for (int i = 0; i < MAX_PENDING; i++)
            if (g_pending[i].fd < 0) { slot = i; break; }
        if (slot < 0) { close(fd); continue; }
        g_pending[slot].fd = fd;
        g_pending[slot].connected = time(NULL);
    }
    for (int i = 0; i < MAX_PENDING; i++) {
        int fd = g_pending[i].fd;
        if (fd < 0) continue;
        unsigned char data[20];
        ssize_t n = recv(fd, data, sizeof(data), MSG_PEEK | MSG_DONTWAIT);
        int remove = n == 0 || time(NULL) - g_pending[i].connected >= 5;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) remove = 1;
        if (n >= 4) {
            uint32_t length;
            memcpy(&length, data, sizeof(length));
            if (ntohl(length) != 16) remove = 1;
        }
        if (!remove && n == (ssize_t)sizeof(data)) {
            Message hello;
            if (net_recv_message(fd, &hello) == 1) register_client(fd, &hello);
            else close(fd);
            g_pending[i].fd = -1;
        } else if (remove) {
            close(fd);
            g_pending[i].fd = -1;
        }
    }
    for (int i = 0; i < MAX_OBSERVERS; i++) {
        Observer* observer = &g_observers[i];
        if (observer->fd < 0) continue;
        char byte;
        ssize_t n = recv(observer->fd, &byte, 1, MSG_DONTWAIT);
        /* Наблюдатель только читает. Отключение не влияет на остальных. */
        if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(observer->fd);
            observer->fd = -1;
        } else flush_observer(observer);
    }
    for (int i = 0; i < g_count; i++) {
        if (g_workers[i] < 0 || i == active_worker) continue;
        char byte;
        ssize_t n = recv(g_workers[i], &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(g_workers[i]);
            g_workers[i] = -1;
            event("Worker %d disconnected; stopping simulation\n", i);
            g_failed = 1;
        }
    }
}

static int all_connected(void) {
    for (int i = 0; i < g_count; i++) if (g_workers[i] < 0) return 0;
    return 1;
}

static int run_workers(int phase) {
    for (int i = 0; i < g_count && !g_stop && !g_failed; i++) {
        int has_work = 0;
        for (int k = i; k < g_port.warehouse_count + g_port.ship_count; k += g_count)
            if ((phase == PHASE_WAREHOUSES) == (k < g_port.warehouse_count)) has_work = 1;
        if (!has_work) continue;
        Message message;
        memset(&message, 0, sizeof(message));
        message.type = NET_STEP;
        message.index = i;
        message.count = g_count;
        message.phase = phase;
        message.port = g_port;
        service_clients(-1);
        if (g_failed || g_stop) return -1;
        if (net_send_message(g_workers[i], &message) != 0) goto failed;
        while (!g_stop) {
            if (net_recv_message(g_workers[i], &message) != 1) goto failed;
            if (message.type == NET_EVENT) {
                log_write("%s", message.text);
                message.index = i;
                broadcast(&message);
            } else if (message.type == NET_STATE && message.index == i &&
                       message.port.tick == g_port.tick &&
                       message.port.ship_count == g_port.ship_count &&
                       message.port.warehouse_count == g_port.warehouse_count &&
                       message.port.berth_count == g_port.berth_count &&
                       message.port.crane_count == g_port.crane_count &&
                       message.port.tug_count == g_port.tug_count) {
                g_port = message.port;
                break;
            } else goto failed;
            service_clients(i);
        }
        service_clients(-1);
        continue;
failed:
        if (!g_stop) {
            event("Worker %d: connection or protocol error; stopping simulation\n", i);
            g_failed = 1;
        }
        return -1;
    }
    return g_stop || g_failed ? -1 : 0;
}

static int number(const char* value, int minimum, int maximum) {
    char* end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || end == value || *end || n < minimum || n > maximum) return -1;
    return (int)n;
}

int main(int argc, char** argv) {
    for (int i = 0; i < MAX_WORKERS; i++) g_workers[i] = -1;
    for (int i = 0; i < MAX_OBSERVERS; i++) g_observers[i].fd = -1;
    for (int i = 0; i < MAX_PENDING; i++) g_pending[i].fd = -1;
    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: %s IP PORT [config.dat] [--processes=N] [--render=full|changes]\n", argv[0]);
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "Usage: %s IP PORT [config.dat] [--processes=N] [--render=full|changes]\n", argv[0]); return 1; }
    int port = number(argv[2], 1, 65535);
    if (port < 0) { fprintf(stderr, "Invalid TCP port\n"); return 1; }
    const char* cfg = "../data/port.dat";
    const char* render = NULL;
    for (int i = 3; i < argc; i++) {
        if (!strncmp(argv[i], "--processes=", 12) || !strcmp(argv[i], "--processes") || !strcmp(argv[i], "-p")) {
            const char* value;
            if (!strncmp(argv[i], "--processes=", 12)) value = argv[i] + 12;
            else if (i + 1 < argc) value = argv[++i];
            else { fprintf(stderr, "Missing process count\n"); return 1; }
            g_count = number(value, 1, MAX_WORKERS);
            if (g_count < 0) { fprintf(stderr, "Process count must be between 1 and %d\n", MAX_WORKERS); return 1; }
        } else if (!strncmp(argv[i], "--render=", 9)) {
            render = argv[i] + 9;
            if (strcmp(render, "full") && strcmp(render, "changes")) {
                fprintf(stderr, "Render mode must be full or changes\n"); return 1;
            }
        } else if (argv[i][0] == '-') { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
        else cfg = argv[i];
    }
    net_setup_signals();
    if (port_load(&g_port, cfg) != 0) { fprintf(stderr, "Failed to load config: %s\n", cfg); return 1; }
    if (render) g_port.render_mode = !strcmp(render, "full") ? RENDER_FULL : RENDER_CHANGES;
    if (!g_count) {
        g_count = g_port.ship_count + g_port.warehouse_count;
        if (!g_count) g_count = 1;
    }
    Message initial;
    memset(&initial, 0, sizeof(initial));
    initial.type = NET_STATE;
    initial.port = g_port;
    unsigned char check[NET_PACKET_SIZE];
    if (net_pack_message(&initial, check, sizeof(check)) < 0) {
        fprintf(stderr, "Invalid values in config: %s\n", cfg);
        return 1;
    }
    if (log_open("../logs/port.log") != 0) return 1;
    g_listener = net_listen(argv[1], port);
    if (g_listener < 0) { perror("TCP listen"); log_close(); return 1; }
    int flags = fcntl(g_listener, F_GETFL, 0);
    if (flags < 0 || fcntl(g_listener, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl listener"); close(g_listener); log_close(); return 1;
    }
    printf("Server listening on %s:%d; waiting for %d workers\n", argv[1], port, g_count);
    const char* client_ip = !strcmp(argv[1], "0.0.0.0") ? "127.0.0.1" : argv[1];
    printf("Run workers (for remote hosts use the server's network IPv4 address):\n");
    for (int i = 0; i < g_count; i++)
        printf("./hw4_worker %s %d %d\n", client_ip, port, i);
    printf("Observer command: ./hw4_observer %s %d\n", client_ip, port);
    fflush(stdout);
    while (!g_stop && !g_failed && !all_connected()) {
        service_clients(-1);
        usleep(10000);
    }
    while (!g_stop && !g_failed && g_port.running) {
        g_port.tick++;
        if (run_workers(PHASE_WAREHOUSES) != 0 || run_workers(PHASE_SHIPS) != 0) break;
        if (g_port.max_ticks > 0 && g_port.tick >= g_port.max_ticks) {
            event("[t=%d] max_ticks reached\n", g_port.tick);
            g_port.running = 0;
        }
        if (port_all_done(&g_port)) {
            event("[t=%d] all ships done\n", g_port.tick);
            g_port.running = 0;
        }
        snapshot();
        service_clients(-1);
        if (g_port.running) usleep(100 * 1000);
    }
    if (g_stop) event("[signal %d] stopping simulation at tick %d\n", (int)g_stop, g_port.tick);
    g_port.running = 0;
    for (int i = 0; i < g_port.ship_count; i++) {
        Ship* ship = &g_port.ships[i];
        if (ship->state != SHIP_DONE)
            event("ship %d stopped: state=%s, cargo=%d/%d\n", ship->id,
                  ship_state_str(ship->state), ship->cargo_volume, ship->cargo_initial);
    }
    snapshot();
    Message stop;
    memset(&stop, 0, sizeof(stop));
    stop.type = NET_STOP;
    snprintf(stop.text, sizeof(stop.text), "Simulation finished at tick %d\n", g_port.tick);
    log_write("%s", stop.text);
    broadcast(&stop);
    unsigned char data[NET_PACKET_SIZE];
    int size = net_pack_message(&stop, data, sizeof(data));
    for (int i = 0; i < g_count; i++) {
        if (g_workers[i] < 0) continue;
        if (size > 0) send(g_workers[i], data, size, MSG_DONTWAIT);
        close(g_workers[i]);
    }
    for (int attempt = 0; attempt < 20; attempt++) {
        int waiting = 0;
        for (int i = 0; i < MAX_OBSERVERS; i++) {
            flush_observer(&g_observers[i]);
            if (g_observers[i].fd >= 0 && g_observers[i].size) waiting = 1;
        }
        if (!waiting) break;
        usleep(10000);
    }
    for (int i = 0; i < MAX_OBSERVERS; i++) if (g_observers[i].fd >= 0) close(g_observers[i].fd);
    for (int i = 0; i < MAX_PENDING; i++) if (g_pending[i].fd >= 0) close(g_pending[i].fd);
    close(g_listener);
    log_close();
    return g_failed ? 1 : 0;
}
