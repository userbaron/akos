#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include "../include/port.h"
#include "../include/log.h"

static volatile sig_atomic_t g_stop = 0;  /* флаг, выставляемый SIGINT-обработчиком */
static Port g_port;                       /* глобальный порт — общий для главного потока и тикера */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  /* защита g_port */

/* Обработчик Ctrl+C: только выставляет флаг, никаких printf внутри. */
static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Поток-тикер: раз в секунду печатает снимок состояния порта. */
static void* ticker_thread(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_lock);
        if (!g_port.running) {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        port_render(&g_port);
        pthread_mutex_unlock(&g_lock);
        sleep(1);
    }
    return NULL;
}

int main(int argc, char** argv) {
    const char* cfg = "../data/port.dat";
    const char* render_override = NULL;

    /* Разбор аргументов командной строки. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: %s [config.dat] [--render=full|changes]\n", argv[0]);
            return 0;
        }
        else if (!strncmp(argv[i], "--render=", 9)) {
            render_override = argv[i] + 9;
        }
        else {
            cfg = argv[i];
        }
    }

    /* Загружаем порт из файла — это обнуляет структуру, поэтому override делаем ПОСЛЕ. */
    if (port_load(&g_port, cfg) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", cfg);
        return 1;
    }

    /* CLI-аргумент перекрывает то, что было в файле. */
    if (render_override) {
        if (!strcmp(render_override, "full")) g_port.render_mode = RENDER_FULL;
        else if (!strcmp(render_override, "changes")) g_port.render_mode = RENDER_CHANGES;
    }

    log_open("../logs/port.log");
    signal(SIGINT, on_sigint);

    g_port.tick = 0;

    /* Тикер запускаем только в режиме full — в changes он не нужен. */
    pthread_t th;
    int ticker_started = 0;
    if (g_port.render_mode == RENDER_FULL) {
        pthread_create(&th, NULL, ticker_thread, NULL);
        ticker_started = 1;
    }

    /* Главный цикл симуляции: такт = 100 мс. */
    while (g_port.running) {
        pthread_mutex_lock(&g_lock);
        g_port.tick++;
        port_step(&g_port);

        /* Три условия завершения: SIGINT, лимит тактов, все суда обслужены. */
        if (g_stop) {
            port_log(&g_port, "\n[SIGINT] stopping simulation at tick %d\n", g_port.tick);
            g_port.running = 0;
        }
        if (g_port.max_ticks > 0 && g_port.tick >= g_port.max_ticks) {
            port_log(&g_port, "[t=%d] max_ticks reached\n", g_port.tick);
            g_port.running = 0;
        }
        if (port_all_done(&g_port)) {
            port_log(&g_port, "[t=%d] all ships done\n", g_port.tick);
            g_port.running = 0;
        }
        pthread_mutex_unlock(&g_lock);

        usleep(100 * 1000);
    }

    /* Дожидаемся тикера, если он был запущен. */
    if (ticker_started) pthread_join(th, NULL);
    port_log(&g_port, "Simulation finished at tick %d\n", g_port.tick);
    log_close();
    return 0;
}
