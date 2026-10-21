#include "../include/port.h"
#include "../include/log.h"
#include "../include/dispatcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Обёртка над log_write: единая точка для событий из диспетчера и шага порта. */
void port_log(Port* p, const char* fmt, ...) {
    (void)p;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write("%s", buf);
}

/* Все суда в состоянии DONE? */
int port_all_done(const Port* p) {
    for (int i = 0; i < p->ship_count; i++)
        if (p->ships[i].state != SHIP_DONE) return 0;
    return 1;
}

/* Строка "DRY"/"LIQUID"/"CONTAINER" -> enum. */
static int parse_cargo_type(const char* s, CargoType* out) {
    if (!strcmp(s, "DRY")) { *out = CARGO_DRY; return 0; }
    if (!strcmp(s, "LIQUID")) { *out = CARGO_LIQUID; return 0; }
    if (!strcmp(s, "CONTAINER")) { *out = CARGO_CONTAINER; return 0; }
    return -1;
}

/* Разбор токена KEY=VALUE на key и val. */
static int parse_kv(const char* tok, char* key, size_t key_sz, char* val, size_t val_sz) {
    const char* eq = strchr(tok, '=');
    if (!eq) return -1;
    size_t klen = (size_t)(eq - tok);
    if (klen == 0 || klen >= key_sz) return -1;
    memcpy(key, tok, klen);
    key[klen] = '\0';
    strncpy(val, eq + 1, val_sz - 1);
    val[val_sz - 1] = '\0';
    return 0;
}

/*
 * Загрузка порта из .dat-файла. Формат — построчный, ключевое слово + параметры.
 * Пустые строки и строки, начинающиеся с '#', игнорируются.
 */
int port_load(Port* p, const char* path) {
    memset(p, 0, sizeof(*p));
    p->running = 1;
    p->enter_ticks = 10;   /* дефолт, если в файле нет TIMING */
    p->leave_ticks = 10;

    FILE* f = fopen(path, "r");
    if (!f) {
        perror("fopen");
        return -1;
    }

    /* Счётчики реально добавленных элементов по каждому типу ресурса. */
    int bi = 0, ti = 0, ci = 0, wi = 0, si = 0;
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '#' || *s == '\n' || *s == '\0')
            continue;

        char kw[32];
        if (sscanf(s, "%31s", kw) != 1)
            continue;

        /* TIMING enter=N leave=M */
        if (!strcmp(kw, "TIMING")) {
            char* tok = s + strlen(kw);
            char key[32], val[32];
            while (sscanf(tok, "%31s", key) == 1) {
                tok = strchr(tok, ' ');
                if (!tok) break;
                tok++;
                char kv[64];
                if (sscanf(tok, "%63s", kv) != 1) break;
                if (parse_kv(kv, key, sizeof(key), val, sizeof(val)) == 0) {
                    if (!strcmp(key, "enter")) p->enter_ticks = atoi(val);
                    else if (!strcmp(key, "leave")) p->leave_ticks = atoi(val);
                }
                tok = strchr(tok, ' ');
                if (!tok) break;
                tok++;
            }
        }
        /* Количество причалов (заголовок секции). */
        else if (!strcmp(kw, "BERTHS")) {
            sscanf(s, "%*s %d", &p->berth_count);
        }
        /* Один причал: BERTH <id> <CARGO>. */
        else if (!strcmp(kw, "BERTH")) {
            int id;
            char t[32];
            if (sscanf(s, "%*s %d %31s", &id, t) == 2) {
                CargoType ct;
                if (parse_cargo_type(t, &ct) == 0 && bi < MAX_BERTHS)
                    berth_init(&p->berths[bi++], id, ct);
            }
        }
        /* Количество буксиров. */
        else if (!strcmp(kw, "TUGS")) {
            sscanf(s, "%*s %d", &p->tug_count);
        }
        /* Один буксир: TUG <id>. */
        else if (!strcmp(kw, "TUG")) {
            int id;
            if (sscanf(s, "%*s %d", &id) == 1 && ti < MAX_TUGS)
                tug_init(&p->tugs[ti++], id);
        }
        /* Количество кранов. */
        else if (!strcmp(kw, "CRANES")) {
            sscanf(s, "%*s %d", &p->crane_count);
        }
        /* Один кран: CRANE <id> <CARGO> RATE=<n>. RATE обязателен и > 0. */
        else if (!strcmp(kw, "CRANE")) {
            int id;
            char t[32];
            int rate = -1;
            if (sscanf(s, "%*s %d %31s", &id, t) == 2) {
                char* r = strstr(s, "RATE=");
                if (r) rate = atoi(r + 5);
                CargoType ct;
                if (rate > 0 && parse_cargo_type(t, &ct) == 0 && ci < MAX_CRANES)
                    crane_init(&p->cranes[ci++], id, ct, rate);
                else {
                    fprintf(stderr, "Bad CRANE line: %s", s);
                    fclose(f);
                    return -1;
                }
            }
        }
        /* Количество складов. */
        else if (!strcmp(kw, "WAREHOUSES")) {
            sscanf(s, "%*s %d", &p->warehouse_count);
        }
        /*
         * Один склад:
         * WAREHOUSE <id> CAP=<n> TYPE=<CARGO> DRAIN_EVERY=<n> DRAIN_AMOUNT=<n>
         * DRAIN_EVERY и DRAIN_AMOUNT обязательны и > 0.
         */
        else if (!strcmp(kw, "WAREHOUSE")) {
            int id, cap;
            int drain_every = -1, drain_amount = -1;
            char t[32];
            if (sscanf(s, "%*s %d CAP=%d TYPE=%31s", &id, &cap, t) == 3) {
                char* de = strstr(s, "DRAIN_EVERY=");
                char* da = strstr(s, "DRAIN_AMOUNT=");
                if (de) drain_every = atoi(de + 12);
                if (da) drain_amount = atoi(da + 13);
                CargoType ct;
                if (drain_every > 0 && drain_amount > 0 &&
                    parse_cargo_type(t, &ct) == 0 && wi < MAX_WAREHOUSES) {
                    warehouse_init(&p->warehouses[wi++], id, ct, cap,
                                   drain_every, drain_amount);
                } else {
                    fprintf(stderr, "Bad WAREHOUSE line: %s", s);
                    fclose(f);
                    return -1;
                }
            }
        }
        /* Количество судов. */
        else if (!strcmp(kw, "SHIPS")) {
            sscanf(s, "%*s %d", &p->ship_count);
        }
        /* Одно судно: SHIP <id> <name> <CARGO> <volume> <arrive_tick>. */
        else if (!strcmp(kw, "SHIP")) {
            int id, vol, arr;
            char name[32], ct[32];
            if (sscanf(s, "%*s %d %31s %31s %d %d", &id, name, ct, &vol, &arr) == 5) {
                CargoType cargo;
                if (parse_cargo_type(ct, &cargo) == 0 && si < MAX_SHIPS)
                    ship_init(&p->ships[si++], id, name, cargo, vol, arr);
            }
        }
        /* Режим отображения: RENDER full | changes. */
        else if (!strcmp(kw, "RENDER")) {
            char mode[16];
            if (sscanf(s, "%*s %15s", mode) == 1) {
                if (!strcmp(mode, "full")) p->render_mode = RENDER_FULL;
                else if (!strcmp(mode, "changes")) p->render_mode = RENDER_CHANGES;
            }
        }
        /* Верхняя граница симуляции по тактам. */
        else if (!strcmp(kw, "MAX_TICKS")) {
            sscanf(s, "%*s %d", &p->max_ticks);
        }
    }
    fclose(f);
    return 0;
}

/* Печать полного снимка порта (используется тикером в режиме full). */
void port_render(const Port* p) {
    printf("=== Port state (tick %d) ===\n", p->tick);

    printf("Berths (%d):\n", p->berth_count);
    for (int i = 0; i < p->berth_count; i++) berth_render(&p->berths[i]);

    printf("Tugs (%d):\n", p->tug_count);
    for (int i = 0; i < p->tug_count; i++) tug_render(&p->tugs[i]);

    printf("Cranes (%d):\n", p->crane_count);
    for (int i = 0; i < p->crane_count; i++) crane_render(&p->cranes[i]);

    printf("Warehouses (%d):\n", p->warehouse_count);
    for (int i = 0; i < p->warehouse_count; i++) warehouse_render(&p->warehouses[i]);

    printf("Ships (%d):\n", p->ship_count);
    for (int i = 0; i < p->ship_count; i++) ship_render(&p->ships[i]);

    printf("============================\n");
}

/* Один такт симуляции: сначала сливаем склады, потом диспетчер двигает суда. */
void port_step(Port* p) {
    for (int i = 0; i < p->warehouse_count; i++) {
        if (warehouse_step(&p->warehouses[i])) {
            Warehouse* w = &p->warehouses[i];
            port_log(p, "[t=%d] warehouse %d drained, now %d/%d\n",
                     p->tick, w->id, w->used, w->capacity);
        }
    }
    dispatcher_step(p);
}
