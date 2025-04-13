#include "game_util.h"
#include <stdlib.h>
#include <string.h>

#define GET_INDEX(i, j) ((i) * UTIL_BOARD_SIZE + (j))
#define LOOKUP(t, i, j, def)                                                  \
    (((i) < 0 || (i) >= UTIL_BOARD_SIZE || (j) < 0 || (j) >= UTIL_BOARD_SIZE) \
         ? def                                                                \
         : (t)[GET_INDEX(i, j)])

const util_line_t lines[4] = {
    {1, 0, 0, 0, UTIL_BOARD_SIZE - UTIL_GOAL + 1, UTIL_BOARD_SIZE},
    {0, 1, 0, 0, UTIL_BOARD_SIZE, UTIL_BOARD_SIZE - UTIL_GOAL + 1},
    {1, 1, 0, 0, UTIL_BOARD_SIZE - UTIL_GOAL + 1,
     UTIL_BOARD_SIZE - UTIL_GOAL + 1},
    {1, -1, 0, UTIL_GOAL - 1, UTIL_BOARD_SIZE - UTIL_GOAL + 1, UTIL_BOARD_SIZE},
};

static char check_line_segment_win(const char *t,
                                   int i,
                                   int j,
                                   util_line_t line)
{
    char last = t[GET_INDEX(i, j)];
    if (last == ' ')
        return ' ';
    for (int k = 1; k < UTIL_GOAL; k++) {
        if (last != t[GET_INDEX(i + k * line.i_shift, j + k * line.j_shift)])
            return ' ';
    }
    if (last == LOOKUP(t, i - line.i_shift, j - line.j_shift, ' ') ||
        last == LOOKUP(t, i + UTIL_GOAL * line.i_shift,
                       j + UTIL_GOAL * line.j_shift, ' '))
        return ' ';
    return last;
}

char check_win(const char *t)
{
    for (int i_line = 0; i_line < 4; ++i_line) {
        util_line_t line = lines[i_line];
        for (int i = line.i_lower_bound; i < line.i_upper_bound; ++i) {
            for (int j = line.j_lower_bound; j < line.j_upper_bound; ++j) {
                char win = check_line_segment_win(t, i, j, line);
                if (win != ' ')
                    return win;
            }
        }
    }
    for (int i = 0; i < UTIL_N_GRIDS; i++)
        if (t[i] == ' ')
            return ' ';
    return 'D';
}

util_fixed_point_t calculate_win_value(char win, char player)
{
    if (win == player)
        return 1U << UTIL_FIXED_SCALE_BITS;
    if (win == (player ^ 'O' ^ 'X'))
        return 0U;
    return 1U << (UTIL_FIXED_SCALE_BITS - 1);
}

int *available_moves(const char *table)
{
    int *moves = calloc(UTIL_N_GRIDS + 1, sizeof(int));
    int m = 0;
    for (int i = 0; i < UTIL_N_GRIDS; i++) {
        if (table[i] == ' ')
            moves[m++] = i;
    }
    moves[m] = -1;
    return moves;
}