#ifndef HW1_SHIP_H
#define HW1_SHIP_H

#include "types.h"

typedef enum {
    SHIP_ARRIVING,     /* ещё не прибыло, ждёт своего такта */
    SHIP_WAIT_BERTH,   /* прибыло, ждёт свободный причал */
    SHIP_WAIT_TUG_IN,  /* причал занят, ждёт буксир на вход */
    SHIP_ENTERING,     /* буксируется к причалу */
    SHIP_WAIT_CRANE,   /* стоит у причала, ждёт кран и место на складе */
    SHIP_UNLOADING,    /* идёт разгрузка */
    SHIP_WAIT_TUG_OUT, /* разгрузка закончена, ждёт буксир на выход */
    SHIP_LEAVING,      /* выходит из порта */
    SHIP_DONE          /* покинуло порт */
} ShipState;

typedef struct {
    int id;
    char name[32];
    CargoType cargo;
    int cargo_volume;   /* сколько ещё осталось разгрузить */
    int cargo_initial;  /* исходный объём, для статистики */
    int arrive_tick;
    ShipState state;

    int berth_id;       /* -1 если не занимает */
    int tug_id;         /* -1 если не занимает */
    int crane_id;       /* -1 если не занимает */
    int warehouse_id;   /* -1 если не занимает */

    int stage_ticks;    /* счётчик для ENTERING / LEAVING */
} Ship;

void ship_init(Ship* s, int id, const char* name, CargoType cargo, int volume, int arrive_tick);
void ship_render(const Ship* s);
const char* ship_state_str(ShipState st);

#endif //HW1_SHIP_H
