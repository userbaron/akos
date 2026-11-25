#include "../include/warehouse.h"
#include <stdio.h>

/* Инициализация склада: тип груза, вместимость и параметры автослива. */
void warehouse_init(Warehouse* w, int id, CargoType type, int cap, int drain_every, int drain_amount) {
    w->id = id;
    w->type = type;
    w->capacity = cap;
    w->used = 0;
    w->drain_every = drain_every;
    w->drain_amount = drain_amount;
    w->drain_counter = drain_every;  /* первый слив — через drain_every тактов */
}

/* Сколько ещё влезет на склад. */
int warehouse_free_space(const Warehouse* w) {
    return w->capacity - w->used;
}

/*
 * Один такт склада. Раз в drain_every тактов отгружает drain_amount единиц.
 * Возвращает 1, если слив реально произошёл (нужно для логирования).
 */
int warehouse_step(Warehouse* w) {
    if (--w->drain_counter > 0) return 0;
    w->drain_counter = w->drain_every;
    if (w->used > 0) {
        w->used -= w->drain_amount;
        if (w->used < 0) w->used = 0;
        return 1;
    }
    return 0;
}

/* Печать состояния склада. */
void warehouse_render(const Warehouse* w) { // Вывод был написан с помощью ИИ
    printf("  Warehouse %d [%s]: %d/%d used\n",
           w->id, cargo_str(w->type), w->used, w->capacity);
}
