CC := gcc
CFLAGS := -Wall
LDLIBS := -lrt
PROGS := $(patsubst %.c,%,$(wildcard *.c))
all: $(PROGS)
$(PROGS): %: %.c
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@
clean:
	-rm -f $(PROGS)
.PHONY: all clean $(PROGS)