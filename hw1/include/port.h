#ifndef HW1_PORT_H
#define HW1_PORT_H

#include "berth.h"
#include "tug.h"
#include "crane.h"
#include "warehouse.h"
#include "ship.h"

#define MAX_BERTHS 16
#define MAX_TUGS 16
#define MAX_CRANES 16
#define MAX_WAREHOUSES 16
#define MAX_SHIPS 64

typedef enum {
    RENDER_FULL,
    RENDER_CHANGES
} RenderMode;

typedef struct Port {
    Berth berths[MAX_BERTHS];
    int berth_count;

    Tug tugs[MAX_TUGS];
    int tug_count;

    Crane cranes[MAX_CRANES];
    int crane_count;

    Warehouse warehouses[MAX_WAREHOUSES];
    int warehouse_count;

    Ship ships[MAX_SHIPS];
    int ship_count;

    int enter_ticks;
    int leave_ticks;
    int max_ticks;
    RenderMode render_mode;

    int tick;
    int running;
} Port;

int port_load(Port* p, const char* path);
void port_render(const Port* p);
void port_step(Port* p);
void port_log(Port* p, const char* fmt, ...);
int port_all_done(const Port* p);

#endif //HW1_PORT_H
