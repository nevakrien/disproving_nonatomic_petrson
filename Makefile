CC   ?= gcc
CFLAGS = -g3 -pthread -march=native -Wall -Wextra

SRC  = peterson_demo.c
BAD  = peterson_volatile
GOOD = peterson_atomic

all: $(BAD) $(GOOD)

$(BAD): $(SRC)
	$(CC) $(CFLAGS) $< -o $@

$(GOOD): $(SRC)
	$(CC) $(CFLAGS) -DSTRICT_ATOMICS $< -o $@

clean:
	rm -f $(BAD) $(GOOD)