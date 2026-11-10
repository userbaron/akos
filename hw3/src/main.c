#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include "../include/ipc.h"
#include "../include/log.h"

#ifndef INDEPENDENT_WORKERS
static pid_t g_children[MAX_WORKERS];
static int g_started = 0;

/* В первых двух вариантах главный процесс создаёт рабочих через fork. */
static int start_workers(void) {
    for (int i = 0; i < g_shared->process_count; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        if (pid == 0) {
            int result = worker_process(i);
            log_close();
            ipc_close();
            _exit(result);
        }
        g_children[g_started++] = pid;
    }
    return 0;
}
#endif

/* Будим ожидающих рабочих и ждём выхода до удаления общих объектов. */
static void stop_workers(void) {
    wait_semaphore(g_lock);
    g_shared->port.running = 0;
    sem_post(g_lock);
    for (int i = 0; i < g_shared->process_count; i++) sem_post(g_start[i]);

#ifdef INDEPENDENT_WORKERS
    /* Рабочие запущены из других терминалов, поэтому waitpid не подходит. */
    while (1) {
        int waiting = 0;
        wait_semaphore(g_lock);
        for (int i = 0; i < g_shared->process_count; i++) {
            pid_t pid = g_shared->worker_pids[i];
            if (pid && !g_shared->exited[i] && (kill(pid, 0) == 0 || errno != ESRCH))
                waiting = 1;
        }
        sem_post(g_lock);
        if (!waiting) break;
        usleep(10000);
    }
#else
    for (int i = 0; i < g_started; i++) {
        while (waitpid(g_children[i], NULL, 0) < 0) {
            if (errno != EINTR) { perror("waitpid"); break; }
        }
    }
#endif
}

/* Как в ДЗ2: сначала склады, потом суда, по одному шагу за такт. */
static int run_workers(int phase) {
    if (g_stop || wait_semaphore(g_lock) != 0) return -1;
    g_shared->phase = phase;
    sem_post(g_lock);
    for (int i = 0; i < g_shared->process_count; i++) sem_post(g_start[i]);
    for (int i = 0; i < g_shared->process_count; i++)
        if (wait_event(g_done) != 0) return -1;
    return 0;
}

int main(int argc, char** argv) {
    const char* cfg = "../data/port.dat";
    const char* render_override = NULL;
    int process_count = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: %s [config.dat] [--render=full|changes] [--processes=N]\n", argv[0]);
            return 0;
        }
        else if (!strncmp(argv[i], "--render=", 9)) {
            render_override = argv[i] + 9;
            if (strcmp(render_override, "full") && strcmp(render_override, "changes")) {
                fprintf(stderr, "Render mode must be full or changes\n");
                return 1;
            }
        }
        /* Число рабочих процессов: --processes=N, --processes N или -p N. */
        else if (!strncmp(argv[i], "--processes=", 12) ||
                 !strcmp(argv[i], "--processes") || !strcmp(argv[i], "-p")) {
            const char* value;
            if (!strncmp(argv[i], "--processes=", 12)) value = argv[i] + 12;
            else if (i + 1 < argc) value = argv[++i];
            else { fprintf(stderr, "Missing process count\n"); return 1; }
            char* end;
            errno = 0;
            long count = strtol(value, &end, 10);
            if (errno || end == value || *end || count < 1 || count > MAX_WORKERS) {
                fprintf(stderr, "Process count must be between 1 and %d\n", MAX_WORKERS);
                return 1;
            }
            process_count = (int)count;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
        else cfg = argv[i];
    }

    /* Обработчик нужен и во время создания IPC, когда семафора ожидания ещё нет. */
    setup_signals(NULL);
    Port initial;
    if (port_load(&initial, cfg) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", cfg);
        return 1;
    }
    if (render_override) {
        if (!strcmp(render_override, "full")) initial.render_mode = RENDER_FULL;
        else initial.render_mode = RENDER_CHANGES;
    }
    if (process_count == 0) {
        process_count = initial.ship_count + initial.warehouse_count;
        if (process_count == 0) process_count = 1;
    }
    if (log_open("../logs/port.log") != 0) return 1;

#ifdef UNNAMED_SEMAPHORES
    int named = 0;
#else
    int named = 1;
#endif
    if (ipc_create(&initial, process_count, named) != 0) {
        log_close();
        return 1;
    }
    if (!realpath("../logs/port.log", g_shared->log_path)) {
        perror("realpath log");
        ipc_close();
        log_close();
        return 1;
    }
    setup_signals(g_done);
    int result = 0;
    if (g_stop) goto finish;

#ifdef INDEPENDENT_WORKERS
    printf("Session %ld: waiting for %d workers. Run in other terminals:\n",
           (long)getpid(), process_count);
    for (int i = 0; i < process_count; i++)
        printf("./hw3_worker %ld %d\n", (long)getpid(), i);
    fflush(stdout);
#else
    if (start_workers() != 0) { result = 1; goto finish; }
#endif
    /* До первого такта каждый рабочий подтверждает подключение. */
    for (int i = 0; i < process_count; i++)
        if (wait_event(g_done) != 0) { if (!g_stop) result = 1; goto finish; }

    Port* p = &g_shared->port;
    if (p->render_mode == RENDER_FULL) {
        port_render(p);
        fflush(stdout);
    }
    while (!g_stop) {
        wait_semaphore(g_lock);
        p->tick++;
        sem_post(g_lock);
        if (run_workers(PHASE_WAREHOUSES) != 0 || run_workers(PHASE_SHIPS) != 0) {
            if (!g_stop) result = 1;
            break;
        }

        wait_semaphore(g_lock);
        /* Главный процесс печатает снимок раз в 10 тактов, тикер больше не нужен. */
        if (p->render_mode == RENDER_FULL && p->tick % 10 == 0) {
            port_render(p);
            fflush(stdout);
        }
        if (p->max_ticks > 0 && p->tick >= p->max_ticks) {
            port_log(p, "[t=%d] max_ticks reached\n", p->tick);
            p->running = 0;
        }
        if (port_all_done(p)) {
            port_log(p, "[t=%d] all ships done\n", p->tick);
            p->running = 0;
        }
        int running = p->running;
        sem_post(g_lock);
        if (!running) break;
        usleep(100 * 1000);
    }

finish:
    stop_workers();
    if (g_stop)
        port_log(&g_shared->port, "\n[signal %d] stopping simulation at tick %d\n",
                 (int)g_stop, g_shared->port.tick);
    for (int i = 0; i < g_shared->port.ship_count; i++) {
        Ship* s = &g_shared->port.ships[i];
        if (s->state != SHIP_DONE)
            port_log(&g_shared->port, "ship %d stopped: state=%s, cargo=%d/%d\n",
                     s->id, ship_state_str(s->state), s->cargo_volume, s->cargo_initial);
    }
    port_log(&g_shared->port, "Simulation finished at tick %d\n", g_shared->port.tick);
    log_close();
    ipc_close();
    return result;
}
