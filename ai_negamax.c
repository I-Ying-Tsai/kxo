#include "ai_negamax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_util.h"
#include "user_zobrist.h"
#include "util.h"

#define MAX_SEARCH_DEPTH 6

static int history_score_sum[N_GRIDS];
static int history_count[N_GRIDS];

static uint64_t hash_value;

static int cmp_moves(const void *a, const void *b)
{
    int move_a = *(const int *) a;
    int move_b = *(const int *) b;
    int score_a = history_count[move_a]
                      ? history_score_sum[move_a] / history_count[move_a]
                      : 0;
    int score_b = history_count[move_b]
                      ? history_score_sum[move_b] / history_count[move_b]
                      : 0;
    return score_b - score_a;
}

static move_t negamax(char *table, int depth, char player, int alpha, int beta)
{
    if (check_win(table) != ' ' || depth == 0) {
        move_t result = {get_score(table, player), -1};
        return result;
    }

    const zobrist_entry_t *entry = zobrist_get(hash_value);
    if (entry)
        return (move_t){.score = entry->score, .move = entry->move};

    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < N_GRIDS && moves[n_moves] != -1)
        ++n_moves;

    qsort(moves, n_moves, sizeof(int), cmp_moves);

    move_t best_move = {-10000, -1};

    for (int i = 0; i < n_moves; i++) {
        int move = moves[i];
        table[move] = player;
        hash_value ^= zobrist_table[move][player == 'X'];

        int score;
        if (!i) {
            score = -negamax(table, depth - 1, player == 'X' ? 'O' : 'X', -beta,
                             -alpha)
                         .score;
        } else {
            score = -negamax(table, depth - 1, player == 'X' ? 'O' : 'X',
                             -alpha - 1, -alpha)
                         .score;
            if (alpha < score && score < beta) {
                score = -negamax(table, depth - 1, player == 'X' ? 'O' : 'X',
                                 -beta, -score)
                             .score;
            }
        }

        history_count[move]++;
        history_score_sum[move] += score;

        if (score > best_move.score) {
            best_move.score = score;
            best_move.move = move;
        }

        table[move] = ' ';
        hash_value ^= zobrist_table[move][player == 'X'];

        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }

    free(moves);
    zobrist_put(hash_value, best_move.score, best_move.move);
    return best_move;
}

void negamax_init(void)
{
    zobrist_init();
    hash_value = 0;
}

move_t negamax_predict(char *table, char player)
{
    for (int i = 0; i < N_GRIDS; ++i) {
        if (table[i] == 'X')
            hash_value ^= zobrist_table[i][1];
        else if (table[i] == 'O')
            hash_value ^= zobrist_table[i][0];
    }
    memset(history_score_sum, 0, sizeof(history_score_sum));
    memset(history_count, 0, sizeof(history_count));
    move_t result = {-1, -1};
    for (int depth = 2; depth <= MAX_SEARCH_DEPTH; depth += 2) {
        result = negamax(table, depth, player, -100000, 100000);
        zobrist_clear();
    }
    return result;
}
