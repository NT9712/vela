# Vela — build the toolchain.
#
#   make            build bin/velac, bin/vela and bin/vela-lsp
#   make test       build, then run the whole test suite
#   make bench      build, then run the benchmarks
#   make install    copy the toolchain and library to $(PREFIX)
#   make clean      remove build products

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter \
           -Wno-misleading-indentation -Wno-unused-function \
           -Wno-format-truncation   # snprintf into fixed path buffers is deliberate
PREFIX  ?= /usr/local

SRC     := $(wildcard bootstrap/src/*.c)
OBJ     := $(SRC:.c=.o)
VELAC   := bin/velac
LIBS    := $(wildcard lib/core/*.vela lib/std/*.vela)
TOOLS   := $(wildcard tools/*.vela)

VERSION ?= 1.1.0
ARCH    ?= x86_64
DISTNAME := vela-$(VERSION)-linux-$(ARCH)

.PHONY: all test bench install uninstall clean fmt check dist site site-deploy cross-test

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
	@echo "installed to $(PREFIX)."
	@echo "velac finds the library next to itself, so nothing else is needed."
	@echo "if you move the binaries, set: export VELA_ROOT=$(PREFIX)/lib/vela"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/velac $(DESTDIR)$(PREFIX)/bin/vela \
	      $(DESTDIR)$(PREFIX)/bin/vela-lsp
	rm -rf $(DESTDIR)$(PREFIX)/lib/vela

cross-test: all
	./tests/run.sh cross

site: all
	VELA_ROOT=$(CURDIR) $(VELAC) -q -o site/build site/build.vela
	./site/build .
	@echo
	@du -sh site/public

site-deploy: site
	cd site && vercel deploy --prod --yes

# `make dist` packages for the host; `make dist ARCH=arm64` cross-compiles the
# Vela-written tools with velac and cross-builds velac itself with $(CROSS_CC).
CROSS_CC ?= aarch64-linux-gnu-gcc

dist: all
	rm -rf dist/$(DISTNAME)
	mkdir -p dist/$(DISTNAME)/bin dist/$(DISTNAME)/lib
ifeq ($(ARCH),arm64)
	$(CROSS_CC) $(CFLAGS) -static -o dist/$(DISTNAME)/bin/velac $(SRC)
	VELA_ROOT=$(CURDIR) $(VELAC) -q --target arm64 -o dist/$(DISTNAME)/bin/vela     tools/cli.vela
	VELA_ROOT=$(CURDIR) $(VELAC) -q --target arm64 -o dist/$(DISTNAME)/bin/vela-lsp tools/lsp.vela
else
	cp bin/velac bin/vela bin/vela-lsp        dist/$(DISTNAME)/bin/
endif
	cp -r lib/core lib/std                    dist/$(DISTNAME)/lib/
	cp -r editor docs examples spec           dist/$(DISTNAME)/
	cp README.md LICENSE                      dist/$(DISTNAME)/
	rm -rf dist/$(DISTNAME)/examples/todo/deps dist/$(DISTNAME)/examples/todo/build
	printf '%s\n' \
	  '#!/usr/bin/env sh' \
	  '# install.sh [prefix]   default prefix: /usr/local' \
	  'set -eu' \
	  'P="$${1:-/usr/local}"' \
	  'mkdir -p "$$P/bin" "$$P/lib/vela"' \
	  'cp bin/velac bin/vela bin/vela-lsp "$$P/bin/"' \
	  'cp -r lib/core lib/std "$$P/lib/vela/"' \
	  'echo' \
	  'echo "installed to $$P"' \
	  'echo' \
	  'echo "  export PATH=\"$$P/bin:\$$PATH\""' \
	  'echo "  export VELA_ROOT=\"$$P/lib/vela\""' \
	  > dist/$(DISTNAME)/install.sh
	chmod +x dist/$(DISTNAME)/install.sh
	cd dist && tar czf $(DISTNAME).tar.gz $(DISTNAME)
	@echo
	@ls -lh dist/$(DISTNAME).tar.gz

clean:
	rm -f bootstrap/src/*.o bin/velac bin/vela bin/vela-lsp site/build
	rm -rf build dist site/public examples/todo/build examples/todo/store/build
