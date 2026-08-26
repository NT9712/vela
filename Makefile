CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter \
           -Wno-misleading-indentation -Wno-unused-function
SRC     := $(wildcard bootstrap/src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := bin/velac

.PHONY: all clean test install

all: $(BIN) bin/vela

$(BIN): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJ)

bootstrap/src/%.o: bootstrap/src/%.c bootstrap/src/vela.h
	$(CC) $(CFLAGS) -c $< -o $@

bin/vela: $(BIN) tools/cli.vela $(wildcard lib/std/*.vela lib/core/*.vela)
	@mkdir -p bin
	VELA_ROOT=$(CURDIR) $(BIN) -o bin/vela tools/cli.vela

clean:
	rm -f bootstrap/src/*.o bin/velac bin/vela

test: all
	./tests/run.sh
