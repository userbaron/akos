#include "../include/log.h"
#include <unistd.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int g_fd = -1;  /* дескриптор лог-файла, -1 если не открыт */

/* Открываем лог на запись, с обрезкой старого содержимого. */
void log_open(const char* path) {
    g_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (g_fd < 0) perror("open log");
}

/* Закрываем лог-файл. */
void log_close(void) {
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

/* Форматируем строку и пишем её и в файл, и в stdout. */
void log_write(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    if (g_fd >= 0) {
        ssize_t w = write(g_fd, buf, n);
        (void)w;
    }
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}
