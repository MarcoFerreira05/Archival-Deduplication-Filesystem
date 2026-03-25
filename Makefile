CC = gcc

CFLAGS = -Wall -Iincludes `pkg-config --cflags fuse3 glib-2.0`
LIBS = `pkg-config --libs fuse3 glib-2.0` -lpthread -lbz2

SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c, build/%.o, $(SRC))

all: passthrough

build/%.o: src/%.c
	@mkdir -p build
	$(CC) -c $< -o $@ $(CFLAGS)

passthrough: $(OBJ)
	$(CC) $^ -o $@ $(LIBS)

.PHONY: clean

clean:
	rm -rf build passthrough
