#ifndef HW1_DISPATCHER_H
#define HW1_DISPATCHER_H

struct Port;

void dispatcher_step(struct Port* p);
void dispatcher_ship_step(struct Port* p, int i);

#endif //HW1_DISPATCHER_H
