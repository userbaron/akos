#include "../include/ipc.h"
#include "../include/log.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: %s SESSION INDEX\n", argv[0]);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "Usage: %s SESSION INDEX\n", argv[0]);
        return 1;
    }
    char* end;
    errno = 0;
    long session = strtol(argv[1], &end, 10);
    if (errno || end == argv[1] || *end || session < 1 || session > INT_MAX) {
        fprintf(stderr, "Invalid session\n");
        return 1;
    }
    errno = 0;
    long index = strtol(argv[2], &end, 10);
    if (errno || end == argv[2] || *end || index < 0 || index >= MAX_WORKERS) {
        fprintf(stderr, "Invalid worker index\n");
        return 1;
    }
    if (ipc_connect(session) != 0) return 1;
    if (index >= g_shared->process_count) {
        fprintf(stderr, "Worker index must be less than %d\n", g_shared->process_count);
        ipc_close();
        return 1;
    }
    if (log_attach(g_shared->log_path) != 0) { ipc_close(); return 1; }
    int result = worker_process((int)index);
    log_close();
    ipc_close();
    return result;
}
