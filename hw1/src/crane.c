#include "../include/crane.h"
#include <stdio.h>

/* Инициализация крана: id, тип груза, скорость разгрузки (единиц за такт). */
void crane_init(Crane* c, int id, CargoType type, int rate) {
    c->id = id;
    c->type = type;
    c->ship_id = -1;
    c->rate = rate;
}

/* Кран свободен, если за ним не закреплено судно. */
int crane_is_free(const Crane* c) {
    return c->ship_id == -1;
}

/* Печать текущего состояния крана. */
void crane_render(const Crane* c) { // Вывод был написан с помощью ИИ
    printf("  Crane %d [%s, rate=%d]: ", c->id, cargo_str(c->type), c->rate);
    if (crane_is_free(c))
        printf("free\n");
    else
        printf("busy with ship %d\n", c->ship_id);
}
