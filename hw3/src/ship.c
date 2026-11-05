#include "../include/ship.h"
#include <stdio.h>
#include <string.h>

/* Инициализация судна: заполняем статические поля, все ресурсы -1. */
void ship_init(Ship* s, int id, const char* name, CargoType cargo, int volume, int arrive_tick) {
    s->id = id;
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';
    s->cargo = cargo;
    s->cargo_volume = volume;
    s->cargo_initial = volume;
    s->arrive_tick = arrive_tick;
    s->state = SHIP_ARRIVING;
    s->berth_id = -1;
    s->tug_id = -1;
    s->crane_id = -1;
    s->warehouse_id = -1;
    s->stage_ticks = 0;
}

/* Строка состояния судна — для отображения. */
const char* ship_state_str(ShipState st) {
    switch (st) {
        case SHIP_ARRIVING: return "ARRIVING";
        case SHIP_WAIT_BERTH: return "WAIT_BERTH";
        case SHIP_WAIT_TUG_IN: return "WAIT_TUG_IN";
        case SHIP_ENTERING: return "ENTERING";
        case SHIP_WAIT_CRANE: return "WAIT_CRANE";
        case SHIP_UNLOADING: return "UNLOADING";
        case SHIP_WAIT_TUG_OUT: return "WAIT_TUG_OUT";
        case SHIP_LEAVING: return "LEAVING";
        case SHIP_DONE: return "DONE";
    }
    return "?";
}

/* Печать состояния судна: груз, состояние и занятые ресурсы (если есть). */
void ship_render(const Ship* s) { // Вывод был написан с помощью ИИ
    printf("  Ship %d \"%s\" [%s, %d/%d, arrive=%d]: state=%s",
           s->id, s->name, cargo_str(s->cargo),
           s->cargo_volume, s->cargo_initial,
           s->arrive_tick, ship_state_str(s->state));
    if (s->berth_id >= 0) printf(", berth=%d", s->berth_id);
    if (s->tug_id >= 0) printf(", tug=%d", s->tug_id);
    if (s->crane_id >= 0) printf(", crane=%d", s->crane_id);
    if (s->warehouse_id >= 0) printf(", wh=%d", s->warehouse_id);
    printf("\n");
}
