TARGET := kxo
obj-m := $(TARGET).o

CCFLAGS := -std=gnu99 -Wno-declaration-after-statement
CFLAGS := -std=gnu99 -Wall -O2

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

GIT_HOOKS := .git/hooks/applied

all: kmod xo-user

kmod: $(GIT_HOOKS) main.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

xo-user: xo-user.c game_util.c ai_mcts.c ai_negamax.c user_xoroshiro.c user_zobrist.c
	$(CC) $(CFLAGS) -o $@ $^

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) xo-user