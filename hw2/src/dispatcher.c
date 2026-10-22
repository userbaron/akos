#include "../include/port.h"
#include "../include/dispatcher.h"

#include <stdio.h>

/* ---------- поиск свободных ресурсов ---------- */

/* Первый свободный причал, принимающий нужный тип груза. */
static Berth* find_free_berth(Port* p, CargoType cargo) {
    for (int i = 0; i < p->berth_count; i++) {
        Berth* b = &p->berths[i];
        if (berth_is_free(b) && b->cargo == cargo)
            return b;
    }
    return NULL;
}

/* Первый свободный буксир. */
static Tug* find_free_tug(Port* p) {
    for (int i = 0; i < p->tug_count; i++) {
        if (tug_is_free(&p->tugs[i]))
            return &p->tugs[i];
    }
    return NULL;
}

/* Первый свободный кран, подходящий под тип груза. */
static Crane* find_free_crane(Port* p, CargoType cargo) {
    for (int i = 0; i < p->crane_count; i++) {
        Crane* c = &p->cranes[i];
        if (crane_is_free(c) && c->type == cargo)
            return c;
    }
    return NULL;
}

/* Есть ли в порту вообще причал для такого груза (не "свободен", а "существует"). */
static int port_has_berth_for(Port* p, CargoType cargo) {
    for (int i = 0; i < p->berth_count; i++)
        if (p->berths[i].cargo == cargo) return 1;
    return 0;
}

/* Есть ли в порту вообще кран для такого груза. */
static int port_has_crane_for(Port* p, CargoType cargo) {
    for (int i = 0; i < p->crane_count; i++)
        if (p->cranes[i].type == cargo) return 1;
    return 0;
}

/* Есть ли в порту вообще склад для такого груза. */
static int port_has_warehouse_for(Port* p, CargoType cargo) {
    for (int i = 0; i < p->warehouse_count; i++)
        if (p->warehouses[i].type == cargo) return 1;
    return 0;
}

/* Склад подходящего типа, у которого достаточно свободного места под весь объём need. */
static Warehouse* find_warehouse_with_space(Port* p, CargoType cargo, int need) {
    for (int i = 0; i < p->warehouse_count; i++) {
        Warehouse* w = &p->warehouses[i];
        if (w->type == cargo && warehouse_free_space(w) >= need)
            return w;
    }
    return NULL;
}

/* ---------- захват / освобождение ---------- */

/* Занять причал под судно: взаимно проставляем id друг другу. */
static void take_berth(Berth* b, Ship* s) {
    b->ship_id = s->id;
    s->berth_id = b->id;
}

/* Освободить причал. */
static void release_berth(Berth* b, Ship* s) {
    b->ship_id = -1;
    s->berth_id = -1;
}

/* Занять буксир под судно. */
static void take_tug(Tug* t, Ship* s) {
    t->ship_id = s->id;
    s->tug_id = t->id;
}

/* Освободить буксир. */
static void release_tug(Tug* t, Ship* s) {
    t->ship_id = -1;
    s->tug_id = -1;
}

/* Занять кран под судно. */
static void take_crane(Crane* c, Ship* s) {
    c->ship_id = s->id;
    s->crane_id = c->id;
}

/* Освободить кран. */
static void release_crane(Crane* c, Ship* s) {
    c->ship_id = -1;
    s->crane_id = -1;
}

/* ---------- вспомогательные ---------- */

/* Поиск причала по id — нужен при освобождении, когда у судна есть только id. */
static Berth* berth_by_id(Port* p, int id) {
    for (int i = 0; i < p->berth_count; i++)
        if (p->berths[i].id == id) return &p->berths[i];
    return NULL;
}

/* Поиск буксира по id. */
static Tug* tug_by_id(Port* p, int id) {
    for (int i = 0; i < p->tug_count; i++)
        if (p->tugs[i].id == id) return &p->tugs[i];
    return NULL;
}

/* Поиск крана по id. */
static Crane* crane_by_id(Port* p, int id) {
    for (int i = 0; i < p->crane_count; i++)
        if (p->cranes[i].id == id) return &p->cranes[i];
    return NULL;
}

/* ---------- основной шаг диспетчера ---------- */

/*
 * Двигаем одно судно на одну стадию вперёд, если есть нужные ресурсы.
 * Рабочий поток вызывает этот шаг под общим мьютексом порта:
 * поиск ресурса и его назначение нельзя разделять между потоками.
 */
void dispatcher_ship_step(Port* p, int i) {
    Ship* s = &p->ships[i];

    switch (s->state) {
        /* Судно "в пути": ждём наступления такта прибытия. */
        case SHIP_ARRIVING:
            if (p->tick >= s->arrive_tick) {
                port_log(p,"[t=%d] ship %d \"%s\" arrived\n", p->tick, s->id, s->name);

                /* Если в порту нет ресурсов под груз судна — сразу отправляем его мимо. */
                if (!port_has_berth_for(p, s->cargo) ||
                    !port_has_crane_for(p, s->cargo) ||
                    !port_has_warehouse_for(p, s->cargo)) {
                    port_log(p,"[t=%d] ship %d cannot be served (no suitable berth/crane/warehouse), leaving\n",
                           p->tick, s->id);
                    s->state = SHIP_DONE;
                    break;
                }

                s->state = SHIP_WAIT_BERTH;
            }
            break;

        /* Ищем свободный причал под тип груза. */
        case SHIP_WAIT_BERTH: {
            Berth* b = find_free_berth(p, s->cargo);
            if (b) {
                take_berth(b, s);
                port_log(p,"[t=%d] ship %d took berth %d, waiting for tug\n",
                       p->tick, s->id, b->id);
                s->state = SHIP_WAIT_TUG_IN;
            }
            break;
        }

        /* Причал уже занят, ждём буксир. */
        case SHIP_WAIT_TUG_IN: {
            Tug* t = find_free_tug(p);
            if (t) {
                take_tug(t, s);
                s->stage_ticks = p->enter_ticks;
                port_log(p,"[t=%d] ship %d got tug %d, entering\n",
                       p->tick, s->id, t->id);
                s->state = SHIP_ENTERING;
            }
            break;
        }

        /* Буксировка к причалу: отсчитываем такты, затем отпускаем буксир. */
        case SHIP_ENTERING:
            if (--s->stage_ticks <= 0) {
                Tug* t = tug_by_id(p, s->tug_id);
                if (t) release_tug(t, s);
                port_log(p,"[t=%d] ship %d moored at berth %d\n",
                       p->tick, s->id, s->berth_id);
                s->state = SHIP_WAIT_CRANE;
            }
            break;

        /*
         * Ждём одновременно свободный кран И достаточное место на складе.
         * Если места нет — кран не занимаем (иначе получим deadlock).
         */
        case SHIP_WAIT_CRANE: {
            Warehouse* w = find_warehouse_with_space(p, s->cargo, s->cargo_volume);
            if (!w) break; /* нет склада — ждём, кран не трогаем */
            Crane* c = find_free_crane(p, s->cargo);
            if (!c) break;
            take_crane(c, s);
            s->warehouse_id = w->id;
            port_log(p,"[t=%d] ship %d got crane %d and warehouse %d, unloading\n",
                   p->tick, s->id, c->id, w->id);
            s->state = SHIP_UNLOADING;
            break;
        }

        /* Разгрузка: за такт переносим c->rate единиц с судна на склад. */
        case SHIP_UNLOADING: {
            Crane* c = crane_by_id(p, s->crane_id);
            if (!c) break;
            int take = c->rate;
            if (take > s->cargo_volume) take = s->cargo_volume;
            Warehouse* w = NULL;
            for (int k = 0; k < p->warehouse_count; k++)
                if (p->warehouses[k].id == s->warehouse_id) w = &p->warehouses[k];
            if (!w) break;

            /* Несколько кранов могут работать на один склад: лишний груз остаётся на судне. */
            int free_space = warehouse_free_space(w);
            if (take > free_space) take = free_space;
            s->cargo_volume -= take;
            w->used += take;

            /* Груз закончился — отпускаем кран и идём ждать буксир на выход. */
            if (s->cargo_volume <= 0) {
                release_crane(c, s);
                s->warehouse_id = -1;
                port_log(p,"[t=%d] ship %d unloaded, waiting for tug out\n",
                       p->tick, s->id);
                s->state = SHIP_WAIT_TUG_OUT;
            }
            break;
        }

        /* Ждём буксир, чтобы выйти из порта. */
        case SHIP_WAIT_TUG_OUT: {
            Tug* t = find_free_tug(p);
            if (t) {
                take_tug(t, s);
                s->stage_ticks = p->leave_ticks;
                port_log(p,"[t=%d] ship %d got tug %d, leaving\n",
                       p->tick, s->id, t->id);
                s->state = SHIP_LEAVING;
            }
            break;
        }

        /* Буксировка из порта: по завершении отпускаем буксир и причал. */
        case SHIP_LEAVING:
            if (--s->stage_ticks <= 0) {
                Tug* t = tug_by_id(p, s->tug_id);
                Berth* b = berth_by_id(p, s->berth_id);
                if (t) release_tug(t, s);
                if (b) release_berth(b, s);
                port_log(p,"[t=%d] ship %d departed\n", p->tick, s->id);
                s->state = SHIP_DONE;
            }
            break;

        /* Финальное состояние — ничего не делаем. */
        case SHIP_DONE:
            break;
    }
}

/* Последовательный обход оставлен для обычного шага порта. */
void dispatcher_step(Port* p) {
    for (int i = 0; i < p->ship_count; i++)
        dispatcher_ship_step(p, i);
}
