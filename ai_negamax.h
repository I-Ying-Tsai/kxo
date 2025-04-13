#ifndef AI_NEGAMAX_H
#define AI_NEGAMAX_H

typedef struct {
    int score;
    int move;
} move_t;

move_t negamax_predict(char *table, char player);
void negamax_init(void);

#endif