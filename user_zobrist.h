#ifndef USER_ZOBRIST_H
#define USER_ZOBRIST_H

#include <stdint.h>
#include "game_util.h"
#include "user_hlist.h"

#define HASH_TABLE_SIZE 1024

struct state_array {
    uint64_t array[2];
};

typedef struct zobrist_entry {
    uint64_t key;
    int score;
    int move;
    struct hlist_node ht_list;
} zobrist_entry_t;

extern uint64_t zobrist_table[UTIL_N_GRIDS][2];

void zobrist_init(void);
void zobrist_clear(void);
zobrist_entry_t *zobrist_get(uint64_t key);
void zobrist_put(uint64_t key, int score, int move);

#endif
