#ifndef HW1_CRANE_H
#define HW1_CRANE_H

#include "types.h"

typedef struct {
    int id;
    CargoType type;
    int ship_id;   // -1 если свободен
    int rate;      // единиц груза за такт
} Crane;

void crane_init(Crane* c, int id, CargoType type, int rate);
int crane_is_free(const Crane* c);
void crane_render(const Crane* c);

#endif
