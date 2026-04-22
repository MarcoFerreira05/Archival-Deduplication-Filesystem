CC = gcc

CFLAGS  = -Wall -Iincludes -D_GNU_SOURCE -g
CFLAGS += $(shell pkg-config --cflags fuse3 glib-2.0)
LIBS   = $(shell pkg-config --libs fuse3 glib-2.0) -lpthread -lbz2 -lcrypto
SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c, build/%.o, $(SRC))

all: passthrough

build/%.o: src/%.c
	@mkdir -p build
	$(CC) -c $< -o $@ $(CFLAGS)

passthrough: $(OBJ)
	@$(CC) $^ -o $@ $(LIBS)

.PHONY: clean

clean:
	rm -rf build passthrough
