#include "../include/types.h"

/* Строка по enum-значению типа груза. */
const char* cargo_str(CargoType t) {
    switch (t) {
        case CARGO_DRY: return "DRY";
        case CARGO_LIQUID: return "LIQUID";
        case CARGO_CONTAINER: return "CONTAINER";
    }
    return "?";
}
