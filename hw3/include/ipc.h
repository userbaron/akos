#ifndef HW3_IPC_H
#define HW3_IPC_H

#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include "port.h"

#define MAX_WORKERS (MAX_SHIPS + MAX_WAREHOUSES)

enum { PHASE_WAREHOUSES, PHASE_SHIPS };

/* Только общие данные. Указатели на именованные семафоры у каждого свои. */
typedef struct {
    Port port;
    int process_count;
    int phase;
    pid_t owner_pid;
    pid_t worker_pids[MAX_WORKERS];
    int exited[MAX_WORKERS];
    char log_path[PATH_MAX];
    sem_t lock;
    sem_t done;
    sem_t start[MAX_WORKERS];
} SharedPort;

extern SharedPort* g_shared;
extern sem_t* g_lock;
extern sem_t* g_done;
extern sem_t* g_start[MAX_WORKERS];
extern volatile sig_atomic_t g_stop;

int ipc_create(const Port* port, int count, int named);
int ipc_connect(long session);
void ipc_close(void);
void setup_signals(sem_t* wakeup);
int wait_semaphore(sem_t* sem);
int wait_event(sem_t* sem);
int worker_process(int index);

#endif
