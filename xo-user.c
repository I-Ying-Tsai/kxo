#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "ai_mcts.h"
#include "ai_negamax.h"
#include "game_util.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

/* coroutine status values */
enum {
    CR_BLOCKED = 0,
    CR_FINISHED = 1,
};

/* Helper macros to generate unique labels */
#define __cr_line3(name, line) _cr_##name##line
#define __cr_line2(name, line) __cr_line3(name, line)
#define __cr_line(name) __cr_line2(name, __LINE__)

struct cr {
    void *label;
    int status;
};

#define cr_context_name(name) __cr_context_##name
#define cr_context(name) struct cr cr_context_name(name)
#define cr_context_init()                   \
    {                                       \
        .label = NULL, .status = CR_BLOCKED \
    }

#define cr_func_name(name) __cr_func_##name
#define cr_proto(name, ...) \
    static void cr_func_name(name)(struct cr * ctx, ##__VA_ARGS__)

#define cr_run(name, ...) \
    cr_func_name(name)(&cr_context_name(name), ##__VA_ARGS__)

#define cr_local static

#define cr_begin()                        \
    do {                                  \
        if ((ctx)->status == CR_FINISHED) \
            return;                       \
        if ((ctx)->label)                 \
            goto *(ctx)->label;           \
    } while (0)
#define cr_yield cr_label(ctx, CR_BLOCKED)
#define cr_label(o, stat)                                   \
    do {                                                    \
        (o)->status = (stat);                               \
        __cr_line(label) : (o)->label = &&__cr_line(label); \
    } while (0)
#define cr_end() cr_label(ctx, CR_FINISHED)

#define cr_status(name) cr_context_name(name).status

#define cr_wait(cond)              \
    do {                           \
        cr_label(ctx, CR_BLOCKED); \
        if (!(cond))               \
            return;                \
    } while (0)

#define cr_exit(stat)        \
    do {                     \
        cr_label(ctx, stat); \
        return;              \
    } while (0)

#define cr_queue(T, size) \
    struct {              \
        T buf[size];      \
        size_t r, w;      \
    }
#define cr_queue_init() \
    {                   \
        .r = 0, .w = 0  \
    }
#define cr_queue_len(q) (sizeof((q)->buf) / sizeof((q)->buf[0]))
#define cr_queue_cap(q) ((q)->w - (q)->r)
#define cr_queue_empty(q) ((q)->w == (q)->r)
#define cr_queue_full(q) (cr_queue_cap(q) == cr_queue_len(q))

#define cr_queue_push(q, el) \
    (!cr_queue_full(q) && ((q)->buf[(q)->w++ % cr_queue_len(q)] = (el), 1))
#define cr_queue_pop(q) \
    (cr_queue_empty(q) ? NULL : &(q)->buf[(q)->r++ % cr_queue_len(q)])

/* Wrap system calls and other functions that return -1 and set errno */
#define cr_sys(call)                                                        \
    cr_wait((errno = 0) ||                                                  \
            !(((call) == -1) && (errno == EAGAIN || errno == EWOULDBLOCK || \
                                 errno == EINPROGRESS || errno == EINTR)))

typedef cr_queue(uint8_t, 4096) byte_queue_t;

game_table_t tables[2];
int turns[2] = {1, 1};
bool finished[2] = {false, false};
volatile bool should_redraw = false;
ssize_t written;

int mcts(const char *table, char player);
move_t negamax_predict(char *table, char player);

void draw_table(const char *t)
{
    for (int i = 0; i < UTIL_BOARD_SIZE; i++) {
        for (int j = 0; j < UTIL_BOARD_SIZE; j++) {
            putchar(t[i * UTIL_BOARD_SIZE + j]);
            if (j < UTIL_BOARD_SIZE - 1)
                putchar('|');
        }
        putchar('\n');
        if (i < UTIL_BOARD_SIZE - 1) {
            for (int j = 0; j < UTIL_BOARD_SIZE * 2 - 1; j++)
                putchar('-');
            putchar('\n');
        }
    }
}

cr_proto(stdin_loop, byte_queue_t *out)
{
    cr_local uint8_t b;
    cr_local int r;
    cr_begin();
    for (;;) {
        cr_sys(r = read(STDIN_FILENO, &b, 1));
        if (r == 0) {
            cr_wait(cr_queue_empty(out));
            cr_exit(1);
        }
        cr_wait(!cr_queue_full(out));
        cr_queue_push(out, b);
    }
    cr_end();
}

cr_proto(socket_write_loop, byte_queue_t *in, int fd)
{
    cr_local uint8_t *b;
    cr_begin();
    for (;;) {
        cr_wait(!cr_queue_empty(in));
        b = cr_queue_pop(in);
        cr_sys(send(fd, b, 1, 0));
    }
    cr_end();
}

cr_proto(socket_read_loop, int fd)
{
    cr_local uint8_t b;
    cr_local int r;
    cr_begin();
    for (;;) {
        cr_sys(r = recv(fd, &b, 1, 0));
        if (r == 0)
            cr_exit(1);
        cr_sys(write(STDOUT_FILENO, &b, 1));
    }
    cr_end();
}

static int nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr = true;
static bool end_attr = false;

cr_context(keyboard_loop);
cr_proto(keyboard_loop)
{
    cr_local char input;
    cr_local int attr_fd;
    cr_local char buf[20];
    cr_begin();

    for (;;) {
        cr_sys(read(STDIN_FILENO, &input, 1));

        attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
        if (attr_fd < 0) {
            perror("open attr_fd");
            continue;
        }

        cr_sys(read(attr_fd, buf, 6));
        buf[6] = '\0';

        switch (input) {
        case 16:  // Ctrl-P
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            written = write(attr_fd, buf, 6);
            if (written != 6) {
                perror("write failed");
                close(attr_fd);
                continue;
            }
            printf("[INFO] Ctrl+P pressed. New read_attr = %d\n", read_attr);
            if (!read_attr)
                printf("Stopping to display the chess board...\n");
            break;
        case 17:  // Ctrl-Q
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            written = write(attr_fd, buf, 6);
            if (written != 6) {
                perror("write failed");
                close(attr_fd);
                continue;
            }
            printf("Stopping the user space tic-tac-toe game...\n");
            break;
        default:
            fprintf(stderr, "[DEBUG] Unhandled input: %d\n", input);
            break;
        }

        close(attr_fd);
        cr_yield;
    }

    cr_end();
}

cr_context(display_loop);
cr_proto(display_loop)
{
    cr_begin();
    for (;;) {
        cr_wait(should_redraw && read_attr);

        printf("\033[H\033[J");
        for (int i = 0; i < 2; i++) {
            printf("=== Game %d ===\n", i + 1);
            draw_table(tables[i].grid);
            printf("\n");
        }
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[9];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
        printf("Time: %s\n", time_str);

        usleep(100000);
        should_redraw = false;
        cr_yield;
    }
    cr_end();
}

cr_context(ai1_loop_0);
cr_context(ai2_loop_0);
cr_context(ai1_loop_1);
cr_context(ai2_loop_1);

cr_proto(ai1_loop_0)
{
    cr_local int move;
    cr_begin();
    while (!finished[0]) {
        cr_wait(turns[0] == 1);
        move = mcts(tables[0].grid, 'O');
        if (move != -1)
            tables[0].grid[move] = 'O';
        if (check_win(tables[0].grid) != ' ')
            finished[0] = true;
        should_redraw = true;
        turns[0] = 2;
        cr_yield;
    }
}

cr_proto(ai2_loop_0)
{
    cr_local int move;
    cr_begin();
    while (!finished[0]) {
        cr_wait(turns[0] == 2);
        move = negamax_predict(tables[0].grid, 'X').move;
        if (move != -1)
            tables[0].grid[move] = 'X';
        if (check_win(tables[0].grid) != ' ')
            finished[0] = true;
        should_redraw = true;
        turns[0] = 1;
        cr_yield;
    }
}

cr_proto(ai1_loop_1)
{
    cr_local int move;
    cr_begin();
    while (!finished[1]) {
        cr_wait(turns[1] == 1);
        move = mcts(tables[1].grid, 'O');
        if (move != -1)
            tables[1].grid[move] = 'O';
        if (check_win(tables[1].grid) != ' ')
            finished[1] = true;
        should_redraw = true;
        turns[1] = 2;
        cr_yield;
    }
}

cr_proto(ai2_loop_1)
{
    cr_local int move;
    cr_begin();
    while (!finished[1]) {
        cr_wait(turns[1] == 2);
        move = negamax_predict(tables[1].grid, 'X').move;
        if (move != -1)
            tables[1].grid[move] = 'X';
        if (check_win(tables[1].grid) != ' ')
            finished[1] = true;
        should_redraw = true;
        turns[1] = 1;
        cr_yield;
    }
}

void reset_game(int i)
{
    memset(tables[i].grid, ' ', UTIL_N_GRIDS);
    turns[i] = 1;
    finished[i] = false;
    should_redraw = true;

    switch (i) {
    case 0:
        cr_context(ai1_loop_0) = cr_context_init();
        cr_context(ai2_loop_0) = cr_context_init();
        break;
    case 1:
        cr_context(ai1_loop_1) = cr_context_init();
        cr_context(ai2_loop_1) = cr_context_init();
        break;
    }

    mcts_init();
    negamax_init();
}

int main(int argc, char *argv[])
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return 1;
    }
    char read_buf[20];
    if (!fgets(read_buf, 20, fp)) {
        perror("fgets failed");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        return 1;
    }

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    if (device_fd < 0) {
        perror("open /dev/kxo");
        return 1;
    }

    cr_context(keyboard_loop) = cr_context_init();
    cr_context(display_loop) = cr_context_init();

    memset(tables[0].grid, ' ', UTIL_N_GRIDS);
    memset(tables[1].grid, ' ', UTIL_N_GRIDS);
    turns[0] = 1;
    turns[1] = 1;

    while (!end_attr) {
        if (!finished[0]) {
            if (turns[0] == 1 && cr_status(ai1_loop_0) != CR_FINISHED)
                cr_run(ai1_loop_0);
            else if (turns[0] == 2 && cr_status(ai2_loop_0) != CR_FINISHED)
                cr_run(ai2_loop_0);
        }

        if (!finished[1]) {
            if (turns[1] == 1 && cr_status(ai1_loop_1) != CR_FINISHED)
                cr_run(ai1_loop_1);
            else if (turns[1] == 2 && cr_status(ai2_loop_1) != CR_FINISHED)
                cr_run(ai2_loop_1);
        }
        cr_run(display_loop);
        cr_run(keyboard_loop);

        if (finished[0]) {
            reset_game(0);
            usleep(50000);
        }

        if (finished[1]) {
            reset_game(1);
            usleep(50000);
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);
    close(device_fd);
    return 0;
}
