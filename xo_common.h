#pragma once

#define XO_BOARD_SIZE 16
#define XO_GOAL 3

struct xo_board {
    char table[XO_BOARD_SIZE];
    char player;
} __attribute__((packed));

struct xo_result {
    int move;
};
