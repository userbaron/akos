#include "../include/berth.h"
#include <stdio.h>

/* Инициализация причала: id, тип груза, который он принимает, и признак "свободен" (-1). */
void berth_init(Berth* b, int id, CargoType cargo) {
    b->id = id;
    b->cargo = cargo;
    b->ship_id = -1;
}

/* Причал свободен, если за ним не закреплено судно. */
int berth_is_free(const Berth* b) { return b->ship_id == -1; }

/* Печать текущего состояния причала. */
void berth_render(const Berth* b) { // Вывод был написан с помощью ИИ
    printf("  Berth %d [%s]: ", b->id, cargo_str(b->cargo));
    if (berth_is_free(b))
        printf("free\n");
    else
        printf("occupied by ship %d\n", b->ship_id);
}
