#include "../include/log.h"
#include "../include/net.h"
#include <unistd.h>

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static int g_fd = -1;  /* дескриптор лог-файла, -1 если не открыт */
static int g_channel = -1;  /* канал событий от вычисляющего потомка к TCP-клиенту */

/* Открываем лог на запись, с обрезкой старого содержимого. */
int log_open(const char* path) {
    g_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_APPEND, 0644);
    if (g_fd < 0) { perror("open log"); return -1; }
    return 0;
}

void log_set_channel(int fd) {
    g_channel = fd;
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
        int offset = 0;
        while (offset < n) {
            ssize_t written = write(g_fd, buf + offset, n - offset);
            if (written > 0) offset += (int)written;
            else if (written < 0 && errno == EINTR) continue;
            else break;
        }
    }
    if (g_channel >= 0) {
        Message message;
        memset(&message, 0, sizeof(message));
        message.type = NET_EVENT;
        memcpy(message.text, buf, n);
        if (net_send_message(g_channel, &message) != 0) g_stop = 1;
    }
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}
