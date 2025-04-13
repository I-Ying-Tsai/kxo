#include "user_zobrist.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "user_hlist.h"

#define HASH(key) ((key) % HASH_TABLE_SIZE)

uint64_t zobrist_table[UTIL_N_GRIDS][2];
static struct hlist_head hash_table[HASH_TABLE_SIZE];

// wyhash-like stateless PRNG
static inline uint64_t wyhash64_stateless(uint64_t *seed)
{
    *seed += 0x60bee2bee120fc15ULL;
    __uint128_t tmp = (__uint128_t) (*seed) * 0xa3b195354a39b70dULL;
    uint64_t m1 = (uint64_t) (tmp >> 64) ^ (uint64_t) tmp;
    tmp = (__uint128_t) m1 * 0x1b03738712fad5c9ULL;
    uint64_t m2 = (uint64_t) (tmp >> 64) ^ (uint64_t) tmp;
    return m2;
}

static uint64_t wyhash64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seed = (uint64_t) ts.tv_nsec ^ (uint64_t) ts.tv_sec;
    return wyhash64_stateless(&seed);
}

void zobrist_init(void)
{
    for (int i = 0; i < UTIL_N_GRIDS; i++) {
        zobrist_table[i][0] = wyhash64();
        zobrist_table[i][1] = wyhash64();
    }

    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        INIT_HLIST_HEAD(&hash_table[i]);
    }
}

zobrist_entry_t *zobrist_get(uint64_t key)
{
    uint64_t hash_key = HASH(key);
    if (hlist_empty(&hash_table[hash_key]))
        return NULL;

    zobrist_entry_t *entry;
    hlist_for_each_entry(entry, &hash_table[hash_key], ht_list) {
        if (entry->key == key)
            return entry;
    }
    return NULL;
}

void zobrist_put(uint64_t key, int score, int move)
{
    uint64_t hash_key = HASH(key);
    zobrist_entry_t *new_entry = malloc(sizeof(zobrist_entry_t));
    if (!new_entry) {
        fprintf(stderr, "zobrist_put: malloc failed\n");
        return;
    }
    new_entry->key = key;
    new_entry->score = score;
    new_entry->move = move;
    hlist_add_head(&new_entry->ht_list, &hash_table[hash_key]);
}

void zobrist_clear(void)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        while (!hlist_empty(&hash_table[i])) {
            zobrist_entry_t *entry =
                hlist_entry(hash_table[i].first, zobrist_entry_t, ht_list);
            hlist_del(&entry->ht_list);
            free(entry);
        }
        INIT_HLIST_HEAD(&hash_table[i]);
    }
}
