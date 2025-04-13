#include <stdlib.h>
#include <string.h>

#include "ai_mcts.h"
#include "game_util.h"
#include "user_xoroshiro.h"

#define ITERATIONS 1000
#define FIXED_LN2 \
    ((util_fixed_point_t) (0.69314718 * (1 << UTIL_FIXED_SCALE_BITS)))

typedef struct node {
    int move;
    char player;
    int n_visits;
    util_fixed_point_t score;
    struct node *parent;
    struct node *children[UTIL_N_GRIDS];
} node_t;

static struct {
    struct state_array xoro_obj;
    int nr_active_nodes;
} mcts_obj;

static node_t *new_node(int move, char player, node_t *parent)
{
    node_t *n = calloc(1, sizeof(node_t));
    n->move = move;
    n->player = player;
    n->parent = parent;
    return n;
}

static void free_node(node_t *node)
{
    for (int i = 0; i < UTIL_N_GRIDS; i++) {
        if (node->children[i])
            free_node(node->children[i]);
    }
    free(node);
}

static util_fixed_point_t fixed_mul(util_fixed_point_t a, util_fixed_point_t b)
{
    return (util_fixed_point_t) (((int64_t) a * b) >> UTIL_FIXED_SCALE_BITS);
}

static util_fixed_point_t fixed_sqrt(util_fixed_point_t x)
{
    if (!x || x == (1U << UTIL_FIXED_SCALE_BITS))
        return x;

    util_fixed_point_t s = 0U;
    for (int i = (31 - __builtin_clz(x | 1)); i >= 0; i--) {
        util_fixed_point_t t = (1U << i);
        if ((((s + t) * (s + t)) >> UTIL_FIXED_SCALE_BITS) <= x)
            s += t;
    }
    return s;
}

static util_fixed_point_t fixed_log(util_fixed_point_t v)
{
    if (v == 0)
        return 0;

    util_fixed_point_t y = v;
    util_fixed_point_t L = 1 << (31 - __builtin_clz(y));
    util_fixed_point_t R = L << 1;
    util_fixed_point_t Llog = ((31 - __builtin_clz(y)) - UTIL_FIXED_SCALE_BITS)
                              << UTIL_FIXED_SCALE_BITS;
    util_fixed_point_t Rlog = Llog + (1 << UTIL_FIXED_SCALE_BITS);
    util_fixed_point_t log;

    for (int i = 0; i < 20; i++) {
        if (y == L)
            return fixed_mul(Llog, FIXED_LN2);
        else if (y == R)
            return fixed_mul(Rlog, FIXED_LN2);

        log = (Llog + Rlog) >> 1;
        int64_t tmp = (int64_t) L * R >> UTIL_FIXED_SCALE_BITS;
        util_fixed_point_t mid = fixed_sqrt((util_fixed_point_t) tmp);

        if (y >= mid) {
            L = mid;
            Llog = log;
        } else {
            R = mid;
            Rlog = log;
        }
    }

    return fixed_mul(log, FIXED_LN2);
}

#define EXPLORATION_FACTOR fixed_sqrt(1U << (UTIL_FIXED_SCALE_BITS + 1))

static inline util_fixed_point_t uct_score(int n_total,
                                           int n_visits,
                                           util_fixed_point_t score)
{
    if (n_visits == 0)
        return (util_fixed_point_t) (~0U);  // max value

    util_fixed_point_t result =
        (score << UTIL_FIXED_SCALE_BITS) / (n_visits << UTIL_FIXED_SCALE_BITS);
    util_fixed_point_t tmp =
        EXPLORATION_FACTOR *
        fixed_sqrt(fixed_log(n_total << UTIL_FIXED_SCALE_BITS) / n_visits);
    tmp >>= UTIL_FIXED_SCALE_BITS;
    return result + tmp;
}

static node_t *select_move(node_t *node)
{
    node_t *best_node = NULL;
    util_fixed_point_t best_score = 0U;
    for (int i = 0; i < UTIL_N_GRIDS; i++) {
        if (!node->children[i])
            continue;
        util_fixed_point_t score =
            uct_score(node->n_visits, node->children[i]->n_visits,
                      node->children[i]->score);
        if (score > best_score) {
            best_score = score;
            best_node = node->children[i];
        }
    }
    return best_node;
}

static util_fixed_point_t simulate(const char *table, char player)
{
    char current_player = player;
    char temp_table[UTIL_N_GRIDS];
    memcpy(temp_table, table, UTIL_N_GRIDS);
    xoro_jump(&mcts_obj.xoro_obj);
    while (1) {
        int *moves = available_moves(temp_table);
        if (moves[0] == -1) {
            free(moves);
            break;
        }
        int n_moves = 0;
        while (n_moves < UTIL_N_GRIDS && moves[n_moves] != -1)
            ++n_moves;
        int move = moves[xoro_next(&mcts_obj.xoro_obj) % n_moves];
        free(moves);
        temp_table[move] = current_player;
        char win = check_win(temp_table);
        if (win != ' ')
            return calculate_win_value(win, player);
        current_player ^= 'O' ^ 'X';
    }
    return 1U << (UTIL_FIXED_SCALE_BITS - 1);  // draw
}

static void backpropagate(node_t *node, util_fixed_point_t score)
{
    while (node) {
        node->n_visits++;
        node->score += score;
        node = node->parent;
        score = (1U << UTIL_FIXED_SCALE_BITS) - score;
    }
}

static int expand(node_t *node, const char *table)
{
    int *moves = available_moves(table);
    int n_moves = 0;
    while (n_moves < UTIL_N_GRIDS && moves[n_moves] != -1)
        ++n_moves;
    for (int i = 0; i < n_moves; i++) {
        node->children[i] = new_node(moves[i], node->player ^ 'O' ^ 'X', node);
    }
    free(moves);
    return n_moves;
}

int mcts(const char *table, char player)
{
    node_t *root = new_node(-1, player, NULL);
    mcts_obj.nr_active_nodes = 1;
    for (int i = 0; i < ITERATIONS; i++) {
        node_t *node = root;
        char temp_table[UTIL_N_GRIDS];
        memcpy(temp_table, table, UTIL_N_GRIDS);
        while (1) {
            char win = check_win(temp_table);
            if (win != ' ') {
                util_fixed_point_t score =
                    calculate_win_value(win, node->player ^ 'O' ^ 'X');
                backpropagate(node, score);
                break;
            }
            if (node->n_visits == 0) {
                util_fixed_point_t score = simulate(temp_table, node->player);
                backpropagate(node, score);
                break;
            }
            if (!node->children[0])
                mcts_obj.nr_active_nodes += expand(node, temp_table);
            node = select_move(node);
            if (!node)
                return -1;
            temp_table[node->move] = node->player ^ 'O' ^ 'X';
        }
    }
    node_t *best_node = NULL;
    int best_visits = -1;
    for (int i = 0; i < UTIL_N_GRIDS; i++) {
        if (root->children[i] && root->children[i]->n_visits > best_visits) {
            best_visits = root->children[i]->n_visits;
            best_node = root->children[i];
        }
    }
    int best_move = best_node ? best_node->move : -1;
    free_node(root);
    return best_move;
}

void mcts_init(void)
{
    xoro_init(&mcts_obj.xoro_obj);
    mcts_obj.nr_active_nodes = 0;
}