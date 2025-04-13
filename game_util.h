#ifndef GAME_UTIL_H
#define GAME_UTIL_H

#include <stdint.h>

#define UTIL_BOARD_SIZE 4
#define UTIL_GOAL 3
#define UTIL_N_GRIDS (UTIL_BOARD_SIZE * UTIL_BOARD_SIZE)
#define UTIL_FIXED_SCALE_BITS 16

typedef struct {
    char grid[UTIL_N_GRIDS];
} game_table_t;

typedef uint32_t util_fixed_point_t;

typedef struct {
    int i_shift;
    int j_shift;
    int i_lower_bound;
    int j_lower_bound;
    int i_upper_bound;
    int j_upper_bound;
} util_line_t;

char check_win(const char *t);
util_fixed_point_t calculate_win_value(char win, char player);
int *available_moves(const char *t);

#endif
