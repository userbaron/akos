#ifndef HW1_TUG_H
#define HW1_TUG_H

typedef struct {
    int id;
    int ship_id;
} Tug;

void tug_init(Tug* t, int id);
int tug_is_free(const Tug* t);
void tug_render(const Tug* t);

#endif //HW1_TUG_H
