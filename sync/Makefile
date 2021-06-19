CC := gcc
CFLAGS := -std=gnu99 -Wall
LDLIBS := -lrt -lpthread
PROGS := $(patsubst %.c,%,$(wildcard *.c))
all: $(PROGS)
$(PROGS): %: %.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
clean:
	-rm -f $(PROGS)
.PHONY: all clean $(PROGS)