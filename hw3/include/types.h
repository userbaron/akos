#ifndef HW1_TYPES_H
#define HW1_TYPES_H

typedef enum {
    CARGO_DRY,
    CARGO_LIQUID,
    CARGO_CONTAINER
} CargoType;

const char* cargo_str(CargoType t);

#endif //HW1_TYPES_H
