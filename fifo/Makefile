CC=gcc
CFLAGS= -std=gnu99 -Wall
PROGS := $(patsubst %.c,%,$(wildcard *.c))
all: $(PROGS)
$(PROGS): %: %.c
	$(CC) $(CFLAGS) $< -o $@
clean:
	-rm -f $(PROGS)
.PHONY: all clean $(PROGS)