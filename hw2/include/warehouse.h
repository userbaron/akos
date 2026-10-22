#ifndef HW1_WAREHOUSE_H
#define HW1_WAREHOUSE_H

#include "types.h"

typedef struct {
    int id;
    CargoType type;
    int capacity;
    int used;
    int drain_every;   // тактов между сливами (> 0)
    int drain_amount;  // сколько единиц сливается за раз (> 0)
    int drain_counter; // текущий отсчёт
} Warehouse;

void warehouse_init(Warehouse* w, int id, CargoType type, int cap,
                    int drain_every, int drain_amount);
int warehouse_is_full(const Warehouse* w);
int warehouse_free_space(const Warehouse* w);
int warehouse_step(Warehouse* w);
void warehouse_render(const Warehouse* w);

#endif
