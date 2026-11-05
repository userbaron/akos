#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include <semaphore.h>
#include "../include/port.h"
#include "../include/log.h"
#include "../include/dispatcher.h"

static _Atomic sig_atomic_t g_stop = 0;  /* общий с SIGINT-обработчиком атомарный флаг */
static Port g_port;                       /* общий порт для рабочих потоков и тикера */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  /* защита g_port */

#define MAX_WORKERS (MAX_SHIPS + MAX_WAREHOUSES)

/* Рабочий поток ждёт свой семафор, затем обрабатывает закреплённые сущности. */
typedef struct {
    pthread_t thread;
    sem_t start;
    int index;
} Worker;

enum {
    PHASE_WAREHOUSES,
    PHASE_SHIPS
};

static Worker g_workers[MAX_WORKERS];
static int g_thread_count = 0;  /* количество рабочих потоков */
static int g_started = 0;       /* сколько потоков удалось создать */
static int g_phase = PHASE_WAREHOUSES;
static sem_t g_done;            /* подтверждения завершения текущей фазы */

/* Обработчик Ctrl+C: только выставляет флаг, никаких printf внутри. */
static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

/* Сигнал может прервать sem_wait, поэтому в этом случае повторяем ожидание. */
static void wait_semaphore(sem_t* sem) {
    while (sem_wait(sem) != 0) {
        if (errno != EINTR) {
            perror("sem_wait");
            exit(1);
        }
    }
}

/* Склады и суда распределены между потоками по индексам с шагом g_thread_count. */
static void* worker_thread(void* arg) {
    Worker* worker = arg;
    while (1) {
        wait_semaphore(&worker->start);
        pthread_mutex_lock(&g_lock);
        int running = g_port.running;
        int phase = g_phase;
        pthread_mutex_unlock(&g_lock);
        if (!running) break;

        /* Мьютекс держим только на время одного шага одной сущности. */
        int total = g_port.warehouse_count + g_port.ship_count;
        for (int i = worker->index; i < total; i += g_thread_count) {
            int is_warehouse = i < g_port.warehouse_count;
            if ((phase == PHASE_WAREHOUSES) != is_warehouse) continue;
            pthread_mutex_lock(&g_lock);
            if (is_warehouse) port_warehouse_step(&g_port, i);
            else dispatcher_ship_step(&g_port, i - g_port.warehouse_count);
            pthread_mutex_unlock(&g_lock);
        }
        sem_post(&g_done);
    }
    return NULL;
}

/* Будим даже ожидающие потоки, дожидаемся их и освобождаем семафоры. */
static void stop_workers(void) {
    pthread_mutex_lock(&g_lock);
    g_port.running = 0;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < g_started; i++) sem_post(&g_workers[i].start);
    for (int i = 0; i < g_started; i++) {
        pthread_join(g_workers[i].thread, NULL);
        sem_destroy(&g_workers[i].start);
    }
    sem_destroy(&g_done);
    g_started = 0;
}

/* При ошибке создания убираем уже запущенные потоки. */
static int start_workers(void) {
    if (sem_init(&g_done, 0, 0) != 0) {
        perror("sem_init");
        return -1;
    }
    for (int i = 0; i < g_thread_count; i++) {
        Worker* worker = &g_workers[i];
        worker->index = i;
        if (sem_init(&worker->start, 0, 0) != 0) {
            perror("sem_init");
            break;
        }
        int err = pthread_create(&worker->thread, NULL, worker_thread, worker);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            sem_destroy(&worker->start);
            break;
        }
        g_started++;
    }
    if (g_started != g_thread_count) {
        stop_workers();
        return -1;
    }
    return 0;
}

/* Главный поток задаёт фазу и ждёт по одному подтверждению от каждого рабочего. */
static void run_workers(int phase) {
    pthread_mutex_lock(&g_lock);
    g_phase = phase;
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < g_started; i++) sem_post(&g_workers[i].start);
    for (int i = 0; i < g_started; i++) wait_semaphore(&g_done);
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
            printf("Usage: %s [config.dat] [--render=full|changes] [--threads=N]\n", argv[0]);
            return 0;
        }
        else if (!strncmp(argv[i], "--render=", 9)) {
            render_override = argv[i] + 9;
        }
        /* Число рабочих потоков: --threads=N, --threads N или -t N. */
        else if (!strncmp(argv[i], "--threads=", 10) ||
                 !strcmp(argv[i], "--threads") || !strcmp(argv[i], "-t")) {
            const char* value;
            if (!strncmp(argv[i], "--threads=", 10)) value = argv[i] + 10;
            else if (i + 1 < argc) value = argv[++i];
            else {
                fprintf(stderr, "Missing thread count\n");
                return 1;
            }
            char* end;
            errno = 0;
            long count = strtol(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' || count < 1 || count > MAX_WORKERS) {
                fprintf(stderr, "Thread count must be between 1 and %d\n", MAX_WORKERS);
                return 1;
            }
            g_thread_count = (int)count;
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

    /* По умолчанию каждому судну и складу соответствует свой рабочий поток. */
    if (g_thread_count == 0) {
        g_thread_count = g_port.ship_count + g_port.warehouse_count;
        if (g_thread_count == 0) g_thread_count = 1;
    }
    if (start_workers() != 0) {
        log_close();
        return 1;
    }

    /* Тикер запускаем только в режиме full — в changes он не нужен. */
    pthread_t th;
    int ticker_started = 0;
    if (g_port.render_mode == RENDER_FULL) {
        int err = pthread_create(&th, NULL, ticker_thread, NULL);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            stop_workers();
            log_close();
            return 1;
        }
        ticker_started = 1;
    }

    /* Главный цикл симуляции: такт = 100 мс. */
    while (g_port.running) {
        pthread_mutex_lock(&g_lock);
        g_port.tick++;
        pthread_mutex_unlock(&g_lock);

        /* Каждый склад и судно делают ровно один шаг за такт; склады идут первыми. */
        run_workers(PHASE_WAREHOUSES);
        run_workers(PHASE_SHIPS);

        pthread_mutex_lock(&g_lock);
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

    /* Останавливаем рабочие потоки и дожидаемся тикера, если он был запущен. */
    stop_workers();
    if (ticker_started) pthread_join(th, NULL);
    port_log(&g_port, "Simulation finished at tick %d\n", g_port.tick);
    log_close();
    pthread_mutex_destroy(&g_lock);
    return 0;
}
