# Vela — build the toolchain.
#
#   make            build bin/velac, bin/vela and bin/vela-lsp
#   make test       build, then run the whole test suite
#   make bench      build, then run the benchmarks
#   make install    copy the toolchain and library to $(PREFIX)
#   make clean      remove build products

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter \
           -Wno-misleading-indentation -Wno-unused-function
PREFIX  ?= /usr/local

SRC     := $(wildcard bootstrap/src/*.c)
OBJ     := $(SRC:.c=.o)
VELAC   := bin/velac
LIBS    := $(wildcard lib/core/*.vela lib/std/*.vela)
TOOLS   := $(wildcard tools/*.vela)

.PHONY: all test bench install uninstall clean fmt check

all: $(VELAC) bin/vela bin/vela-lsp

$(VELAC): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJ)

bootstrap/src/%.o: bootstrap/src/%.c bootstrap/src/vela.h
	$(CC) $(CFLAGS) -c $< -o $@

bin/vela: $(VELAC) $(TOOLS) $(LIBS)
	@mkdir -p bin
	VELA_ROOT=$(CURDIR) $(VELAC) -q -o $@ tools/cli.vela

bin/vela-lsp: $(VELAC) $(TOOLS) $(LIBS)
	@mkdir -p bin
	VELA_ROOT=$(CURDIR) $(VELAC) -q -o $@ tools/lsp.vela

test: all
	./tests/run.sh

bench: all
	./bench/run.sh

fmt: all
	VELA_ROOT=$(CURDIR) bin/vela fmt lib/core/*.vela lib/std/*.vela tools/*.vela \
	    examples/*.vela examples/todo/src/*.vela examples/todo/store/src/*.vela \
	    bench/*.vela tests/run/*.vela tests/lib/*.vela

check: all
	VELA_ROOT=$(CURDIR) bin/vela fmt --check lib/core/*.vela lib/std/*.vela tools/*.vela

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m755 bin/velac bin/vela bin/vela-lsp $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/lib/vela
	cp -r lib/core lib/std $(DESTDIR)$(PREFIX)/lib/vela/
	@echo
	@echo "installed. add to your shell profile:"
	@echo "    export VELA_ROOT=$(PREFIX)/lib/vela"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/velac $(DESTDIR)$(PREFIX)/bin/vela \
	      $(DESTDIR)$(PREFIX)/bin/vela-lsp
	rm -rf $(DESTDIR)$(PREFIX)/lib/vela

clean:
	rm -f bootstrap/src/*.o bin/velac bin/vela bin/vela-lsp
	rm -rf build examples/todo/build examples/todo/store/build
