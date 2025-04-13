#ifndef USER_XOROSHIRO_H
#define USER_XOROSHIRO_H

#include <stdint.h>

typedef struct state_array {
    uint64_t array[2];
} state_array;

uint64_t xoro_next(state_array *obj);
void xoro_jump(state_array *obj);
void xoro_init(state_array *obj);

#endif
