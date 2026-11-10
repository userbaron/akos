#include "../include/ipc.h"
#include "../include/dispatcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

SharedPort* g_shared = NULL;
sem_t* g_lock = NULL;
sem_t* g_done = NULL;
sem_t* g_start[MAX_WORKERS];
volatile sig_atomic_t g_stop = 0;

static int g_owner = 0;
static int g_named = 1;
static long g_session;
static sem_t* g_wakeup = NULL;

/* sem_post разрешён в обработчике: будим собственное ожидание для выхода. */
static void on_signal(int sig) {
    int saved_errno = errno;
    if (!g_stop) g_stop = sig;
    if (g_wakeup) sem_post(g_wakeup);
    errno = saved_errno;
}

void setup_signals(sem_t* wakeup) {
    g_wakeup = wakeup;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
}

/* Ожидание замка доводим до конца, в том числе при остановке. */
int wait_semaphore(sem_t* sem) {
    while (sem_wait(sem) != 0) {
        if (errno != EINTR) { perror("sem_wait"); return -1; }
    }
    return 0;
}

/* При ожидании фазы сигнал означает отмену; обработчик разбудит sem_wait. */
int wait_event(sem_t* sem) {
    if (g_stop || wait_semaphore(sem) != 0) return -1;
    return g_stop ? -1 : 0;
}

static sem_t* open_semaphore(const char* suffix, int create, unsigned int value) {
    char name[64];
    snprintf(name, sizeof(name), "/akos_%ld_%s", g_session, suffix);
    sem_t* sem = sem_open(name, create ? O_CREAT | O_EXCL : 0, 0600, value);
    if (sem == SEM_FAILED) { perror("sem_open"); return NULL; }
    return sem;
}

/* pshared=1: неименованные sem_t лежат в разделяемой памяти. */
static int init_semaphores(int count, int create) {
    if (g_named) {
        g_lock = open_semaphore("lock", create, 1);
        if (!g_lock) return -1;
        g_done = open_semaphore("done", create, 0);
        if (!g_done) return -1;
        for (int i = 0; i < count; i++) {
            char suffix[16];
            snprintf(suffix, sizeof(suffix), "s%d", i);
            g_start[i] = open_semaphore(suffix, create, 0);
            if (!g_start[i]) return -1;
        }
    } else {
        if (sem_init(&g_shared->lock, 1, 1) != 0) goto fail;
        g_lock = &g_shared->lock;
        if (sem_init(&g_shared->done, 1, 0) != 0) goto fail;
        g_done = &g_shared->done;
        for (int i = 0; i < count; i++) {
            if (sem_init(&g_shared->start[i], 1, 0) != 0) goto fail;
            g_start[i] = &g_shared->start[i];
        }
    }
    return 0;
fail:
    perror("sem_init (process-shared semaphores require Linux)");
    return -1;
}

int ipc_create(const Port* port, int count, int named) {
    g_session = (long)getpid();
    g_named = named;
    char name[64];
    snprintf(name, sizeof(name), "/akos_%ld_mem", g_session);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return -1; }
    g_owner = 1;
    if (ftruncate(fd, sizeof(SharedPort)) != 0) {
        perror("ftruncate");
        close(fd);
        ipc_close();
        return -1;
    }
    void* memory = mmap(NULL, sizeof(SharedPort), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) { perror("mmap"); ipc_close(); return -1; }
    g_shared = memory;
    memset(g_shared, 0, sizeof(*g_shared));
    g_shared->port = *port;
    g_shared->process_count = count;
    g_shared->owner_pid = getpid();
    if (init_semaphores(count, 1) != 0) { ipc_close(); return -1; }
    return 0;
}

/* Независимая программа открывает уже созданные память и семафоры. */
int ipc_connect(long session) {
    g_session = session;
    char name[64];
    snprintf(name, sizeof(name), "/akos_%ld_mem", g_session);
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return -1; }
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < (off_t)sizeof(SharedPort)) {
        fprintf(stderr, "Shared memory is not ready\n");
        close(fd);
        return -1;
    }
    void* memory = mmap(NULL, sizeof(SharedPort), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) { perror("mmap"); return -1; }
    g_shared = memory;
    if (g_shared->owner_pid != (pid_t)session ||
        g_shared->process_count < 1 || g_shared->process_count > MAX_WORKERS ||
        init_semaphores(g_shared->process_count, 0) != 0) {
        ipc_close();
        return -1;
    }
    return 0;
}

static void close_semaphore(sem_t* sem, const char* suffix) {
    if (!sem) return;
    if (g_named) {
        sem_close(sem);
        if (g_owner) {
            char name[64];
            snprintf(name, sizeof(name), "/akos_%ld_%s", g_session, suffix);
            sem_unlink(name);
        }
    } else if (g_owner) sem_destroy(sem);
}

/* Создатель удаляет объекты; остальные процессы закрывают свои ссылки. */
void ipc_close(void) {
    g_wakeup = NULL;
    close_semaphore(g_lock, "lock");
    close_semaphore(g_done, "done");
    for (int i = 0; i < MAX_WORKERS; i++) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "s%d", i);
        close_semaphore(g_start[i], suffix);
        g_start[i] = NULL;
    }
    g_lock = g_done = NULL;
    if (g_shared) munmap(g_shared, sizeof(SharedPort));
    g_shared = NULL;
    if (g_owner) {
        char name[64];
        snprintf(name, sizeof(name), "/akos_%ld_mem", g_session);
        shm_unlink(name);
    }
    g_owner = 0;
}

/* Шаги склада/судна прежние, общий замок теперь является семафором. */
int worker_process(int index) {
    g_owner = 0;
    setup_signals(g_start[index]);
    if (wait_semaphore(g_lock) != 0) return 1;
    if (!g_shared->port.running || g_shared->worker_pids[index] != 0) {
        sem_post(g_lock);
        fprintf(stderr, "Worker %d is already connected or simulation has stopped\n", index);
        return 1;
    }
    g_shared->worker_pids[index] = getpid();
    sem_post(g_lock);
    sem_post(g_done);  /* подтверждение подключения */

    int result = 0;
    while (!g_stop) {
        if (wait_event(g_start[index]) != 0) {
            if (!g_stop) result = 1;
            break;
        }
        if (wait_semaphore(g_lock) != 0) { result = 1; break; }
        int running = g_shared->port.running;
        int phase = g_shared->phase;
        sem_post(g_lock);
        if (!running) break;

        Port* p = &g_shared->port;
        int total = p->warehouse_count + p->ship_count;
        for (int i = index; i < total; i += g_shared->process_count) {
            int is_warehouse = i < p->warehouse_count;
            if ((phase == PHASE_WAREHOUSES) != is_warehouse) continue;
            if (wait_semaphore(g_lock) != 0) { result = 1; break; }
            if (is_warehouse) port_warehouse_step(p, i);
            else dispatcher_ship_step(p, i - p->warehouse_count);
            sem_post(g_lock);
        }
        if (result) break;
        sem_post(g_done);
    }

    /* Прерывание отдельного рабочего останавливает всё приложение. */
    if (g_stop || result) kill(g_shared->owner_pid, SIGTERM);
    if (wait_semaphore(g_lock) == 0) {
        g_shared->exited[index] = 1;
        sem_post(g_lock);
    }
    return result;
}
