#ifndef HW1_BERTH_H
#define HW1_BERTH_H

#include "types.h"

typedef struct {
    int id;
    CargoType cargo;
    int ship_id;
} Berth;

void berth_init(Berth* b, int id, CargoType cargo);
int berth_is_free(const Berth* b);
void berth_render(const Berth* b);

#endif //HW1_BERTH_H
