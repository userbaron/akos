#include "../include/tug.h"
#include <stdio.h>

/* Инициализация буксира: id + признак "свободен". */
void tug_init(Tug* t, int id) { t->id = id; t->ship_id = -1; }

/* Буксир свободен, если не закреплён за судном. */
int tug_is_free(const Tug* t) { return t->ship_id == -1; }

/* Печать состояния буксира. */
void tug_render(const Tug* t) { // Вывод был написан с помощью ИИ
    printf("  Tug %d: ", t->id);
    if (tug_is_free(t))
        printf("free\n");
    else
        printf("busy with ship %d\n", t->ship_id);
}
